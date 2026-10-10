#!/usr/bin/env bash
# Build and run the panel_core host tests with plain gcc + ASan/UBSan, no
# IDF linux target needed (which requires libbsd-dev). Uses IDF's bundled
# Unity and the cJSON the component manager downloaded (run
# `idf.py reconfigure` in test/host once if managed_components is missing).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
cjson="$here/managed_components/espressif__cjson/cJSON"
unity="${IDF_PATH:?IDF_PATH not set}/components/unity/unity/src"
out="$here/build-plain"
mkdir -p "$out"
gcc -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter \
    -Wno-sign-compare -fsanitize=address,undefined -g \
    -I"$root/components/panel_core/include" -I"$cjson" -I"$unity" \
    "$root"/components/panel_core/*.c "$here"/main/test_*.c \
    "$here/plain_main.c" "$cjson/cJSON.c" "$unity/unity.c" \
    -o "$out/host_tests"
"$out/host_tests"
