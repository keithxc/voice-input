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
- 16 kHz mono PipeWire capture that follows the desktop's own input setting,
  with optional pinning or parallel quality-based selection across up to 16
  hot-plugged devices;
- bounded adaptive input gain for quiet speech, with peak limiting;
- a local Unix socket command/event protocol;
- start, stop, toggle, status, monitor, and clean shutdown commands;
- throttled real-time RMS level events;
- sherpa-onnx streaming Zipformer recognition for Chinese and English;
- partial/final transcript events and endpoint detection;
- a non-focusable Qt6/QML Wayland layer-shell overlay showing recording,
  recognition, partial text, audio level, commit success, and output errors;
- live system light/dark palette integration, with a compact squared KDE/Plasma
  treatment and a softer rounded GNOME treatment selected automatically;
- a `sources` command reporting every discovered capture node with its state,
  level, noise floor, and score;
- an opt-in pre-roll and a trailing tail so neither end of an utterance is cut;
- automatic punctuation of the committed text by a local ct-transformer model,
  applied to final text only and never to partials;
- a native Fcitx5 addon that commits final text to the focused application;
- model and decoder selection from the environment, so a model can be swapped
  between two runs of the same build;
- an offline benchmark that scores a recorded corpus for accuracy and latency
  and reports Chinese and English error rates separately;
- systemd user services, a Nix package, and automated protocol, source-selection,
  integration, adaptive-gain, UI-model, QML rendering, error-rate scoring,
  punctuation, normal-ASR, and quiet-ASR tests.

No placeholder transcript is emitted: UI text and state come from the daemon's
real event stream.

Quiet-speech sensitivity can be tuned without rebuilding. The defaults are a
maximum gain of `6.0` and a conservative target RMS of `0.03`:

```sh
VOICE_INPUT_MAX_GAIN=8 VOICE_INPUT_TARGET_RMS=0.03 voice-inputd
```

Higher values hear softer speech but also amplify room noise. Chunks below the
noise gate remain at unity gain, so pre-roll silence cannot prime the first
syllable at maximum gain. Accepted ranges are `1..16` for maximum gain and
`0.01..0.30` for target RMS.

The first words of an utterance are easy to lose, because a hotkey press has to
travel through the desktop's shortcut dispatch and a capture stream has to be
negotiated before a single sample exists. `VOICE_INPUT_PREROLL_MS` closes that
gap by keeping the ring buffer warm and rewinding into it when recording starts:

```sh
VOICE_INPUT_PREROLL_MS=1000 voice-inputd
```

It is off by default because it holds the microphone open for as long as the
daemon runs, which desktop environments show as continuous recording. Accepted
range is `0..3000` ms; audio older than the ring's 4 seconds cannot be recovered.

Speech also trails off at the end, so capture continues for a short tail after
the stop command before the recogniser finalises. `VOICE_INPUT_TAIL_MS` sets it,
defaulting to `250`; `0` finalises immediately, at the cost of the last syllable
of a sentence that fades out. Accepted range is `0..2000` ms.

## Choosing the microphone

By default the daemon listens to **the input selected in the desktop's own
sound settings**, the same one every other application uses. It follows
PipeWire's `default.audio.source`, so changing the input in KDE's sound
settings changes what voice-input hears, with no restart and no configuration
here.

`voice-inputctl status` says which one that resolved to:

```text
source-mode:       default
source:            AB13X Headset Adapter Mono
```

Two other modes exist for the cases the default does not cover:

```sh
VOICE_INPUT_SOURCE=Ryzen voice-inputd    # pin: substring of the node name
                                         # or description, case-insensitive
VOICE_INPUT_SOURCE=auto voice-inputd     # open every input and score them
```

`auto` opens every available `Audio/Source` in parallel, scores each on
speech-to-noise ratio, useful level and clipping, and feeds the recogniser from
the clearest. It is useful when microphones are hot-plugged and the desktop
default is not the one being spoken into, but it is **not** the default any
more: with several quiet inputs the scores are close, so it picks one before
anybody has spoken and can pick the wrong one. The arbitration rules live in
`src/selection.c` as a pure function over plain source statistics, so they are
unit tested without an audio server. Unrelated devices are never mixed, because
their clocks, latency and noise are not synchronised.

The `sources` command lists every discovered node with its state, level, noise
floor and score in any mode, and the overlay shows the source actually feeding
the recogniser.

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

If nothing is recognised, inspect the capture nodes while recording:

```sh
./result/bin/voice-inputctl start
./result/bin/voice-inputctl sources
```

Each entry reports the PipeWire node name, stream state, delivered buffer
count, RMS, noise floor, and score. An empty list means PipeWire exposes no
capture node to the daemon; entries stuck at `connecting` or with `chunks` at
zero mean the node is present but delivering nothing, which is usually a muted
or unrouted device rather than a recognition problem.

The default socket is
`$XDG_RUNTIME_DIR/voice-input/voice-input.sock`. Events are newline-delimited
JSON, so the QML process does not link against the audio core.

For a control-path test without a microphone or PipeWire session:

```sh
./build/voice-inputd --no-audio --socket /tmp/voice-input-test/voice-input.sock
./build/voice-inputctl --socket /tmp/voice-input-test/voice-input.sock toggle
```

## Development

`scripts/dev-session.sh` swaps the installed user services for a local build in
the running desktop session, inheriting the model path and tuning from the
service it replaces, and restores them on exit. It needs no reinstall:

```sh
cmake --build build
./scripts/dev-session.sh
```

`VOICE_INPUT_DEBUG_TIMING` (which that script sets) stamps the start command,
the first audio into the recogniser, every partial and final, the text commit,
and a throughput ratio, so a perceived delay can be attributed rather than
guessed at.

## Punctuation

