#!/usr/bin/env bash
# tools/package.sh OUT.zip: the built server, ready to unzip and run: the
# executable (and on Windows the MinGW and SDL2 DLLs it needs), the license
# and a short readme. server.ini is written by the server the first time.
set -euo pipefail
out=$(realpath -m "$1")
cd "$(dirname "$0")/.."
exe=""
for f in port/build/*-server/sms-server.exe port/build/*-server/sms-server; do
  [[ -f "$f" ]] && { exe=$f; break; }
done
[[ -n "$exe" ]] || { echo "Build the server first (./build.sh)." >&2; exit 1; }
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
dir="$stage/sms-server"
mkdir -p "$dir/mods"
cp "$exe" "$dir/"
if [[ "$exe" == *.exe ]]; then
  # the DLLs MSYS2 gave it (SDL2, the MinGW runtime)
  ldd "$exe" | awk '$3 ~ /^\/mingw64\// { print $3 }' | sort -u | xargs -r -I{} cp {} "$dir/"
else
  strip "$dir/sms-server"
fi
cp LICENSE "$dir/LICENSE.txt"
cp docs/QUICKSTART.txt "$dir/README.txt"
echo "Mods go here, one folder each (a game mode such as squid-race/)." > "$dir/mods/README.txt"
rm -f "$out"
(cd "$stage" && if command -v zip >/dev/null; then zip -qr "$out" sms-server; else python3 -m zipfile -c "$out" sms-server; fi)
ls -l "$out"
