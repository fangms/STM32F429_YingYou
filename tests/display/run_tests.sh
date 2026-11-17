#!/bin/sh
set -eu
firmware_root=${1:?firmware root required}
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_root=${2:?output directory required}
mkdir -p "$output_root"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I "$test_root/hal_stub" -I "$firmware_root/Core/Inc" \
  "$test_root/test_display.c" "$firmware_root/Core/Src/ssd2828_bus.c" \
  "$firmware_root/Core/Src/board_ssd2828.c" "$firmware_root/Core/Src/display_test.c" \
  "$firmware_root/Core/Src/debug_log.c" "$firmware_root/Core/Src/ssd2828_video.c" \
  "$firmware_root/Core/Src/gc9503_panel.c" "$firmware_root/Core/Src/boot_screen.c" \
  "$firmware_root/Core/Src/board_screen.c" "$firmware_root/Core/Src/board_display_diag.c" -o "$output_root/test_display"
cd "$output_root"
"$output_root/test_display"
