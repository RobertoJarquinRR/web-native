"""Backend wgpu (D3D12/Vulkan/Metal): 2 pipelines, 2 draw calls.

Rects: triángulos opacos con color por vértice.
Glifos: quads texturizados desde un atlas R8; el canal R es el alpha y el color
viene por vértice. Se mantiene el contrato de exp06.
"""
from array import array

import wgpu
from PIL import Image

RECT_WGSL = """
struct VSOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec4<f32> };
@vertex fn vs_main(@location(0) pos: vec2<f32>, @location(1) color: vec4<f32>) -> VSOut {
    return VSOut(vec4<f32>(pos, 0.0, 1.0), color); }
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> { return in.color; }
"""
GLYPH_WGSL = """
struct VSOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32>,
               @location(1) color: vec4<f32> };
@vertex fn vs_main(@location(0) pos: vec2<f32>, @location(1) uv: vec2<f32>,
                   @location(2) color: vec4<f32>) -> VSOut {
    return VSOut(vec4<f32>(pos, 0.0, 1.0), uv, color); }
@group(0) @binding(0) var atlas_tex: texture_2d<f32>;
@group(0) @binding(1) var atlas_sampler: sampler;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
    let a = textureSample(atlas_tex, atlas_sampler, in.uv).r;
    return vec4<f32>(in.color.rgb, in.color.a * a); }
"""


def _rgb(color: str) -> tuple[int, int, int]:
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def to_ndc(x, y, w, h, width, height):
    return (x / width * 2 - 1, 1 - y / height * 2,
            (x + w) / width * 2 - 1, 1 - (y + h) / height * 2)


def rect_vertices(rect, width, height):
    x0, y0, x1, y1 = to_ndc(rect["x"], rect["y"], rect["w"], rect["h"], width, height)
    r, g, b = (c / 255.0 for c in _rgb(rect["fill"]))
    out = []
    for px, py in ((x0, y0), (x0, y1), (x1, y1),
                   (x0, y0), (x1, y1), (x1, y0)):
        out.extend((px, py, r, g, b, 1.0))
    return out


def glyph_vertices(op, width, height):
    r, g, b = (c / 255.0 for c in _rgb(op["color"]))
    out = []
    for glyph in op["glyphs"]:
        if glyph["w"] == 0 or glyph["h"] == 0:
            continue
        x0, y0, x1, y1 = to_ndc(glyph["x"], glyph["y"], glyph["w"], glyph["h"],
                                width, height)
        u0, v0, u1, v1 = (glyph[k] for k in ("u0", "v0", "u1", "v1"))
        for px, py, u, v in ((x0, y0, u0, v0), (x0, y1, u0, v1),
                             (x1, y1, u1, v1), (x0, y0, u0, v0),
                             (x1, y1, u1, v1), (x1, y0, u1, v0)):
            out.extend((px, py, u, v, r, g, b, 1.0))
    return out


