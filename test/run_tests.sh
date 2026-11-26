#!/bin/sh

set -e
cd "$(dirname "$0")/.."

# Only the FAP sources are scanned.
banned='qsort|strtok_r|strtok|system|popen|exec|fork'
if grep -nE "\b($banned)\s*\(" tele_radio_remote_app.c tele_radio_remote_radio.c \
     tele_radio_remote_storage.c tele_radio_remote_core.c; then
    echo "ERROR: banned libc symbol used in FAP sources (APPCHK would fail)" >&2
    exit 1
fi

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
CFLAGS="-Wall -Wextra -Werror -std=c11"
"${CC:-cc}" $CFLAGS -I. test/test_core.c tele_radio_remote_core.c -o "$dir/core"
"$dir/core"
"${CC:-cc}" $CFLAGS -I. -Itest/stubs test/test_radio.c tele_radio_remote_radio.c \
    tele_radio_remote_core.c -o "$dir/radio"
"$dir/radio"
