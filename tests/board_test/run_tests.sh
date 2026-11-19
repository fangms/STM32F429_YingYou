#!/bin/sh
set -eu
firmware_root=${1:?firmware root required}
output_root=${2:?external output directory required}
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$output_root"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I "$test_root/hal_stub" -I "$firmware_root/Core/Inc" \
  "$test_root/test_board_test.c" "$firmware_root/Core/Src/board_test.c" \
  "$firmware_root/Core/Src/boot_screen.c" "$firmware_root/Core/Src/display_test.c" \
  -o "$output_root/test_board_test"
cd "$output_root"
"$output_root/test_board_test"
