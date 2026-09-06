# Benchmark results

Everything here was measured on this machine with `voice-input-asr-bench`, the
tool in `tools/benchmark/`, against the versions in `flake.nix`. Where a number
is missing it is missing on purpose, and it says so.

Machine: x86_64 Linux 6.18, CPU provider, 2 inference threads, int8 weights.

## What is measured, and what is not

Two halves of the same question — is the text better, and is it still fast
enough:

- **Accuracy**: character error rate over Han characters and word error rate
  over Latin words, kept apart so a mixed sentence cannot hide which half
  regressed. Substitutions, deletions and insertions counted separately.
- **Latency**: time from **speech onset** to the first partial, time from the
  end of speech to the final text, punctuation cost, decode time as a fraction
  of realtime, peak RSS and CPU.

Audio is fed **paced at realtime**. Replaying a wav file as fast as the CPU
allows measures throughput, not what a speaker waits for. First-partial time is
measured from the first 20 ms window that rises above the clip's noise floor,
not from the head of the file, so a recording that starts with silence does not
flatter the model.

The benchmark feeds wav files. Capture, adaptive gain and source selection are
outside it; a regression in those does not show up here.

## Accuracy: not yet measured

**No accuracy number in this file is a real accuracy number, because no
labelled recording of this project's own speech exists yet.**

`tests/asr/cases.txt` holds the prompt set — Chinese sentences carrying English
identifiers, plain Chinese, plain English, quiet speech, speech over noise, and
one deliberate mid-sentence pause. It has not been recorded. Recording it takes
about ten minutes and is the single thing blocking every model decision below:

```sh
./scripts/record-corpus.sh ~/voice-input-corpus
./result/bin/voice-input-asr-bench ~/voice-input-corpus/manifest.tsv
```

The clips used below are the test recordings shipped with the upstream models.
They are real human speech with heavy Chinese/English code-switching, which is
a fair proxy for the workload, but **upstream publishes no reference transcript
for them**, so they can be timed and read but not scored. Using one model's
output as another's reference would only measure disagreement with the
incumbent, so it is not done.

The scoring path itself is verified: on a transcript that differs from its
reference only in punctuation, case and spacing it reports 0.0%, and the
alignment and per-language attribution are unit tested (`ctest -R score`).

## Streaming models compared

Four clips, 28.7 s of Chinese/English code-switched speech, identical
conditions.

| Model | Kind | Weights | First partial | Final | RTF | Peak RSS |
| --- | --- | --- | --- | --- | --- | --- |
| **zipformer-bilingual-zh-en-2023-02-20** (current default) | transducer | 66 MB | **724 ms** | **21 ms** | **0.06x** | 420 MB |
| streaming-paraformer-bilingual-zh-en | paraformer | 228 MB | 886 ms | 38 ms | 0.09x | 479 MB |
| streaming-zipformer-multi-zh-hans-2023-12-12 | transducer | 70 MB | 802 ms | 22 ms | 0.07x | **293 MB** |

First partial is the mean over the four clips; on the clips where the speaker
starts immediately it is **480-520 ms** for the incumbent.

What the transcripts show on the same audio (a reading, not a score):

```text
clip 0 — "昨天是 Monday, today is ..., the day after tomorrow 是星期三"
  bilingual zipformer  昨天是MONDAY TODAY IS LIBR THE DAY AFTER TOMORROW是星期三。
  paraformer           昨天是mon today is is零班二。the day after tomorrow是星期三。
  multi-zh-hans        昨天是MAND T的一零八二只，对阿富特曼人是星期三。

clip 2 — "这个是频繁的啊，不认识，记下来 frequently 频繁的"
  bilingual zipformer  这个是频繁的啊，不认识记下来FREQUENTLY频繁的。
  paraformer           这个是平繁的啊，不认识，接下来frequently苹繁的。
  multi-zh-hans        就是平凡的，真是接下来福瑞冯的，平凡的。
```

## Default model: unchanged, and why

The current `sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20` stays
the default. On measurement it is the fastest of the three on every latency
axis, the smallest to load, and the only one of the three that keeps English
words intact inside a Chinese sentence — which is the workload this project
exists for.