class WgpuRenderer:
    def __init__(self, viewport: tuple[int, int], atlas_size: int = 1024):
        self.width, self.height = viewport
        adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if adapter is None:
            raise RuntimeError("no wgpu adapter available")
        info = adapter.info
        self.info = {
            "device": info["device"], "backend": info["backend_type"],
            "adapter_type": info["adapter_type"], "vendor_id": info["vendor_id"],
        }
        self.device = adapter.request_device_sync()
        self.format = wgpu.TextureFormat.rgba8unorm
        self.atlas_size = atlas_size

        rect_mod = self.device.create_shader_module(code=RECT_WGSL)
        self.rect_pipe = self.device.create_render_pipeline(
            layout=wgpu.AutoLayoutMode.auto,
            vertex={"module": rect_mod, "entry_point": "vs_main", "buffers": [{
                "array_stride": 24, "step_mode": wgpu.VertexStepMode.vertex,
                "attributes": [
                    {"format": wgpu.VertexFormat.float32x2, "offset": 0,
                     "shader_location": 0},
                    {"format": wgpu.VertexFormat.float32x4, "offset": 8,
                     "shader_location": 1}]}]},
            primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
            fragment={"module": rect_mod, "entry_point": "fs_main",
                      "targets": [{"format": self.format}]})

        def blend(src, dst):
            return {"src_factor": src, "dst_factor": dst,
                    "operation": wgpu.BlendOperation.add}

        glyph_mod = self.device.create_shader_module(code=GLYPH_WGSL)
        self.glyph_pipe = self.device.create_render_pipeline(
            layout=wgpu.AutoLayoutMode.auto,
            vertex={"module": glyph_mod, "entry_point": "vs_main", "buffers": [{
                "array_stride": 32, "step_mode": wgpu.VertexStepMode.vertex,
                "attributes": [
                    {"format": wgpu.VertexFormat.float32x2, "offset": 0,
                     "shader_location": 0},
                    {"format": wgpu.VertexFormat.float32x2, "offset": 8,
                     "shader_location": 1},
                    {"format": wgpu.VertexFormat.float32x4, "offset": 16,
                     "shader_location": 2}]}]},
            primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
            fragment={"module": glyph_mod, "entry_point": "fs_main",
                      "targets": [{"format": self.format, "blend": {
                          "color": blend(wgpu.BlendFactor.src_alpha,
                                         wgpu.BlendFactor.one_minus_src_alpha),
                          "alpha": blend(wgpu.BlendFactor.one,
                                         wgpu.BlendFactor.one_minus_src_alpha)}}]})

        atlas_tex = self.device.create_texture(
            size=(atlas_size, atlas_size, 1), format=wgpu.TextureFormat.r8unorm,
            usage=wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST)
        # La textura se rellena en render() con el atlas real.
        self._atlas_tex = atlas_tex
        sampler = self.device.create_sampler(
            mag_filter=wgpu.FilterMode.linear, min_filter=wgpu.FilterMode.linear,
            address_mode_u=wgpu.AddressMode.clamp_to_edge,
            address_mode_v=wgpu.AddressMode.clamp_to_edge)
        self.bind_group = self.device.create_bind_group(
            layout=self.glyph_pipe.get_bind_group_layout(0),
            entries=[{"binding": 0, "resource": atlas_tex.create_view()},
                     {"binding": 1, "resource": sampler}])

    def render(self, ops: list[dict], atlas, clear_rgb: tuple[int, int, int]) -> Image.Image:
        device, width, height = self.device, self.width, self.height
        device.queue.write_texture(
            {"texture": self._atlas_tex, "mip_level": 0, "origin": (0, 0, 0)},
            bytes(atlas.data), {"offset": 0, "bytes_per_row": self.atlas_size,
                                "rows_per_image": self.atlas_size},
            (self.atlas_size, self.atlas_size, 1))

        target = device.create_texture(
            size=(width, height, 1), format=self.format,
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC)
        rect_verts, glyph_verts = [], []
        for op in ops:
            if op["type"] == "rect":
                rect_verts.extend(rect_vertices(op, width, height))
            elif op["type"] == "text_run":
                glyph_verts.extend(glyph_vertices(op, width, height))
        rect_buf = device.create_buffer_with_data(
            data=array("f", rect_verts), usage=wgpu.BufferUsage.VERTEX) \
            if rect_verts else None
        glyph_buf = device.create_buffer_with_data(
            data=array("f", glyph_verts), usage=wgpu.BufferUsage.VERTEX) \
            if glyph_verts else None

        encoder = device.create_command_encoder()
        rpass = encoder.begin_render_pass(color_attachments=[{
            "view": target.create_view(),
            "clear_value": tuple(c / 255.0 for c in clear_rgb) + (1.0,),
            "load_op": wgpu.LoadOp.clear, "store_op": wgpu.StoreOp.store}])
        if rect_buf is not None:
            rpass.set_pipeline(self.rect_pipe)
            rpass.set_vertex_buffer(0, rect_buf)
            rpass.draw(vertex_count=len(rect_verts) // 6)
        if glyph_buf is not None:
            rpass.set_pipeline(self.glyph_pipe)
            rpass.set_bind_group(0, self.bind_group)
            rpass.set_vertex_buffer(0, glyph_buf)
            rpass.draw(vertex_count=len(glyph_verts) // 8)
        rpass.end()

        bytes_per_row = ((width * 4 + 255) // 256) * 256
        readback = device.create_buffer(
            size=bytes_per_row * height,
            usage=wgpu.BufferUsage.COPY_DST | wgpu.BufferUsage.MAP_READ)
        encoder.copy_texture_to_buffer(
            {"texture": target, "mip_level": 0, "origin": (0, 0, 0)},
            {"buffer": readback, "offset": 0, "bytes_per_row": bytes_per_row,
             "rows_per_image": height}, (width, height, 1))
        device.queue.submit([encoder.finish()])
        readback.map_sync(wgpu.MapMode.READ)
        data = bytes(readback.read_mapped())
        readback.unmap()
        rows = [data[i * bytes_per_row:i * bytes_per_row + width * 4]
                for i in range(height)]
        return Image.frombytes("RGBA", (width, height), b"".join(rows))
