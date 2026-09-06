#!/usr/bin/env bash
# Run the locally built daemon and overlay in place of the installed user
# services, so a change can be tried in the real desktop session without
# reinstalling or rebuilding the system configuration.
#
#   ./scripts/dev-session.sh                    # timing traces on
#   VOICE_INPUT_PREROLL_MS=1000 ./scripts/dev-session.sh
#
# Ctrl-C restores the installed services.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${VOICE_INPUT_BUILD_DIR:-$root/build}"
for binary in voice-inputd voice-input-overlay; do
    if [ ! -x "$build/$binary" ]; then
        echo "dev-session: $build/$binary is missing; build it first" >&2
        exit 1
    fi
done

# Inherit the model and tuning from the running service so the only difference
# from production is the code under test. The unit's own Environment= does not
# carry all of it, but the live process does, so read that while it still runs.
installed_pid="$(systemctl --user show voice-inputd -p MainPID --value 2>/dev/null || echo 0)"
if [ "${installed_pid:-0}" -gt 0 ] && [ -r "/proc/$installed_pid/environ" ]; then
    while IFS= read -r -d '' assignment; do
        case "$assignment" in
            VOICE_INPUT_*)
                name="${assignment%%=*}"
                [ -z "${!name:-}" ] && export "${assignment?}"
                ;;
        esac
    done < "/proc/$installed_pid/environ"
fi
# The installed service predates a setting whenever a new one is added, so fall
# back to the pinned store paths this flake already builds rather than leaving
# the feature silently off in the very session meant to test it.
packaged_model() {
    nix build --no-link --print-out-paths "$root#$1" 2>/dev/null || true
}
if [ -z "${VOICE_INPUT_MODEL_DIR:-}" ]; then
    VOICE_INPUT_MODEL_DIR="$(packaged_model asr-model)"
    export VOICE_INPUT_MODEL_DIR
fi
if [ -z "${VOICE_INPUT_MODEL_DIR:-}" ]; then
    echo "dev-session: set VOICE_INPUT_MODEL_DIR (the installed service has none)" >&2
    exit 1
fi
if [ -z "${VOICE_INPUT_PUNCT_MODEL_DIR:-}" ] && [ -z "${VOICE_INPUT_PUNCT_MODEL:-}" ]; then
    VOICE_INPUT_PUNCT_MODEL_DIR="$(packaged_model punctuation-model)"
    export VOICE_INPUT_PUNCT_MODEL_DIR
fi
export VOICE_INPUT_DEBUG_TIMING="${VOICE_INPUT_DEBUG_TIMING:-1}"

restore() {
    trap - INT TERM EXIT
    kill "${daemon_pid:-}" "${overlay_pid:-}" 2>/dev/null || true
    wait "${daemon_pid:-}" "${overlay_pid:-}" 2>/dev/null || true
    echo "dev-session: restoring installed services"
    systemctl --user start voice-inputd voice-input-overlay || true
}
trap restore INT TERM EXIT

echo "dev-session: stopping installed services"
systemctl --user stop voice-inputd voice-input-overlay

"$build/voice-inputd" &
daemon_pid=$!
sleep 1
"$build/voice-input-overlay" &
overlay_pid=$!

echo "dev-session: running; press the voice-input hotkey as usual, Ctrl-C to stop"
wait "$daemon_pid"
