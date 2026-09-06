# voice-input

`voice-input` is a small, NixOS-first voice input service designed for low
latency, low idle resource use, and native Wayland integration.

The project is being built as a C17 daemon with native PipeWire capture, a Unix
domain socket protocol, sherpa-onnx streaming recognition, and a thin Qt/QML
overlay. Fcitx5 is the preferred text commit path; libei and clipboard paste are
fallbacks.

## Status: v0.1 usable CLI and Fcitx5 input path

The first usable milestone includes:

- a persistent C17 daemon with zero audio processing while idle;
- native PipeWire capture at 16 kHz, mono, signed 16-bit PCM;
- a local Unix socket command/event protocol;
- start, stop, toggle, status, monitor, and clean shutdown commands;
- throttled real-time RMS level events for the future overlay;
- sherpa-onnx streaming Zipformer recognition for Chinese and English;
- partial/final transcript events and endpoint detection;
- a native Fcitx5 addon that commits final text to the focused application;
- a systemd user service, Nix package, and automated tests.

The Qt/QML overlay is the next milestone. No placeholder transcript is emitted.

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

Start the packaged daemon in one terminal. The Nix wrapper selects the pinned
model automatically:

```sh
./result/bin/voice-inputd
```

Control it from another terminal:

```sh
./result/bin/voice-inputctl status
./result/bin/voice-inputctl start
./result/bin/voice-inputctl monitor
./result/bin/voice-inputctl stop
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
          +---- PipeWire native capture
          +---- sherpa-onnx streaming ASR
          +---- Fcitx5 text commit
```

See [`SPEC.md`](SPEC.md) for the implementation constraints and reference
projects supplied for the project.

## Privacy and scope

Audio is processed locally. The project does not contain employer source code,
proprietary SDKs, private logs, customer data, or confidential configuration.

The default model is the official sherpa-onnx bilingual Chinese/English
streaming Zipformer. Nix downloads it from the upstream release with a pinned
SHA-256 hash and extracts only the INT8 encoder/joiner, decoder, tokens, and a
test fixture. sherpa-onnx is Apache-2.0 licensed; consult the upstream model
documentation for model and training-data terms.

## License

MIT
