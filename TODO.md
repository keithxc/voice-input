# Recognition quality backlog

What is left after the v0.2 accuracy and punctuation work, and what was checked
against the versions this repository actually pins rather than remembered:

- sherpa-onnx **1.13.3** (`/nix/store/…-sherpa-onnx-1.13.3/include/sherpa-onnx/c-api/c-api.h`)
- model **`sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20`** (`flake.nix`)
- decoding **`greedy_search`**, endpoint rules 2.4 / 1.2 / 20.0 s (`src/asr.c`)

`CURRENT_ARCHITECTURE.md` describes what the code does today and
`BENCHMARK.md` holds the measurements.

## 0. Record the corpus and take the baseline

**This is the one thing everything else waits for.** The harness is finished
and tested; the recordings are not made, and they cannot be automated.

```sh
./scripts/record-corpus.sh ~/voice-input-corpus     # about ten minutes
./result/bin/voice-input-asr-bench ~/voice-input-corpus/manifest.tsv
```

Text-to-speech is not a substitute: `espeak-ng` output scored far worse than
human speech on this model, so it measures the synthesiser rather than the
recogniser. Keep `tests/asr/cases.txt` and its tags stable once recorded, or
later runs stop being comparable.

Note what the harness does not cover: it feeds a wav file, so capture, gain and
source selection are outside the measurement. A regression in those will not
show up here.

## 1. Mixed Chinese and English

The model is bilingual, so the failure to look for is not "cannot do English"
but code-switching inside one sentence, and technical terms in particular.
`tests/asr/cases.txt` is built around exactly that; establish which failure it
is from the baseline before choosing a lever.

- **Hotwords / contextual biasing.** `hotwords_file` is already wired through
  `VOICE_INPUT_ASR_HOTWORDS`, but it works **only with
  `decoding_method = "modified_beam_search"`**; `greedy_search` accepts the
  setting and silently ignores it. This is the most direct lever for domain
  vocabulary (NixOS, PipeWire, GPIO, framebuffer, product names). It must stay
  optional and must not ship with anyone's word list baked in.
- **Cost of switching decoders.** Beam search is slower than greedy. Greedy
  decodes at 0.06x realtime on this desktop, so there is headroom, but
  `voice-input-asr-bench` prints that ratio per run: confirm it on the slowest
  target machine rather than here.
- `blank_penalty` is exposed in `struct vi_asr_config` and worth a sweep; it
  trades insertions against deletions, which the benchmark reports separately
  for exactly this reason.
- Hotwords can also be attached per stream with
  `SherpaOnnxCreateOnlineStreamWithHotwords`, which would allow a per-session
  vocabulary without rebuilding the recogniser.

## 2. Other models

The loader accepts transducer, Paraformer and zipformer2-CTC directories and
prefers int8 weights, so a candidate only has to be unpacked and pointed at:

```sh
./result/bin/voice-input-asr-bench --model ~/models/candidate \
    ~/voice-input-corpus/manifest.tsv
```

Two alternatives were already measured on timing and read for their failure
modes; see `BENCHMARK.md`. Neither beat the incumbent, and the decision to keep
it was made without error rates, so it is the first thing to revisit once the
corpus exists. Hold the latency budget while doing it: a model that wins on CER
but finalises in more than about 250 ms is not the right default for an input
method.

## 3. Endpoint tuning

`rule2_min_trailing_silence` (1.2 s) decides when a sentence is cut mid-speech.
Too eager splits a thought into two commits; too slow delays the text. It is
settable without a rebuild (`VOICE_INPUT_ENDPOINT_RULE2_MS`) and has never been
swept against recordings. The `endpoint` tag in `tests/asr/cases.txt` exists
for this.

## 4. First-partial latency

The measured 480-520 ms from speech onset misses the 250 ms target by about 2x.
It is the model's own chunk latency, not decode cost, so it needs a model with
a shorter chunk rather than tuning here. Both alternatives measured are worse
on this axis. See `BENCHMARK.md`.

## 5. Two-pass rescoring

Researched and measured, not implemented: a second pass costs more than the
entire commit budget and did not win on the clips tried. `TWO_PASS_NOTES.md`
has the numbers and the conditions that would change the answer.

## Open question

Whether the reported "panel shows nothing for about two seconds" is still
present after the pre-roll work. It was never reproduced under measurement:
audio reaches the recogniser 10 ms after the start command and is fed at 0.97x
realtime, and the benchmark measures a first partial at about 500 ms from
speech onset, not a two-second stall. If it persists, capture a run with
`VOICE_INPUT_DEBUG_TIMING=1` and read the first `partial:` stamp before
touching anything on this list.