Final text is punctuated before it is committed. The recogniser produces bare
words; a local ct-transformer model turns them into a sentence:

```text
帮我看一下这个buffer应该怎么处理然后把return value检查一下
帮我看一下这个 buffer 应该怎么处理，然后把 return value 检查一下。
```

The model is `sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8`,
packaged by Nix with a pinned hash like the recognition model, loaded once when
the daemon starts and never reloaded. It costs **0.6-21 ms** per utterance
depending on sentence length, measured on this machine, and runs only on the
final text: partials are redrawn several times a second and are left alone.

It punctuates every language in Chinese, so an utterance holding no Chinese
character has its marks rewritten in ASCII -- nothing is added or removed.

Punctuation is optional and its failure is not fatal. Without the model the
daemon commits unpunctuated text and reports `punctuation: unavailable`:

```sh
VOICE_INPUT_PUNCTUATION=0 voice-inputd          # off
VOICE_INPUT_PUNCT_MODEL=/path/to/model voice-inputd
```

## Choosing a model

Recognition is configured from the environment, so swapping a model needs no
rebuild:

```sh
VOICE_INPUT_ASR_MODEL=~/models/some-streaming-zipformer \
VOICE_INPUT_ASR_DECODER=modified_beam_search \
VOICE_INPUT_ASR_THREADS=4 voice-inputd
```

The loader reads whatever files the model directory holds rather than one
release's names: encoder + decoder + joiner is a transducer, encoder + decoder
a Paraformer, a single model file a zipformer2 CTC. Quantised weights are
preferred; `VOICE_INPUT_ASR_INT8=0` asks for float ones. Hotwords
(`VOICE_INPUT_ASR_HOTWORDS`) need `modified_beam_search` -- the greedy decoder
accepts the setting and ignores it.

`voice-inputctl status` reports what is actually loaded:

```text
state:             idle
audio:             ready
asr:               ready
asr-backend:       sherpa-cpu
asr-model:         voice-input-streaming-zipformer-zh-en
asr-kind:          transducer
decoder:           greedy_search
threads:           2
punctuation:       enabled
punctuation-model: model.int8.onnx
sample-rate:       16000
tail-ms:           250
```

## Measuring accuracy and latency

Recognition changes trade one thing for another, so they are judged against a
recorded corpus rather than an impression. `tests/asr/cases.txt` holds a fixed
prompt set built around what this project actually dictates: Chinese sentences
carrying English identifiers, plain Chinese, plain English, quiet speech and
speech over noise. Record it once:

```sh
./scripts/record-corpus.sh ~/voice-input-corpus
```

Each prompt is recorded separately at 16 kHz mono; prompts already recorded are
skipped, so the corpus can be finished over several sittings. Recordings stay
outside the repository. Then measure:

```sh
./result/bin/voice-input-asr-bench ~/voice-input-corpus/manifest.tsv
```

The report gives, per clip, per tag and in total: a character error rate for
Han characters and a word error rate for Latin words **separately**, the time
from speech onset to the first partial, the time from the end of speech to the
final text, the punctuation cost, the decode time as a fraction of realtime,
peak RSS and CPU. Keeping the two error rates apart matters for a bilingual
model: one averaged number hides which of the two halves a change made worse.
Substitutions, deletions and insertions are counted apart, because tuning that
trades one against the other cannot be read from a single total.

Audio is fed **paced at realtime** by default, because replaying a wav file as
fast as the CPU allows measures throughput and says nothing about what a
speaker waits for. `--fast` turns the pacing off when only accuracy matters.

Any model can be scored against the same clips without rebuilding:

```sh
./result/bin/voice-input-asr-bench --model ~/models/other-model \
    --decoder modified_beam_search --threads 4 ~/voice-input-corpus/manifest.tsv
```

The benchmark can also replay the daemon's adaptive-gain stage and expose beam
and hotword settings explicitly:

```sh
./result/bin/voice-input-asr-bench --adaptive-gain --max-gain 8 \
    --target-rms 0.03 ~/voice-input-corpus/manifest.tsv
./result/bin/voice-input-asr-bench --decoder modified_beam_search \
    --max-active-paths 4 --hotwords terms.txt --hotwords-score 1.5 \
    ~/voice-input-corpus/manifest.tsv
```

Spacing, letter case, punctuation and fullwidth forms are normalised away
before comparison, so only recognition differences are counted. Number words
are not: a reference reading "三点" against a transcript of "3点" scores as an
error on purpose, because the text that lands in the application is what is
being measured. The comparison itself is unit tested (`ctest -R score`), so it
needs neither the model nor a microphone to be trusted.

A manifest is one clip per line, `path<TAB>tags<TAB>reference`, with paths
resolved against the manifest's own directory, so any recording can be scored
by adding a line to it. A reference of `-` times a clip without scoring it.

See [`BENCHMARK.md`](BENCHMARK.md) for measured results and
[`CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) for what the code does
today.

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
          +---- sherpa-onnx streaming ASR  ---- partial ---> overlay
          +---- ct-transformer punctuation ---- final -----> Fcitx5 commit
```

See [`SPEC.md`](SPEC.md) for the implementation constraints and reference
projects supplied for the project.

## Privacy and scope

Audio is processed locally. The project does not contain employer source code,
proprietary SDKs, private logs, customer data, or confidential configuration.

The default models are the official sherpa-onnx bilingual Chinese/English
streaming Zipformer and the ct-transformer Chinese/English punctuation model.
Nix downloads both from the upstream releases with pinned SHA-256 hashes and
extracts only the INT8 weights, tokens, and a test fixture. Nothing is
downloaded at runtime, and no audio or text leaves the machine. sherpa-onnx is
Apache-2.0 licensed; consult the upstream model documentation for model and
training-data terms.

## License

MIT
