# Media roadmap after E4

> **Status:** E3a (video element, blob v7, media ABI) and E4 (inline session,
> `media_test`, RGBA sink) are merged. This document is the execution plan for
> the remaining media work. `docs/media-v1.md` remains the contract; this file
> only sequences it.
>
> **Rule:** a wave is done only with build, tests, real evidence (frame PNG or
> app screenshot), and review. "Compiles" is not done. No fake playback.

## 1. Baseline today

| Area | State |
|---|---|
| `<video>` element | kind 6, optional media section, source-less host-bound nodes |
| Blob | v7, single reader, no v6 compatibility |
| ABI | 8; provider ABI 1; no public change in E4 |
| Session | inline, deterministic clock, full state machine, events |
| Providers | `media_null` (always), `media_test` (tests only) |
| Frames | RGBA8 upload + `wn_video_batch` on D3D11; wgpu has no video sink |
| Audio | none |
| Subtitles | none |
| App player | none; `PlaybackCoordinator` + external fallback only |

What is missing is exactly the real decode/audio/subtitle/UI stack, not the
element or the session contract.

## 2. Wave sequence

Estimates are **agent wall-clock** with two implementation agents in parallel
plus one reviewer, including tests and evidence. They are not human-days and
they are not promises.

| Wave | Scope | Depends on | Wall-clock | Done when |
|---|---|---|---:|---|
| **E5a** | Worker thread, queues, Media Foundation video provider, RGB32 frames | E4 | 1.5–3 d | H.264 MP4/MKV plays, seeks and loops in the offscreen harness |
| **A11** | `<video>` in the gallery document, controls, coordinator binding, states | E4; can run with E5a | 1–2 d | App shows a real player; without a provider it reports unavailable |
| **E5b** | WASAPI shared audio, resampler, audio master clock, device change | E5a | 1.5–3 d | Video + audio stay within 20 ms over 60 s |
| **E5c** | NV12/P010 planes, YUV shaders, D3D11VA import, rotation | E5a | 1.5–3 d | 1080p NV12 frame upload < 2 ms CPU; 10-bit path verified |
| **E6** | Optional FFmpeg LGPL provider, allowlist load, packaging | E5a/E5c | 3–6 d | MKV/HEVC/AV1 fixture matrix passes with the plugin, and the app still starts without it |
| **E7** | SRT/VTT parsing, timing, overlay, seek correctness | E5b | 0.5–1 d | Subtitles follow seek within one frame |
| **E7b** | ASS/SSA via libass, PGS bitmap overlay | E7 | 2–4 d | Styled and bitmap subtitles render correctly |
| **E8** | wgpu RGBA upload, then native import; HDR/tonemap | E5c | 2–5 d | D3D11/wgpu color fixture parity; HDR reference |

**MVP:** E5a + A11 + E5b → **4–7 calendar days** for a real player with audio.
**Full:** E5a–E8 → **3–5 weeks**.

Parallelism is limited by shared hotspots: one agent owns `media_core.c` and
the renderer at a time; the app can work in parallel because it is another
repository.

## 3. E5a — first real video

### Scope
- Per-session worker thread with the queues and ownership rules from
  `media-v1.md` §3.6: packets (32), decoded video (3), audio (200 ms), events
  (64), serial-based flush.
- `WN_MEDIA_THREADS=ON` becomes the production path; inline mode stays for
  deterministic tests.
- `media_mf` provider: probe/open/streams/read/decode/receive_video/seek/close.
- Request **RGB32** output first. Reuse the E4 RGBA sink; do not block on YUV.
- Video-only: no audio sink yet. The master clock is the wall clock until E5b.
- Buffer underrun → `BUFFERING`; seek flushes and re-anchors; loop restarts at 0.

### Out of scope
- HEVC without the Windows codec pack, MKV if the OS splitter is absent.
- Hardware zero-copy, NV12, HDR.
- Audio.

### Gates
- A real H.264 MP4 and a supported MKV play for at least 60 s offscreen.
- `seek(0.5 * duration)` lands on the expected frame.
- Pause freezes the clock and the frame; resume does not jump.
- No UI stall longer than 16 ms during decode.
- `media_test` and `media_null` suites still pass.

