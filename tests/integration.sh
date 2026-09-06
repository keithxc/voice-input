#!/usr/bin/env bash
set -euo pipefail

daemon=$1
ctl=$2
test_dir=$(mktemp -d)
socket_path="$test_dir/voice-input.sock"
daemon_pid=

cleanup() {
  if [[ -n "$daemon_pid" ]]; then
    kill "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
  rm -rf "$test_dir"
}
trap cleanup EXIT

"$daemon" --no-audio --socket "$socket_path" &
daemon_pid=$!
for _ in {1..50}; do
  [[ -S "$socket_path" ]] && break
  sleep 0.02
done
[[ -S "$socket_path" ]]

status=$("$ctl" --socket "$socket_path" status)
grep -q '^state: *idle' <<<"$status"
grep -q '^asr: *disabled' <<<"$status"
grep -q '^punctuation:' <<<"$status"
"$ctl" --socket "$socket_path" start | grep -q '"recording":true'
"$ctl" --socket "$socket_path" toggle | grep -q '"recording":false'
"$ctl" --socket "$socket_path" quit | grep -q '"stopping"'
wait "$daemon_pid"
daemon_pid=
