#!/bin/sh
set -eu
firmware_root=${1:?firmware root required}
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_root=${2:?output directory required}
mkdir -p "$output_root"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I "$test_root/hal_stub" -I "$firmware_root/Core/Inc" \
  "$test_root/test_bringup.c" "$firmware_root/Core/Src/debug_log.c" \
  "$firmware_root/Core/Src/board_sdram.c" -o "$output_root/test_bringup"
"$output_root/test_bringup"