## 4. A11 — player in the app

### Scope
- Add a real `<video>` node (no `src`) to `app/index.html`, with a poster and a
  host-set source.
- Player overlay: title/episode context, timeline, play/pause, ±10 s, volume,
  mute, audio/subtitle menus, buffering/error/ended states, close/back.
- `PlaybackCoordinator` calls `wn_node_media_*`; events update the UI.
- `IPlaybackService` implementation over the engine; `ExternalPlayback` remains
  the fallback when no provider is registered.
- `PositionTracked=false` is shown as "posición no disponible", never as 0 %.
- Keyboard: space play/pause, arrows seek, Esc close.

### Out of scope
- PiP, playback rate, styled subtitles, chapter thumbnails.
- A second video surface or image-node fake.

### Gates
- The app opens an item, plays real video, seeks, pauses, resumes and closes.
- Without a provider: poster + explicit unavailable message + "Abrir con el
  sistema"; no fake controls.
- Screenshots at 1280×900 of poster, playing, paused, buffering, error.
- 242+ .NET tests and desktop/CLI selftests pass.

## 5. E5b — audio and A/V sync

- WASAPI shared mode, ~10 ms buffer, linear resampler.
- Audio sink is the master clock when audio exists; wall clock otherwise.
- Underrun policy, device change, `muted`, volume, video-only/audio-only.
- Measure drift for 60 s and assert < 20 ms.
- Without WASAPI, video still plays silent and reports the audio state.

## 6. E5c — color and hardware

- Plane upload for NV12 and P010, YUV shaders with BT.601/709 matrices.
- `object-fit`, rotation metadata, 10-bit, color range.
- D3D11VA `device()` + `import_native`; automatic software fallback with an
  `imports` counter.
- wgpu stays software until E8; no fake hardware path.

## 7. E6 — optional FFmpeg provider

- Separate DLL/static target, LGPL notices, no linking into the core.
- Loaded by `wn_app_load_media_provider` from an allowlist; never from a media
  path, a document or a network URL.
- MKV/HEVC/AV1/VP9 and audio formats; software decode first.
- The shipped build starts and plays Media Foundation formats without it.
- Fixture matrix with known hashes/frames; skipped cells are explicit.

## 8. E7 — subtitles

- SRT/VTT: parse, cue lookup, time offset, seek, language preference.
- Overlay in the engine (not a fake image) with the existing text renderer;
  plain style first.
- ASS/SSA via libass (optional target); PGS bitmap overlay.
- Subtitle menus in the app read the existing `subtitle_tracks` data.

## 9. E8 — wgpu and HDR

- RGBA upload on wgpu first, then native import if the backend allows it.
- HDR swapchain/tonemap, 10-bit, reference frames.
- No parity claims without a color fixture; document gaps.

## 10. Risks and decisions

| Risk | Mitigation |
|---|---|
| HEVC needs the Microsoft codec pack | Detect and report; FFmpeg optional covers it |
| MKV support varies by Windows build | Probe with MF; fall back to FFmpeg |
| FFmpeg licensing and DLL size | Separate optional provider, notices, allowlist |
| A/V drift | Audio master clock, measured gate |
| GPU/driver variance | Fixed backend for goldens, tolerance, layout/text asserts |
| Agent overconfidence | Evidence per wave: PNG, log, tests, review |
| App/engine contract drift | ABI 8 frozen during E5; additive only with review |

Open decisions for the owner:
1. Ship Media Foundation only, or FFmpeg as the default optional provider?
2. Subtitles in the engine overlay or app overlay for SRT first?
3. Minimum Windows version and HEVC policy.
4. Whether wgpu is a supported playback backend or a fallback renderer.

## 11. Definition of done for the media milestone

- A local H.264 MP4/MKV plays in the gallery with audio, seek, pause, volume and
  a clear error state.
- Without a provider, the app remains honest: poster, unavailable message and
  external open.
- `media_test`, session, frame, blob and ABI suites pass without a decoder.
- A real-codec fixture matrix passes on the target machine.
- No blob compatibility shim, no image-node video, no fake player, no codec
  linked into the core.
