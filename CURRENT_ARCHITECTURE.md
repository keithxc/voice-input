# Current architecture

What the code does today, read out of the source rather than remembered. Every
number here was measured on this machine or taken from the pinned derivations;
where a figure is a measurement, how it was taken is stated with it.

Versions this describes:

- sherpa-onnx **1.13.3** (`/nix/store/…-sherpa-onnx-1.13.3/include/sherpa-onnx/c-api/c-api.h`)
- streaming model **`sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20`** (`flake.nix`)
- punctuation model **`sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8`** (`flake.nix`)

## Processes

Three, all in the user session, all local:

| Process | Language | Role |
| --- | --- | --- |
| `voice-inputd` | C17 | capture, recognition, punctuation, commit |
| `voice-input-overlay` | C++/QML | non-focusable layer-shell panel |
| `voice-input-fcitx5` | C++ | Fcitx5 addon that types the text |

They talk over two Unix domain sockets under `$XDG_RUNTIME_DIR/voice-input`:
`voice-input.sock` carries newline-delimited JSON events to the overlay and the
CLI, and `fcitx5.sock` carries a length-prefixed UTF-8 commit to the input
method. No HTTP, no D-Bus on the hot path, no Python, no network.

## The one thread that matters

`voice-inputd` has a single loop (`src/daemon.c:main`) and no worker threads:

```text
accept_clients -> read_clients -> vi_audio_iterate(10 ms) -> process_audio
   -> maybe_finish_recording -> throughput/source/level events -> repeat
```

PipeWire is driven by `pw_loop_iterate` from inside that loop, so its callbacks
run on this thread too. The capture callback only copies samples into a lossy
ring buffer (`VI_RING_SAMPLES` = 65536 samples = **4.096 s** at 16 kHz,
`src/audio.c:20`); everything expensive happens afterwards in `process_audio`,
which drains the ring and feeds the recogniser.

This matters for every latency claim below: **nothing heavy runs in a realtime
audio callback**, and a slow step delays the next loop iteration rather than
dropping a buffer, up to the four seconds the ring holds.

## Capture

- Every PipeWire `Audio/Source` is opened in parallel, up to `VI_MAX_SOURCES`
  = 16, at **16 kHz mono float**.
- Each source is scored on speech-to-noise, level and clipping; `src/selection.c`
  is a pure function over those statistics and is unit tested without an audio
  server.
- Bounded adaptive gain (`VOICE_INPUT_MAX_GAIN`, default 6.0;
  `VOICE_INPUT_TARGET_RMS`, default 0.08) lifts quiet microphones.
- `VOICE_INPUT_PREROLL_MS` (default 0, max 3000) rewinds into the ring when
  recording starts, so the first syllable survives the hotkey dispatch.
- `VOICE_INPUT_TAIL_MS` (default 250) keeps capturing after the stop command
  before finalising, so the last syllable survives too.

## Recognition

`src/asr.c` wraps the sherpa-onnx online recogniser.

- **Model kind** is detected from the files present in the model directory:
  encoder+decoder+joiner is a transducer, encoder+decoder is a Paraformer, a
  lone model file is a zipformer2 CTC. Quantised weights are preferred, so a
  directory holding both loads the int8 one. Nothing is hard-coded to one
  release's file names, which is what makes model comparison possible.
- **Default decoder** is `greedy_search`, 2 threads, provider `cpu`.
- **Endpointing** is on: rule 1 (2.4 s trailing silence), rule 2 (1.2 s
  trailing silence after text), rule 3 (20 s maximum utterance). Rules 1 and 2
  are settable in milliseconds through the environment.
- Audio is fed in whatever the ring returns, up to 4096 samples at a time, and
  decoded to completion each call.

Data flow out of it:

```text
partial (text changed)  -> JSON event -> overlay only, never committed
final   (endpoint, or   -> punctuation -> JSON event -> Fcitx5 commit
         stop + tail)
```

A partial is emitted only when the text actually changes, so the panel is not
redrawn for an unchanged string. On an endpoint the stream is reset; on stop,
`vi_asr_finish` pads with 300 ms of silence, flushes, emits the final, and
recreates the stream for the next utterance.

## Punctuation

`src/punctuation.c` wraps the offline ct-transformer. It is created **once at
daemon start** and never reloaded, and it runs **only on final text**,
immediately before the commit. Partials are never punctuated: they are redrawn
several times a second and repunctuating each redraw would cost the model's
whole latency for a string nobody keeps.

