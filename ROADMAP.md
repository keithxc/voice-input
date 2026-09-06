# Roadmap

The architecture is fixed by `SPEC.md`: C17 core, native PipeWire, sherpa-onnx,
Unix domain sockets, a thin Qt/QML overlay, Fcitx5/libei output, Nix packaging,
and optional AMD XDNA2 acceleration.

## v0.1 — First usable desktop input path (complete)

- Native 16 kHz mono PipeWire capture
- Idle/start/stop/toggle state machine
- Newline-delimited JSON events over a Unix domain socket
- CLI controller and event monitor
- Nix package, systemd user unit, unit test, and integration test
- Real microphone validation on NixOS/Wayland
- Pin sherpa-onnx and a multilingual streaming Zipformer model
- Feed captured PCM into `OnlineStream`
- Publish partial and final transcript events
- Implement endpoint detection and stream reset
- Commit final text through a native Fcitx5 addon
- Show recording, partial transcript, level, commit success, and errors in a
  non-focusable Qt6/QML layer-shell overlay
- Test the overlay event model and render the real QML in a headless smoke test
- Discover and capture hot-plugged PipeWire input devices in parallel, score
  signal-to-noise/clipping, and automatically select the clearest source

Checkpoint: `nix build` passes protocol, daemon integration, adaptive-gain,
UI-model, QML rendering, normal-ASR, and 4%-volume quiet-ASR decoding tests; a
live session emits microphone level events and the overlay stays above normal
Plasma windows without taking focus.

## v0.2 — Desktop integration and measurement

- Clipboard/libei fallback with explicit backend reporting
- KDE global shortcut and Home Manager/NixOS module
- Measure first-partial and finalization latency on the Ryzen AI 9 HX 370 CPU

Checkpoint: press one shortcut, speak, see partial text, and commit final text
into native Wayland and Electron applications.

## Later

- Model/config selection without a runtime downloader
- Latency, CPU, wakeup, memory, and package-power benchmarks
- Optional XDNA2 backend only if measurements beat the CPU experience
- ARM64 packaging and clean-room Raspberry Pi validation
