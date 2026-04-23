#!/usr/bin/env bash

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export IDF_PATH="$ROOT_DIR/.tools/esp-idf"
export IDF_TOOLS_PATH="$ROOT_DIR/.tools/espressif"

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF is not installed at $IDF_PATH" >&2
    echo "Expected local setup under .tools/esp-idf and .tools/espressif." >&2
    return 1 2>/dev/null || exit 1
fi

. "$IDF_PATH/export.sh"
