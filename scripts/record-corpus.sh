#!/usr/bin/env bash
# Record the accuracy corpus that voice-input-asr-score reads.
#
#   ./scripts/record-corpus.sh ~/voice-input-corpus
#   ./scripts/record-corpus.sh ~/voice-input-corpus tests/asr/cases.txt
#
# Walks the prompt set, records one 16 kHz mono clip per prompt, and writes a
# manifest beside them. Already recorded prompts are skipped, so a corpus can
# be finished over several sittings and Ctrl-C never costs more than the clip
# being spoken. Recordings stay outside the repository: they are the speaker's
# voice, and they are large.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
corpus="${1:-${VOICE_INPUT_CORPUS_DIR:-}}"
prompts="${2:-$root/tests/asr/cases.txt}"

if [ -z "$corpus" ]; then
    echo "usage: record-corpus.sh CORPUS_DIR [PROMPTS]" >&2
    exit 1
fi
if [ ! -r "$prompts" ]; then
    echo "record-corpus: cannot read $prompts" >&2
    exit 1
fi
if ! command -v pw-record >/dev/null; then
    echo "record-corpus: pw-record is not on PATH" >&2
    exit 1
fi

mkdir -p "$corpus/clips"
manifest="$corpus/manifest.tsv"
touch "$manifest"

hint() {
    case ",$1," in
        *,quiet,*) echo "  speak softly, as if someone were asleep next door" ;;
        *,noise,*) echo "  play some background noise first, then speak normally" ;;
        *,endpoint,*) echo "  pause for about a second in the middle" ;;
        *) echo "  speak normally, at the distance you would really dictate from" ;;
    esac
}

index=0
recorded=0
while IFS=$'\t' read -r tags text; do
    case "$tags" in ''|'#'*) continue ;; esac
    [ -z "${text:-}" ] && continue
    index=$((index + 1))
    clip="$(printf 'clips/%03d.wav' "$index")"
    if grep -qF "$clip	" "$manifest"; then
        continue
    fi

    printf '\n[%03d] %s\n%s\n' "$index" "$tags" "$text"
    hint "$tags"
    read -r -p "  Enter to record, s to skip, q to quit: " answer </dev/tty
    case "$answer" in
        q|Q) break ;;
        s|S) continue ;;
    esac

    pw-record --rate 16000 --channels 1 --format s16 "$corpus/$clip" &
    recorder=$!
    trap 'kill "$recorder" 2>/dev/null || true' INT
    read -r -p "  recording, Enter to stop: " _ </dev/tty
    kill "$recorder" 2>/dev/null || true
    wait "$recorder" 2>/dev/null || true
    trap - INT

    if [ ! -s "$corpus/$clip" ]; then
        echo "  nothing was captured; leaving this prompt for later" >&2
        rm -f "$corpus/$clip"
        continue
    fi
    printf '%s\t%s\t%s\n' "$clip" "$tags" "$text" >> "$manifest"
    recorded=$((recorded + 1))
done < "$prompts"

echo
echo "record-corpus: $recorded new clip(s); $(grep -c '	' "$manifest") in $manifest"
echo "score them with:"
echo "  ./result/bin/voice-input-asr-bench $manifest"
