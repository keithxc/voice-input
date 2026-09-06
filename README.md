# voice-input

`voice-input` is a small, NixOS-first voice input service designed for low
latency, low idle resource use, and native Wayland integration.

The project is being built as a C17 daemon with native PipeWire capture, a Unix
domain socket protocol, sherpa-onnx streaming recognition, and a thin Qt/QML
overlay. Fcitx5 is the preferred text commit path; libei and clipboard paste are
fallbacks.

## Status: v0.1 usable desktop input path

The first usable milestone includes:

- a persistent C17 daemon with zero audio processing while idle;
- zero-configuration parallel PipeWire capture of up to 16 hot-plugged input
  devices at 16 kHz mono, with automatic quality-based source selection;
- bounded adaptive input gain for quiet speech, with peak limiting;
- a local Unix socket command/event protocol;
- start, stop, toggle, status, monitor, and clean shutdown commands;
- throttled real-time RMS level events;
- sherpa-onnx streaming Zipformer recognition for Chinese and English;
- partial/final transcript events and endpoint detection;
- a non-focusable Qt6/QML Wayland layer-shell overlay showing recording,
  recognition, partial text, audio level, commit success, and output errors;
- a native Fcitx5 addon that commits final text to the focused application;
- systemd user services, a Nix package, and automated protocol, integration,
  adaptive-gain, UI-model, QML rendering, normal-ASR, and quiet-ASR tests.

No placeholder transcript is emitted: UI text and state come from the daemon's
real event stream.

Quiet-speech sensitivity can be tuned without rebuilding. The defaults are a
maximum gain of `6.0` and a target RMS of `0.08`:

```sh
VOICE_INPUT_MAX_GAIN=8 VOICE_INPUT_TARGET_RMS=0.10 voice-inputd
```

Higher values hear softer speech but also amplify room noise. Accepted ranges
are `1..16` for maximum gain and `0.01..0.30` for target RMS.

While recording, every available PipeWire `Audio/Source` is opened as a shared
capture stream. Each stream is scored independently using speech-to-noise
ratio, useful signal level, and clipping. The daemon sends only the clearest
source to ASR and uses a margin, consecutive votes, and a cooldown before
switching. Once speech begins, the chosen source is held through short pauses
so an utterance cannot be split by quality fluctuations. Newly connected USB
or Bluetooth microphones are discovered without configuration; unavailable
sources are skipped. Unrelated devices are not
mixed because their clocks, latency, and noise are not synchronized.
The overlay shows the source currently feeding ASR.

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

Start the desktop overlay in the graphical session:

```sh
./result/bin/voice-input-overlay
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
JSON, so the QML process does not link against the audio core.

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
   Unix domain socket <---- Qt/QML layer-shell overlay
          |
          v
     voice-inputd (C17)
          |
          +---- parallel PipeWire capture + automatic source selection
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
