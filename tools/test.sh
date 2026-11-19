#!/bin/sh
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output_root=$(realpath -m -- "${1:?an external output directory is required}")
case "$output_root/" in
  "$repo_root/"*) echo "Test output must be outside the repository" >&2; exit 2 ;;
esac
sh "$repo_root/tests/display/run_tests.sh" "$repo_root" "$output_root/display"
sh "$repo_root/tests/sdram/run_tests.sh" "$repo_root" "$output_root/sdram"
sh "$repo_root/tests/board_test/run_tests.sh" "$repo_root" "$output_root/board_test"
