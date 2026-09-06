# Recognition quality backlog

Groundwork for improving accuracy, mixed Chinese/English handling and
punctuation. Everything below was checked against the versions this repository
actually pins, not from memory:

- sherpa-onnx **1.12.38** (`/nix/store/…-sherpa-onnx-1.12.38/include/sherpa-onnx/c-api/c-api.h`)
- model **`sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20`** (`flake.nix:12`)
- decoding **`greedy_search`**, endpoint rules 2.4 / 1.2 / 20.0 s (`src/asr.c:52-60`)

## 0. A repeatable accuracy measurement, before anything else

Nothing else on this list can be judged without it. Every change below trades
one thing for another, and "sounds better" is not a result.

- Collect a fixed set of clips: pure Chinese, pure English, mixed sentences,
  technical vocabulary, quiet speech, and speech over background noise.
- Report CER for Chinese and WER for English separately; a mixed-language
  average hides which half regressed.
- Reuse the existing offline path: `tests/asr_test.c` reads `test_wavs/2.wav`
  from the directory it is given, so a directory of symlinks to the model plus
  one clip is enough to score any recording without touching the daemon.
- Record the baseline of the current model before changing anything.

Recording real speech is the slow part. Text-to-speech is not a substitute:
`espeak-ng` output scored far worse than human speech on this model, so it
measures the synthesiser rather than the recogniser.

## 1. Punctuation

The C API already exposes it; no new dependency is needed.

```c
SherpaOnnxOfflinePunctuationConfig config;      /* .model.ct_transformer = path */
const SherpaOnnxOfflinePunctuation *punct =
    SherpaOnnxCreateOfflinePunctuation(&config);
const char *punctuated = SherpaOfflinePunctuationAddPunct(punct, text);
```

- Model: `sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8`,
  which covers Chinese and English. Add it to `flake.nix` beside the ASR model.
- Apply it in `on_transcript()` (`src/daemon.c`) to the **final** text only,
  immediately before `vi_output_commit()`. Partials are redrawn constantly and
  punctuating each one would make the panel flicker.
- It is a second model on the commit path, so measure what it adds to the
  keypress-to-text budget; the tail buffer already costs 250 ms.
- Make it optional (`VOICE_INPUT_PUNCTUATION_MODEL_DIR`, unset = off) so the
  daemon still runs without the extra model.
- An online variant exists (`SherpaOnnxOnlinePunctuationConfig`, cnn_bilstm +
  bpe_vocab) but its published model is English-only; check before using it.

## 2. Mixed Chinese and English

The model is bilingual, so the failure mode to look for is not "cannot do
English" but code-switching inside one sentence, and technical terms in
particular. Establish which it is with the harness from item 0 before choosing.

- **Hotwords / contextual biasing.** `hotwords_file`, `hotwords_buf` and
  `hotwords_score` are already in `SherpaOnnxOnlineRecognizerConfig`, but they
  work **only with `decoding_method = "modified_beam_search"`**; the current
  `greedy_search` silently ignores them. This is the most direct lever for
  domain vocabulary (NixOS, PipeWire, Zipformer, product names).
- **Cost of switching decoders.** Beam search is slower than greedy. Measured
  headroom today: one decode takes 20-28 ms per 320 ms of audio, roughly 8% of
  realtime, so there is room — but confirm on the slowest target machine, not
  on the desktop.
- `blank_penalty` is also exposed and worth a sweep; it trades insertions
  against deletions.

## 3. Other models

Upstream still recommends the pinned 2023-02-20 bilingual Zipformer for
Chinese, and the trilingual Paraformer (zh / Cantonese / en) for English. Both
are streaming. Score them with the same clips rather than trusting either
recommendation, and keep the larger non-mobile variants in the comparison since
the decode budget has room.

## 4. Tuning already available

- Endpoint rules (`src/asr.c:57-59`): `rule1_min_trailing_silence` 2.4 s and
  `rule2_min_trailing_silence` 1.2 s decide when an utterance is cut. Too eager
  splits a sentence mid-thought; too slow delays the commit.
- `VOICE_INPUT_MAX_GAIN` / `VOICE_INPUT_TARGET_RMS` already tune quiet speech.
- `VOICE_INPUT_PREROLL_MS` and `VOICE_INPUT_TAIL_MS` protect the two ends of an
  utterance and are deployed; they do not affect accuracy in between.

## Open question

Whether the reported "panel shows nothing for about two seconds" is still
present after the pre-roll work. It was never reproduced under measurement:
audio reaches the recogniser 10 ms after the start command and is fed at 0.97x
realtime, and the model's own streaming latency is one 320 ms chunk. If it
persists, capture a run with `VOICE_INPUT_DEBUG_TIMING=1` and read the first
`partial:` stamp before touching anything on this list.