- **streaming Paraformer (bilingual)** is 3.5x the weights, 50% slower to
  decode, 80% slower to finalise, and on these clips visibly worse. Nothing
  recommends it here.
- **multi-zh-hans** is a Chinese-only model. It uses the least memory and is
  fast, but it transliterates English into Chinese characters (`frequently` →
  `福瑞冯`), which makes it unusable for mixed input regardless of its Chinese
  accuracy. It would be a reasonable default only for a Chinese-only build.

This is a decision made on latency and on legible failure modes, not on error
rate. **It should be revisited once `tests/asr/cases.txt` is recorded**, which
is the only way to tell whether the incumbent's Chinese is actually competitive
with a newer Chinese-only model on a bilingual speaker's Chinese.

## Punctuation

`sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8`, loaded
once at daemon start, run on final text only.

| Utterance | Length | Time |
| --- | --- | --- |
| `好` | 1 character | **0.6 ms** |
| `这个function返回NULL的时候需要重新初始化` | 20 characters | **6.2 ms** |
| `今天下午三点我们在会议室讨论下一版的发布计划` | 22 characters | **12.1 ms** |
| `帮我看一下这个buffer应该怎么处理然后把return value检查一下` | 26 characters | **12.4 ms** |
| a 58-character multi-clause sentence | 58 characters | **21.4 ms** |

Load: **205-243 ms**. Budget: 100 ms per utterance. The worst case measured is
a fifth of it, so punctuation runs synchronously on the daemon's loop rather
than on a worker thread; the four-second capture ring absorbs the pause. The
result:

```text
in   帮我看一下这个buffer应该怎么处理然后把return value检查一下
out  帮我看一下这个 buffer 应该怎么处理，然后把 return value 检查一下。

in   这个PipeWire stream为什么一直处于paused状态NixOS重新build之后systemd service没有启动我怀疑是配置文件里面某一行写错了你帮我看一下日志
out  这个PipeWire stream为什么一直处于paused状态？NixOS重新build之后，systemd service没有启动，我怀疑是配置文件里面某一行写错了，你帮我看一下日志。
```

## Against the latency budget

| Target | Budget | Measured | |
| --- | --- | --- | --- |
| first partial | < 250 ms | **480-520 ms** | **misses** |
| partial refresh | 50-150 ms | 408 ms mean between changes | see below |
| speech end → ASR final | < 250 ms | **21 ms** | passes |
| punctuation | < 100 ms | **7-21 ms** | passes |
| speech end → commit | < 350 ms | **~30 ms** plus the Fcitx5 round trip | passes |

Two entries need reading rather than a verdict:

- **First partial misses its target by roughly 2x.** It is not decode cost —
  the decoder runs at 0.06x realtime — it is the model's own chunk latency.
  Nothing in this repository fixes it; a model with a shorter chunk would.
  Both alternatives measured are *worse* on this axis.
- **Partial refresh** is not a refresh rate here. A partial is emitted only
  when the recognised text actually changes, so 408 ms is how often new words
  appear, not how often the panel could redraw. Emitting unchanged partials
  faster would add flicker, not information.

The daemon adds `VOICE_INPUT_TAIL_MS` (default 250 ms) between the stop command
and finalisation, deliberately, so a trailing syllable is not cut. That is a
capture choice, not recognition latency, and it is settable.

## Resource use

- Peak RSS with both models held: **420 MB** — mostly onnxruntime arenas, not
  the 146 MB of weights.
- Model load at daemon start: **~1.2 s** cold for recognition, **~0.21 s** for
  punctuation. Paid once per session.
- Idle CPU is unchanged by this work: no polling was added, punctuation runs
  only on a final, and nothing new runs while the daemon is not recording.

## Reproducing

```sh
nix build
./result/bin/voice-input-asr-bench ~/voice-input-corpus/manifest.tsv
./result/bin/voice-input-asr-bench --model ~/models/candidate \
    --decoder modified_beam_search --threads 4 ~/voice-input-corpus/manifest.tsv
```

A manifest line is `path<TAB>tags<TAB>reference`; a reference of `-` times a
clip without scoring it, which is how the unlabelled upstream clips above were
measured.
