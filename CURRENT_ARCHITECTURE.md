# Current architecture

Updated 2026-09-08. The Nix package pins sherpa-onnx 1.13.3, streaming bilingual
Zipformer, offline SenseVoice INT8, offline bilingual Paraformer INT8 and
ct-transformer punctuation. Models load once at daemon startup; inference and
audio remain local.

## Processes and threads

The C17 daemon handles PipeWire capture, socket commands and streaming previews
in its main loop. A pthread worker performs final offline recognition. The
Qt/QML layer-shell overlay and Fcitx5 addon are separate processes connected by
Unix sockets under `$XDG_RUNTIME_DIR/voice-input`.

```text
PipeWire raw 16 kHz mono -> bounded session buffer -> final worker
                        -> adaptive gain -> 20 ms ASR blocks -> preview
stop -> 250 ms capture tail -> worker -> optional punctuation -> Fcitx5 ACK
cancel -> discard session and suppress any pending worker result
```

The capture callback copies into a 4.096-second ring. Streaming ASR buffers
caller chunks into 320-sample blocks, so callback packet sizes do not change
recognition. Endpoint finals accumulate in the draft in accurate mode. The
60-second raw session buffer bounds storage and automatically ends capture.
Optional pre-roll (default off, up to 3 seconds) never crosses a previous stop.

The final worker selects SenseVoice for Chinese/English and Paraformer for
mixed drafts. It retains the draft when mixed output loses meaningful Latin
words. Weak utterances receive uniform peak-bounded gain; silence and already
strong input are not boosted. SenseVoice punctuation is retained; the other
outputs use ct-transformer when enabled. Model failure falls back to streaming.
`VOICE_INPUT_FINAL_MODE=streaming` explicitly restores endpoint commits.

Cancellation marks a pending job and immediately updates the UI; inference
itself finishes in the background, but its cancelled result cannot commit.
Only the main loop commits text. Fcitx5 socket send/receive operations have
one-second timeouts. Startup model loading and final punctuation still run on
the main thread; streaming decoding also needs to keep up with capture.

## UI and input quality

The protocol exposes recording, finishing, processing, cancelled, output
success/error and idle states. Status queries reply only to their requesting
client. Level events include raw RMS, peak and clipping before adaptive gain,
so the overlay can display weak-input and overload hints without replacing the
transcript. Software gain cannot recover speech lost in the capture path.

## Validation and limits

CTest covers fixed ASR block sizes, stream reset, pre-roll isolation, raw
metrics, output timeout, worker cancellation/reuse, routing, normalization,
protocol, overlay behavior and QML rendering. BENCHMARK.md records referenced
Chinese/English and unscored mixed acoustic samples. Mixed technical terms,
very weak speech and personal dictation remain accuracy risks; the small public
corpus is not evidence of universal accuracy.
