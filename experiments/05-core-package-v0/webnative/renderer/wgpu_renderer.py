"""Renderer offscreen con wgpu-py (wgpu-native: D3D12 / Vulkan / Metal).

Un pipeline de triángulos y un draw call por frame: todas las ops rect van a un
vertex buffer. Device/pipeline se cachean en la instancia (~2 s solo la primera vez).
"""
from array import array

import wgpu
from PIL import Image

from ..ir import DisplayList, RectOp
from . import rgb01

WGSL = """
struct VSOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec4<f32> };
@vertex fn vs_main(@location(0) pos: vec2<f32>, @location(1) color: vec4<f32>) -> VSOut {
    var out: VSOut; out.pos = vec4<f32>(pos, 0.0, 1.0); out.color = color; return out;
}
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> { return in.color; }
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


class WgpuRenderer:
    name = "wgpu"

    def __init__(self) -> None:
        self._device = None
        self._pipeline = None
        self.backend_info = "wgpu (se inicializa en el primer frame)"

    def ensure_ready(self) -> "WgpuRenderer":
        """Crea adapter/device/pipeline una sola vez; idempotente."""
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
        shader = self._device.create_shader_module(code=WGSL)
        self._pipeline = self._device.create_render_pipeline(
            layout=wgpu.AutoLayoutMode.auto,
            vertex={"module": shader, "entry_point": "vs_main", "buffers": [{
                "array_stride": 24, "step_mode": wgpu.VertexStepMode.vertex,
                "attributes": [
                    {"format": wgpu.VertexFormat.float32x2, "offset": 0, "shader_location": 0},
                    {"format": wgpu.VertexFormat.float32x4, "offset": 8, "shader_location": 1},
                ]}]},
            primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
            fragment={"module": shader, "entry_point": "fs_main",
                      "targets": [{"format": wgpu.TextureFormat.rgba8unorm}]})
        return self

    def render(self, display_list: DisplayList, width: int | None = None,
               height: int | None = None) -> Image.Image:
        self.ensure_ready()
        w = display_list.width if width is None else int(width)
        h = display_list.height if height is None else int(height)
        vertices: list[float] = []
        for op in display_list.ops:
            if not isinstance(op, RectOp):
                raise NotImplementedError(f"wgpu: op no soportada: {type(op).__name__}")
            vertices.extend(rect_vertices(op, w, h))
        device, fmt = self._device, wgpu.TextureFormat.rgba8unorm
        texture = device.create_texture(
            size=(w, h, 1), format=fmt,
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC)
        encoder = device.create_command_encoder()
        render_pass = encoder.begin_render_pass(color_attachments=[{
            "view": texture.create_view(), "clear_value": (0.0, 0.0, 0.0, 1.0),
            "load_op": wgpu.LoadOp.clear, "store_op": wgpu.StoreOp.store}])
        if vertices:
            vbuf = device.create_buffer_with_data(data=array("f", vertices),
                                                  usage=wgpu.BufferUsage.VERTEX)
            render_pass.set_pipeline(self._pipeline)
            render_pass.set_vertex_buffer(0, vbuf)
            render_pass.draw(vertex_count=len(vertices) // 6)
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
        rows = [data[r * bytes_per_row:r * bytes_per_row + w * 4] for r in range(h)]
        return Image.frombytes("RGBA", (w, h), b"".join(rows)).convert("RGB")
