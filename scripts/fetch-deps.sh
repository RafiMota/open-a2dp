#!/usr/bin/env sh
# Download third-party sources into third_party/ (not committed).
# Pin exact commits here once a release is cut.
set -eu
cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

fetch() { # name url
    if [ -d "$1/.git" ]; then
        git -C "$1" pull --ff-only
    elif [ -d "$1" ]; then
        echo "$1: already present (not a git checkout), skipping"
    else
        git clone --depth 1 "$2" "$1"
    fi
}

fetch sbc         https://git.kernel.org/pub/scm/bluetooth/sbc.git
fetch libopenaptx https://github.com/pali/libopenaptx.git
fetch libldac     https://android.googlesource.com/platform/external/libldac
fetch FindWDK     https://github.com/SergiusTheBest/FindWDK.git
