"""Demo opcional: renderiza un frame de exp12 con wgpu y lo compara con el CPU.

No forma parte del bucle principal. Uso:
    .venv\\Scripts\\python.exe wgpu_frame.py [numero_frame]
"""
from __future__ import annotations

import json
import sys
import time
from array import array
from pathlib import Path

import wgpu
from PIL import Image, ImageChops

import runtime as rt

HERE = Path(__file__).resolve().parent

WGSL = """
struct VSOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) color: vec4<f32>,
};
@vertex
fn vs_main(@location(0) pos: vec2<f32>, @location(1) color: vec4<f32>) -> VSOut {
    var out: VSOut;
    out.pos = vec4<f32>(pos, 0.0, 1.0);
    out.color = color;
    return out;
}
@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
    return in.color;
}
"""


def vertices(op: dict, width: int, height: int) -> list[float]:
    x, y, w, h = op["x"], op["y"], op["w"], op["h"]
    r, g, b = (c / 255.0 for c in rt.rgb255(op["fill"]))
    x0, x1 = x / width * 2 - 1, (x + w) / width * 2 - 1
    y0, y1 = 1 - y / height * 2, 1 - (y + h) / height * 2
    out: list[float] = []
    for px, py in ((x0, y0), (x0, y1), (x1, y1), (x0, y0), (x1, y1), (x1, y0)):
        out.extend((px, py, r, g, b, 1.0))
    return out


def render(doc: dict) -> tuple[Image.Image, dict]:
    width, height = int(doc["viewport"]["width"]), int(doc["viewport"]["height"])
    t0 = time.perf_counter()
    adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
    info = adapter.info
    device = adapter.request_device_sync()
    t_device = time.perf_counter()

    fmt = wgpu.TextureFormat.rgba8unorm
    texture = device.create_texture(size=(width, height, 1), format=fmt,
                                    usage=wgpu.TextureUsage.RENDER_ATTACHMENT
                                    | wgpu.TextureUsage.COPY_SRC)
    clear = tuple(c / 255.0 for c in rt.rgb255(doc["ops"][0]["fill"])) + (1.0,)
    verts: list[float] = []
    for op in doc["ops"][1:]:
        verts.extend(vertices(op, width, height))
    vbuf = device.create_buffer_with_data(data=array("f", verts),
                                          usage=wgpu.BufferUsage.VERTEX)
    shader = device.create_shader_module(code=WGSL)
    pipeline = device.create_render_pipeline(
        layout=wgpu.AutoLayoutMode.auto,
        vertex={"module": shader, "entry_point": "vs_main", "buffers": [{
            "array_stride": 24, "step_mode": wgpu.VertexStepMode.vertex,
            "attributes": [
                {"format": wgpu.VertexFormat.float32x2, "offset": 0, "shader_location": 0},
                {"format": wgpu.VertexFormat.float32x4, "offset": 8, "shader_location": 1},
            ]}]},
        primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
        fragment={"module": shader, "entry_point": "fs_main",
                  "targets": [{"format": fmt}]},
    )
    encoder = device.create_command_encoder()
    rpass = encoder.begin_render_pass(color_attachments=[{
        "view": texture.create_view(), "clear_value": clear,
        "load_op": wgpu.LoadOp.clear, "store_op": wgpu.StoreOp.store}])
    rpass.set_pipeline(pipeline)
    rpass.set_vertex_buffer(0, vbuf)
    rpass.draw(vertex_count=len(verts) // 6)
    rpass.end()

    row = ((width * 4 + 255) // 256) * 256
    readback = device.create_buffer(size=row * height,
                                    usage=wgpu.BufferUsage.COPY_DST
                                    | wgpu.BufferUsage.MAP_READ)
    encoder.copy_texture_to_buffer(
        {"texture": texture, "mip_level": 0, "origin": (0, 0, 0)},
        {"buffer": readback, "offset": 0, "bytes_per_row": row, "rows_per_image": height},
        (width, height, 1))
    device.queue.submit([encoder.finish()])
    readback.map_sync(wgpu.MapMode.READ)
    data = bytes(readback.read_mapped())
    readback.unmap()
    rows = [data[i * row:i * row + width * 4] for i in range(height)]
    image = Image.frombytes("RGBA", (width, height), b"".join(rows))
    elapsed = (time.perf_counter() - t0) * 1000
    meta = {"backend": info["backend_type"], "adapter": info["device"],
            "adapter_type": info["adapter_type"], "gpu_total_ms": round(elapsed, 2),
            "device_init_ms": round((t_device - t0) * 1000, 2)}
    return image, meta


def main() -> int:
    number = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    entry = next(item for item in json.loads(
        (HERE / "state_log.json").read_text(encoding="utf-8"))["frames"]
        if item["frame"] == number)
    doc = json.loads((HERE / entry["display_list"]).read_text(encoding="utf-8"))
    image, meta = render(doc)
    out = HERE / "frames" / f"frame_{number:03d}_wgpu.png"
    image.save(out)
    cpu = Image.open(HERE / entry["png"]).convert("RGBA")
    diff = ImageChops.difference(cpu, image)
    maxdiff = max(diff.tobytes()) if diff.tobytes() else 0
    report = {"ok": maxdiff == 0, "frame": number, "cpu_png": entry["png"],
              "wgpu_png": out.relative_to(HERE).as_posix(), "maxdiff": maxdiff, **meta}
    (HERE / "wgpu_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if maxdiff == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
