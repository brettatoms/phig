#!/usr/bin/env bash
# Run clang-tidy over first-party sources with the project's .clang-tidy config.
#
# clang-tidy's clang frontend needs to find the C++ standard library (and, under
# Nix, third-party) headers. We discover g++'s own search dirs at runtime and
# inject them as -isystem (paths are never hardcoded — they change with the
# toolchain), skipping only GCC's own builtin-include dir, since clang ships its
# own and GCC's stddef.h (__float128) clashes.
#
# The skip must match that one directory, not any path containing /lib/gcc/:
# libstdc++'s dirs are often spelled through it (e.g.
# /usr/lib/gcc/<triple>/16/../../../../include/c++/16), and dropping those makes
# <cstdlib>'s `#include_next <stdlib.h>` fail with "file not found".
#
# Uses run-clang-tidy when available (it parallelizes); otherwise falls back to a
# per-file clang-tidy loop. Both branches run the SAME `clang-tidy` binary — the
# first one on PATH, which is the version mise/CI pins. run-clang-tidy otherwise
# resolves its own default (e.g. a distro clang-tidy-18), which silently ignores
# the pin and reports different findings than a local run.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f build/compile_commands.json ]; then
    echo "build/compile_commands.json not found; run: cmake -B build" >&2
    exit 1
fi

tidy_bin="$(command -v clang-tidy || true)"
if [ -z "$tidy_bin" ]; then
    echo "clang-tidy not found on PATH" >&2
    exit 1
fi
# The version is part of the gate's result, so make it visible in CI logs.
echo "using $tidy_bin ($("$tidy_bin" --version | awk '/version/{print $NF; exit}'))" >&2

# GCC's own builtin-include dir, resolved rather than pattern-matched.
gcc_builtin="$(g++ -print-file-name=include)"
gcc_builtin_fixed="$(g++ -print-file-name=include-fixed)"

isystem=()
while IFS= read -r dir; do
    # Compare canonicalized paths: the search list spells dirs with ../ segments.
    real="$(readlink -m -- "$dir")"
    if [ "$real" = "$(readlink -m -- "$gcc_builtin")" ] ||
        [ "$real" = "$(readlink -m -- "$gcc_builtin_fixed")" ]; then
        continue
    fi
    isystem+=("-isystem$dir")
done < <(g++ -E -x c++ - -v </dev/null 2>&1 |
    awk '/#include <...> search starts/{f=1;next} /End of search list/{f=0} f{gsub(/^ +/,"");print}')

if command -v run-clang-tidy >/dev/null 2>&1; then
    args=()
    for inc in "${isystem[@]}"; do args+=("-extra-arg=$inc"); done
    exec run-clang-tidy -quiet -clang-tidy-binary "$tidy_bin" -p build "${args[@]}" \
        '(core|libphig|cli)/.*\.(cpp|h)$' "$@"
fi

# Fallback: invoke clang-tidy per file, failing if any file reports findings.
args=()
for inc in "${isystem[@]}"; do args+=("--extra-arg=$inc"); done
status=0
while IFS= read -r file; do
    "$tidy_bin" -p build "${args[@]}" "$file" "$@" || status=1
done < <(find core libphig cli -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.h' \))
exit "$status"