Because the model punctuates every language in Chinese, an utterance holding no
Han character has its marks rewritten in ASCII (`vi_punctuation_localise`),
which adds and removes nothing. When the model is missing or fails to load the
daemon logs it, reports `punctuation: unavailable`, and commits raw text.

## Model lifecycle

Both models are loaded once at startup and live for the life of the process.
Nothing is downloaded at runtime: Nix fetches both archives with a pinned
SHA-256, extracts only the files that are used, and the wrapper points the
daemon at the store paths. Measured on this machine:

| | load time | on disk |
| --- | --- | --- |
| streaming Zipformer (int8) | ~1.2 s cold, ~0.3 s warm | 66 MB |
| punctuation ct-transformer (int8) | ~0.21 s | 80 MB |

## Commit path

`vi_output_commit` connects to the Fcitx5 addon socket, sends a 32-bit
big-endian length and the UTF-8 text, and waits for a one-byte acknowledgment.
It is synchronous and it is on the loop thread. A failure is reported to the
overlay as `output-error`; nothing is retried.

## Configuration

All of it is environment variables, read once at startup, so a model can be
swapped between two runs of the same build:

| Variable | Default | Meaning |
| --- | --- | --- |
| `VOICE_INPUT_ASR_MODEL` | Nix store path | streaming model directory |
| `VOICE_INPUT_ASR_DECODER` | `greedy_search` | also `modified_beam_search` |
| `VOICE_INPUT_ASR_THREADS` | 2 | inference threads |
| `VOICE_INPUT_ASR_INT8` | 1 | 0 prefers float weights |
| `VOICE_INPUT_ASR_HOTWORDS` | unset | hotwords file, beam search only |
| `VOICE_INPUT_ENDPOINT_RULE1_MS` | 2400 | trailing silence, no text yet |
| `VOICE_INPUT_ENDPOINT_RULE2_MS` | 1200 | trailing silence after text |
| `VOICE_INPUT_PUNCT_MODEL` | Nix store path | punctuation model directory |
| `VOICE_INPUT_PUNCTUATION` | 1 | 0 disables punctuation |
| `VOICE_INPUT_PREROLL_MS` | 0 | pre-roll rewind |
| `VOICE_INPUT_TAIL_MS` | 250 | capture tail after stop |
| `VOICE_INPUT_MAX_GAIN` | 6.0 | adaptive gain ceiling |
| `VOICE_INPUT_TARGET_RMS` | 0.08 | adaptive gain target |
| `VOICE_INPUT_DEBUG_TIMING` | unset | millisecond trace of the whole path |

`voice-inputctl status` reports the ones that decide recognition, so the
running configuration never has to be inferred from a log.

## Performance risks

1. **The commit path is synchronous on the loop thread.** ASR finalisation,
   punctuation and the Fcitx5 round trip all happen before the loop iterates
   again. Measured, this is small — punctuation is 0.6-21 ms depending on
   sentence length, finalisation about 21 ms — but it is the place where a
   heavier model would first hurt, and the four-second ring is what keeps it
   safe. A second pass costing hundreds of milliseconds could not go here
   without moving to a worker thread.
2. **Peak RSS with both models is around 420 MB** (measured with the benchmark
   tool, which holds both). That is the cost of onnxruntime's arenas rather
   than the 146 MB of weights.
3. **Model load is on the startup path**, about 1.5 s for both cold. The daemon
   is long-lived, so this is paid once per session, but it delays the first
   usable hotkey after a fresh login.
4. **One decode call is unbounded in the loop**: `vi_asr_accept` decodes until
   the stream is not ready. With greedy search at 0.07x realtime there is
   plenty of headroom; a beam search on a slower machine narrows it.

## Accuracy risks

1. **The streaming Zipformer is from February 2023** and is the smallest
   bilingual option. It is the accuracy baseline, not a considered choice.
2. **Hotwords do nothing under `greedy_search`.** The fields exist in the
   config and are silently ignored; contextual biasing needs
   `modified_beam_search`.
3. **Technical vocabulary is the weak spot** for a Chinese/English model on a
   programmer's desk, and it is exactly what this project dictates.
4. **Endpoint rule 2 (1.2 s) decides where a sentence is cut.** Too eager and a
   thought is split into two commits; too slow and the commit is late. It has
   never been swept against recordings.
5. **Nothing was measured against a reference until now.** `tests/asr/cases.txt`
   plus `voice-input-asr-bench` close that gap; see `BENCHMARK.md`.
