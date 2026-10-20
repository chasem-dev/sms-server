#!/usr/bin/env bash
# tools/check_start.sh EXE: starts the server in an empty folder, where it
# writes server.ini and stops for want of a disc image. That proves the
# executable loads (its libraries are there) and the server's code runs
# before the game's.
set -uo pipefail
exe=$(realpath "$1")
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cd "$dir"
out=$("$exe" 2>&1)
code=$?
echo "$out" | grep '^\[server\]'
if [[ $code -eq 1 && -f server.ini ]] && grep -q 'Wrote server.ini' <<<"$out" && grep -q 'No disc image' <<<"$out"; then
  echo "sms-server starts: it wrote server.ini and asked for a disc image."
else
  echo "sms-server didn't start as expected (exit code $code):" >&2
  echo "$out" | tail -20 >&2
  exit 1
fi
