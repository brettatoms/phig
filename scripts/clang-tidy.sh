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

# Canonicalize a path. `readlink -m` is GNU-only and macOS ships BSD readlink,
# which has no -m. These are directories from a compiler's own search list, so
# cd + `pwd -P` resolves them; a path that does not exist (include-fixed often
# does not) keeps its literal spelling, which is enough — it only ever has to
# compare equal to itself.
canon() {
    if [ -d "$1" ]; then (cd "$1" && pwd -P); else printf '%s\n' "$1"; fi
}

isystem=()
if [ "$(uname -s)" = "Darwin" ]; then
    # macOS has no GCC to interrogate — /usr/bin/g++ is a clang shim with no
    # GCC search list — and the headers live in the SDK. AppleClang finds that
    # implicitly, so compile_commands.json never names it and a clang-tidy from
    # pip cannot find <cstddef> or even <unistd.h> without being told.
    isystem+=("-isysroot" "$(xcrun --show-sdk-path)")
else
    # GCC's own builtin-include dir, resolved rather than pattern-matched.
    gcc_builtin="$(g++ -print-file-name=include)"
    gcc_builtin_fixed="$(g++ -print-file-name=include-fixed)"

    while IFS= read -r dir; do
        # Compare canonicalized paths: the search list spells dirs with ../ segments.
        real="$(canon "$dir")"
        if [ "$real" = "$(canon "$gcc_builtin")" ] ||
            [ "$real" = "$(canon "$gcc_builtin_fixed")" ]; then
            continue
        fi
        isystem+=("-isystem$dir")
    done < <(g++ -E -x c++ - -v </dev/null 2>&1 |
        awk '/#include <...> search starts/{f=1;next} /End of search list/{f=0} f{gsub(/^ +/,"");print}')
fi

if command -v run-clang-tidy >/dev/null 2>&1; then
    args=()
    for inc in "${isystem[@]}"; do args+=("-extra-arg=$inc"); done
    exec run-clang-tidy -quiet -clang-tidy-binary "$tidy_bin" -p build "${args[@]}" \
        '(core|libphig|cli)/.*\.(cpp|h)$' "$@"
fi

# Fallback: invoke clang-tidy per file, failing if any file reports findings.
#
# Sources only, no headers, to match what the run-clang-tidy branch above
# actually covers: it filters compile_commands.json, which holds translation
# units and therefore no .h entries at all. Passing a header to clang-tidy
# directly makes it the main file, which surfaces diagnostics in it that CI
# never reports — so globbing headers here made the local gate fail while CI
# passed. Headers are still analyzed as they are included by these sources.
args=()
for inc in "${isystem[@]}"; do args+=("--extra-arg=$inc"); done
status=0
while IFS= read -r file; do
    "$tidy_bin" -p build "${args[@]}" "$file" "$@" || status=1
done < <(find core libphig cli -maxdepth 1 -type f -name '*.cpp')
exit "$status"
