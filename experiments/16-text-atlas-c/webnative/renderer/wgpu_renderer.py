"""Renderer offscreen con wgpu-py (wgpu-native: D3D12 / Vulkan / Metal).

Dos pipelines en un solo render pass, en orden de display list:
  1. rect: triángulos opacos (pos + color).
  2. glyph: quads texturizados con alfa desde un atlas r8unorm (pos + uv + color).
El atlas se reconstruye desde los `TextOp` (misma fuente, mismos gids/tamaños) y
se cachea por documento en la instancia. Device, pipelines y sampler se crean
una sola vez con `ensure_ready()`.
"""
from array import array

import wgpu
from PIL import Image

from .. import text as textmod
from ..ir import DisplayList, RectOp, TextOp
from . import rgb01

RECT_WGSL = """
struct VSOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec4<f32> };
@vertex fn vs_main(@location(0) pos: vec2<f32>, @location(1) color: vec4<f32>) -> VSOut {
    var out: VSOut; out.pos = vec4<f32>(pos, 0.0, 1.0); out.color = color; return out;
}
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> { return in.color; }
"""

GLYPH_WGSL = """
struct VSOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32>,
               @location(1) color: vec4<f32> };
@vertex fn vs_main(@location(0) pos: vec2<f32>, @location(1) uv: vec2<f32>,
                   @location(2) color: vec4<f32>) -> VSOut {
    var out: VSOut; out.pos = vec4<f32>(pos, 0.0, 1.0); out.uv = uv; out.color = color;
    return out;
}
@group(0) @binding(0) var atlas_tex: texture_2d<f32>;
@group(0) @binding(1) var atlas_sampler: sampler;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
    let a = textureSample(atlas_tex, atlas_sampler, in.uv).r;
    return vec4<f32>(in.color.rgb, in.color.a * a);
}
"""


def rect_vertices(op: RectOp, width: int, height: int) -> list[float]:
    x, y, w, h = round(op.x), round(op.y), round(op.w), round(op.h)
    r, g, b = rgb01(op.fill)
    x0, x1 = x / width * 2 - 1, (x + w) / width * 2 - 1
    y0, y1 = 1 - y / height * 2, 1 - (y + h) / height * 2
    vertices: list[float] = []
    for vx, vy in ((x0, y0), (x0, y1), (x1, y1), (x0, y0), (x1, y1), (x1, y0)):
        vertices.extend((vx, vy, r, g, b, 1.0))
    return vertices


def glyph_vertices(op: TextOp, atlas: "textmod.GlyphAtlas",
                   engine: "textmod.TextEngine",
                   width: int, height: int) -> list[float]:
    """Quads de glyph con UV del atlas recalculado (no se confía en las del IR)."""
    face = engine.face(op.font, op.font_file or None)
    r, g, b = rgb01(op.color)
    size_key = int(round(op.size))
    vertices: list[float] = []
    for glyph in op.glyphs:
        key = (str(face.path), glyph.gid, size_key)
        rect = atlas.rects.get(key)
        if not rect:
            continue
        gw, gh = rect[2], rect[3]
        if gw == 0 or gh == 0:
            continue
        x0, x1 = glyph.x / width * 2 - 1, (glyph.x + gw) / width * 2 - 1
        y0, y1 = 1 - glyph.y / height * 2, 1 - (glyph.y + gh) / height * 2
        u0, v0, u1, v1 = atlas.uv(key)
        for vx, vy, u, v in ((x0, y0, u0, v0), (x0, y1, u0, v1),
                             (x1, y1, u1, v1), (x0, y0, u0, v0),
                             (x1, y1, u1, v1), (x1, y0, u1, v0)):
            vertices.extend((vx, vy, u, v, r, g, b, 1.0))
    return vertices


