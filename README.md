# voice-input

`voice-input` is a small, NixOS-first voice input service designed for low
latency, low idle resource use, and native Wayland integration.

The project is being built as a C17 daemon with native PipeWire capture, a Unix
domain socket protocol, sherpa-onnx streaming recognition, and a thin Qt/QML
overlay. Fcitx5 is the preferred text commit path; libei and clipboard paste are
fallbacks.

## Status: v0.1 audio and control foundation

The first runnable milestone includes:

- a persistent C17 daemon with zero audio processing while idle;
- native PipeWire capture at 16 kHz, mono, signed 16-bit PCM;
- a local Unix socket command/event protocol;
- start, stop, toggle, status, monitor, and clean shutdown commands;
- throttled real-time RMS level events for the future overlay;
- a systemd user service, Nix package, and automated tests.

Streaming ASR, the Qt/QML overlay, and Fcitx5 text commit are the next milestone.
No placeholder transcript is emitted in v0.1.

## Build

```sh
nix build
```

For development:

```sh
nix develop
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

Start the daemon in one terminal:

```sh
./build/voice-inputd
```

Control it from another terminal:

```sh
./build/voice-inputctl status
./build/voice-inputctl start
./build/voice-inputctl monitor
./build/voice-inputctl stop
```

The default socket is
`$XDG_RUNTIME_DIR/voice-input/voice-input.sock`. Events are newline-delimited
JSON, so the future QML process does not need to link against the audio core.

For a control-path test without a microphone or PipeWire session:

```sh
./build/voice-inputd --no-audio --socket /tmp/voice-input-test/voice-input.sock
./build/voice-inputctl --socket /tmp/voice-input-test/voice-input.sock toggle
```

## Architecture

```text
global shortcut / CLI
          |
          v
   Unix domain socket <---- future Qt/QML overlay
          |
          v
     voice-inputd (C17)
          |
          +---- PipeWire native capture (v0.1)
          +---- sherpa-onnx streaming ASR (next)
          +---- Fcitx5 text commit (next)
```

See [`SPEC.md`](SPEC.md) for the implementation constraints and reference
projects supplied for the project.

## Privacy and scope

Audio is processed locally. The project does not contain employer source code,
proprietary SDKs, private logs, customer data, or confidential configuration.

## License

MIT
