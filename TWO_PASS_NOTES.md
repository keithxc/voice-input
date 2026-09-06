# Two-pass rescoring: measured, not adopted

The idea is standard: show a small streaming model's partials while the speaker
talks, then rerun the finished utterance through an accurate offline model
before committing.

```text
audio -> streaming model -> partial -> overlay
endpoint -> offline model -> punctuation -> commit
```

It was measured before being designed in, because the whole question is
whether the second pass fits in the budget this project committed to:

```text
speech end -> ASR final     < 250 ms
punctuation                 < 100 ms
speech end -> commit        < 350 ms
```

## What was measured

`sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17`, the obvious candidate: a
non-autoregressive offline model covering Chinese and English, driven through
the same sherpa-onnx C API, `greedy_search`, 2 threads, CPU provider, on this
machine. Each utterance was decoded three times and the best time taken.

| Utterance | Audio | Second pass | Realtime factor |
| --- | --- | --- | --- |
| test_wavs/2.wav | 4.7 s | **385 ms** | 0.08x |
| test_wavs/1.wav | 5.1 s | **417 ms** | 0.08x |
| test_wavs/0.wav | 10.1 s | **833 ms** | 0.08x |

Load: 1.75 s. Peak RSS holding the model: **1.44 GB** (the float weights are
895 MB; this archive ships no int8 variant).

For comparison, on the same clips, the streaming Zipformer already in the
daemon finalises in **20 ms** and punctuation costs **7-14 ms**.

## Conclusion: not on the default path

1. **It does not fit.** A five-second sentence costs about 400 ms of second
   pass alone, before punctuation and the commit. The whole budget from the end
   of speech to text on screen is 350 ms. The cost is linear in utterance
   length -- 0.08x realtime -- so a ten-second sentence costs twice that. This
   is exactly the "说完后等 1~3 秒" failure the project exists to avoid.
2. **It did not even win.** On the one clip where the difference is legible,
   the second pass was worse than the streaming model it was meant to correct:

   ```text
   streaming Zipformer  这个是频繁的啊，不认识记下来FREQUENTLY频繁的。
   SenseVoice offline   就是平凡的啊，不然是接下来frequent平繁的。
   ```

   Three clips is not an evaluation, and this is not a claim that SenseVoice is
   a worse model. It is a claim that "offline and larger" is not automatically
   more accurate for this audio, and that the assumption had to be tested
   before spending 400 ms on it.
3. **It costs 1.4 GB of RSS** on a daemon that is supposed to be idle-cheap.

## What would change the answer

- An **int8 SenseVoice** build exists as a separate upstream release and was
  not measured here. It should be perhaps twice as fast and a quarter of the
  size. Even at 200 ms for a five-second sentence it would consume most of the
  budget, so it would have to be opt-in, not the default.
- A **shorter budget for short utterances only**: dictation is mostly one
  clause at a time, and a two-second sentence would cost about 160 ms. A second
  pass gated on utterance length is conceivable, but it makes the commit
  latency depend on what was said, which is worse to use than a consistent one.
- **Committing the first pass and correcting it afterwards** is not an option
  here: the text has already been typed into somebody's editor.

If it is ever revisited, the second pass must run on a worker thread, not on
the daemon's loop, and the accuracy claim must come from
`voice-input-asr-bench` over the recorded corpus rather than from three clips.

## Where the real headroom is

The streaming model finalises in 20 ms. The number that misses its target is
the **first partial: about 480-520 ms from speech onset** against a 250 ms
goal. That is the model's own chunk latency, not decode cost -- the decoder
runs at 0.06x realtime. Reducing it means a model with a shorter chunk, not a
second pass, and it changes what the speaker sees rather than what is
committed.