class WgpuRenderer:
    name = "wgpu"

    def __init__(self) -> None:
        self._device = None
        self._rect_pipeline = None
        self._glyph_pipeline = None
        self._sampler = None
        self._engine = textmod.TextEngine()
        self._atlas = None
        self._atlas_keys: list[tuple[str, int, int]] | None = None
        self._atlas_texture = None
        self._atlas_view = None
        self._bind_group = None
        self.last_frame: dict[str, int] = {}
        self.backend_info = "wgpu (se inicializa en el primer frame)"

    def ensure_ready(self) -> "WgpuRenderer":
        """Crea adapter/device/pipelines una sola vez; idempotente."""
        if self._device is not None:
            return self
        adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if adapter is None:
            raise RuntimeError("wgpu: no hay adapter disponible")
        info = dict(adapter.info)
        self.backend_info = (f"{info.get('device', '?')} / "
                             f"{info.get('backend_type', '?')} / "
                             f"{info.get('adapter_type', '?')}")
        self._device = adapter.request_device_sync()

        rect_module = self._device.create_shader_module(code=RECT_WGSL)
        self._rect_pipeline = self._device.create_render_pipeline(
            layout=wgpu.AutoLayoutMode.auto,
            vertex={"module": rect_module, "entry_point": "vs_main", "buffers": [{
                "array_stride": 24, "step_mode": wgpu.VertexStepMode.vertex,
                "attributes": [
                    {"format": wgpu.VertexFormat.float32x2, "offset": 0, "shader_location": 0},
                    {"format": wgpu.VertexFormat.float32x4, "offset": 8, "shader_location": 1},
                ]}]},
            primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
            fragment={"module": rect_module, "entry_point": "fs_main",
                      "targets": [{"format": wgpu.TextureFormat.rgba8unorm}]})

        glyph_module = self._device.create_shader_module(code=GLYPH_WGSL)

        def blend(src, dst):
            return {"src_factor": src, "dst_factor": dst,
                    "operation": wgpu.BlendOperation.add}

        alpha_blend = {
            "color": blend(wgpu.BlendFactor.src_alpha, wgpu.BlendFactor.one_minus_src_alpha),
            "alpha": blend(wgpu.BlendFactor.one, wgpu.BlendFactor.one_minus_src_alpha),
        }
        self._glyph_pipeline = self._device.create_render_pipeline(
            layout=wgpu.AutoLayoutMode.auto,
            vertex={"module": glyph_module, "entry_point": "vs_main", "buffers": [{
                "array_stride": 32, "step_mode": wgpu.VertexStepMode.vertex,
                "attributes": [
                    {"format": wgpu.VertexFormat.float32x2, "offset": 0, "shader_location": 0},
                    {"format": wgpu.VertexFormat.float32x2, "offset": 8, "shader_location": 1},
                    {"format": wgpu.VertexFormat.float32x4, "offset": 16, "shader_location": 2},
                ]}]},
            primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
            fragment={"module": glyph_module, "entry_point": "fs_main",
                      "targets": [{"format": wgpu.TextureFormat.rgba8unorm,
                                   "blend": alpha_blend}]})
        self._sampler = self._device.create_sampler(
            mag_filter=wgpu.FilterMode.linear, min_filter=wgpu.FilterMode.linear,
            address_mode_u=wgpu.AddressMode.clamp_to_edge,
            address_mode_v=wgpu.AddressMode.clamp_to_edge)
        return self

    def _ensure_atlas(self, ops) -> "textmod.GlyphAtlas | None":
        keys = textmod.text_op_glyph_keys(ops, self._engine)
        if not keys:
            return None
        if keys != self._atlas_keys:
            self._atlas = textmod.build_atlas(self._engine, keys)
            self._atlas_keys = keys
            self._atlas_texture = None
            self._atlas_view = None
            self._bind_group = None
        return self._atlas

    def _ensure_atlas_texture(self) -> None:
        if self._atlas_texture is not None:
            return
        size = self._atlas.size
        texture = self._device.create_texture(
            size=(size, size, 1), format=wgpu.TextureFormat.r8unorm,
            usage=wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST)
        self._device.queue.write_texture(
            {"texture": texture, "mip_level": 0, "origin": (0, 0, 0)},
            bytes(self._atlas.data),
            {"offset": 0, "bytes_per_row": size, "rows_per_image": size},
            (size, size, 1))
        self._atlas_texture = texture
        self._atlas_view = texture.create_view()
        self._bind_group = self._device.create_bind_group(
            layout=self._glyph_pipeline.get_bind_group_layout(0),
            entries=[{"binding": 0, "resource": self._atlas_view},
                     {"binding": 1, "resource": self._sampler}])

    def render(self, display_list: DisplayList, width: int | None = None,
               height: int | None = None) -> Image.Image:
        self.ensure_ready()
        w = display_list.width if width is None else int(width)
        h = display_list.height if height is None else int(height)

        atlas = self._ensure_atlas(display_list.ops)
        if atlas is not None:
            self._ensure_atlas_texture()

        # Batches contiguos del mismo tipo conservan el painter's order del IR.
        rect_data = array("f")
        glyph_data = array("f")
        commands: list[tuple[str, int, int]] = []
        glyph_count = text_count = rect_count = 0
        for op in display_list.ops:
            if isinstance(op, RectOp):
                start = len(rect_data) // 6
                rect_data.extend(rect_vertices(op, w, h))
                commands.append(("rect", start, len(rect_data) // 6 - start))
                rect_count += 1
            elif isinstance(op, TextOp):
                start = len(glyph_data) // 8
                glyph_data.extend(glyph_vertices(op, atlas, self._engine, w, h))
                count = len(glyph_data) // 8 - start
                if count:
                    commands.append(("glyph", start, count))
                text_count += 1
                glyph_count += count
            else:
                raise NotImplementedError(f"wgpu: op no soportada: {type(op).__name__}")

        device = self._device
        texture = device.create_texture(
            size=(w, h, 1), format=wgpu.TextureFormat.rgba8unorm,
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC)
        rect_buffer = (device.create_buffer_with_data(
            data=rect_data, usage=wgpu.BufferUsage.VERTEX) if rect_data else None)
        glyph_buffer = (device.create_buffer_with_data(
            data=glyph_data, usage=wgpu.BufferUsage.VERTEX) if glyph_data else None)

        encoder = device.create_command_encoder()
        render_pass = encoder.begin_render_pass(color_attachments=[{
            "view": texture.create_view(), "clear_value": (0.0, 0.0, 0.0, 1.0),
            "load_op": wgpu.LoadOp.clear, "store_op": wgpu.StoreOp.store}])
        for kind, first, count in commands:
            if kind == "rect":
                render_pass.set_pipeline(self._rect_pipeline)
                render_pass.set_vertex_buffer(0, rect_buffer)
            else:
                render_pass.set_pipeline(self._glyph_pipeline)
                render_pass.set_bind_group(0, self._bind_group)
                render_pass.set_vertex_buffer(0, glyph_buffer)
            render_pass.draw(vertex_count=count, instance_count=1, first_vertex=first)
        render_pass.end()

        bytes_per_row = ((w * 4 + 255) // 256) * 256  # alineación exigida por wgpu
        readback = device.create_buffer(
            size=bytes_per_row * h,
            usage=wgpu.BufferUsage.COPY_DST | wgpu.BufferUsage.MAP_READ)
        encoder.copy_texture_to_buffer(
            {"texture": texture, "mip_level": 0, "origin": (0, 0, 0)},
            {"buffer": readback, "offset": 0, "bytes_per_row": bytes_per_row,
             "rows_per_image": h}, (w, h, 1))
        device.queue.submit([encoder.finish()])
        readback.map_sync(wgpu.MapMode.READ)
        data = bytes(readback.read_mapped())
        readback.unmap()
        rows = [data[row * bytes_per_row:row * bytes_per_row + w * 4] for row in range(h)]
        self.last_frame = {
            "rect_ops": rect_count, "text_runs": text_count, "glyphs": glyph_count,
            "atlas_size": atlas.size if atlas is not None else 0,
            "atlas_glyphs": atlas.glyph_count if atlas is not None else 0,
        }
        return Image.frombytes("RGBA", (w, h), b"".join(rows)).convert("RGB")
