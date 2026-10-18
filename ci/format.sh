#!/usr/bin/env bash
# Formats MafiaNet's C++ sources with clang-format (see .clang-format and
# .clang-format-ignore).
#
#   ci/format.sh          reformat in place
#   ci/format.sh --check  list the files that are not formatted; exit 1 if any
#
# CI pins clang-format 18.1.8 (pip install clang-format==18.1.8); other
# versions can format a few constructs differently. Set CLANG_FORMAT to use a
# specific binary.
set -euo pipefail
cd "$(dirname "$0")/.."

clang_format=${CLANG_FORMAT:-clang-format}
mode=(-i)
if [ "${1:-}" = "--check" ]; then
	mode=(--dry-run --Werror)
fi

run() {
	git ls-files -z -- \
		'Source/*.cpp' 'Source/*.h' \
		'Tests/*.cpp' 'Tests/*.h' \
		'Samples/*.cpp' 'Samples/*.h' \
		'DependentExtensions/*.cpp' 'DependentExtensions/*.h' \
		'Benchmarks/*.cpp' 'Benchmarks/*.h' |
		xargs -0 -n 50 -P "$(getconf _NPROCESSORS_ONLN)" "$clang_format" "${mode[@]}"
}

run
# A first pass over code that was never formatted can leave a trailing comment
# that a second pass aligns differently; formatting in place, run again so one
# invocation always ends stable.
if [ "${mode[0]}" = "-i" ]; then
	run
fi
