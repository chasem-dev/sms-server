#!/usr/bin/env bash
# ./build.sh: builds sms-server for this computer, from the game (port/, the
# sms-pc-port submodule) with this repository's sources added:
#   Linux    port/build/linux-64-server/sms-server
#   Windows  port/build/windows-64-server/sms-server.exe (MSYS2 MINGW64 shell)
#   macOS    port/build/macos-64-server/sms-server
# The game's own build prerequisites apply (port/BUILD.md), plus OpenSSL's
# headers on Linux (libssl-dev). SMS_GAME_VERSION=<version> names the game's
# version (as SMS Launcher does); JOBS=n limits parallel compiler jobs.
set -euo pipefail
cd "$(dirname "$0")"
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
fi
[[ -f port/build.sh ]] || git submodule update --init port
export SMS_SERVER_DIR="$PWD" SMS_ARCH="${SMS_ARCH:-64}" SMS_STANDALONE=0
exec port/build.sh
