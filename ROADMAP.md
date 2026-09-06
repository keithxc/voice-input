# Roadmap

The architecture is fixed by `SPEC.md`: C17 core, native PipeWire, sherpa-onnx,
Unix domain sockets, a thin Qt/QML overlay, Fcitx5/libei output, Nix packaging,
and optional AMD XDNA2 acceleration.

## v0.1 — Audio and control foundation (complete)

- Native 16 kHz mono PipeWire capture
- Idle/start/stop/toggle state machine
- Newline-delimited JSON events over a Unix domain socket
- CLI controller and event monitor
- Nix package, systemd user unit, unit test, and integration test
- Real microphone validation on NixOS/Wayland

Checkpoint: `nix build` passes both tests and a live session emits level events.

## v0.2 — First useful transcription

- Pin sherpa-onnx and a multilingual streaming Zipformer model
- Feed captured PCM into `OnlineStream`
- Publish partial and final transcript events
- Implement endpoint detection and stream reset
- Measure first-partial and finalization latency on the Ryzen AI 9 HX 370 CPU

Checkpoint: dictate Chinese and English locally and receive stable partial/final
text through `voice-inputctl monitor`.

## v0.3 — Desktop input loop

- Thin Qt6/QML overlay consuming socket events
- Fcitx5-native commit into the focused input context
- Clipboard/libei fallback with explicit backend reporting
- KDE global shortcut and Home Manager/NixOS module

Checkpoint: press one shortcut, speak, see partial text, and commit final text
into native Wayland and Electron applications.

## Later

- Model/config selection without a runtime downloader
- Latency, CPU, wakeup, memory, and package-power benchmarks
- Optional XDNA2 backend only if measurements beat the CPU experience
- ARM64 packaging and clean-room Raspberry Pi validation
