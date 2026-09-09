# Backlog

Updated 2026-09-08: accuracy takes priority over the former 250 ms final target.

Completed: fixed-size streaming input, pre-roll isolation, background whole-
utterance refinement, cancellation, one commit after stop, raw clipping/weak
input hints, bounded output waits, and reproducible Nix packaging of both final
models. Public acoustic recordings provide an initial measurable baseline;
see BENCHMARK.md.

1. Obtain independent references for at least 10 representative personal
   Chinese/English technical utterances, including identifiers and long pauses.
   Compare CER/WER, lost Latin terms and stop-to-commit time with both modes.
2. Improve mixed-language routing only against referenced data; the current
   Latin-word guard can retain a wrong draft and cannot recover missing terms.
3. Investigate very weak capture: raw waveforms at the lowest playback level
   already lose Chinese speech. Keep device-level changes separate from model
   comparisons and measure signal quality before tuning gain.
4. Measure 60-second dictation and memory/latency on slower CPUs. Make the
   duration limit configurable only with a bounded storage/inference policy.
5. Test actual focused application commits across supported desktops; the
   acoustic harness uses a fake Fcitx5 ACK to avoid typing into user windows.

Next action: the referenced personal technical corpus. Checkpoint: measured
improvement over streaming with no dropped known English terms, and explicit
reporting of remaining errors. Keep recordings and private vocabulary outside
Git. This is not a blocker for using the measured public-corpus improvements.

2026-09-09: v0.1.2 adds experimental, default-off VAD-gated empty-draft recognition and conservative
prefix cleanup. Next: independently referenced technical code-switching audio,
including numeric self-corrections and negation. Evaluate stop-to-commit P50/P95
against a two-second target; model-only timings are not sufficient. General
LLM rewrite candidates remain excluded from automatic commit after observed
number changes and translation. Keep personal recordings outside Git.
