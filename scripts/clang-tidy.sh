#!/usr/bin/env bash
# Run clang-tidy over first-party sources with the project's .clang-tidy config.
#
# clang-tidy's clang frontend needs to find the C++ standard library (and, under
# Nix, third-party) headers. We discover g++'s own search dirs at runtime and
# inject them as -isystem (paths are never hardcoded — they change on every
# `devenv update`), skipping the compiler-internal /lib/gcc/ dirs since clang
# ships its own and GCC's stddef.h (__float128) clashes.
#
# Uses run-clang-tidy when available (local Nix env); otherwise falls back to a
# per-file clang-tidy loop (CI installs a pinned clang-tidy without the wrapper).
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f build/compile_commands.json ]; then
    echo "build/compile_commands.json not found; run: cmake -B build" >&2
    exit 1
fi

isystem=()
while IFS= read -r dir; do
    case "$dir" in
    */lib/gcc/*) ;;
    *) isystem+=("-isystem$dir") ;;
    esac
done < <(g++ -E -x c++ - -v </dev/null 2>&1 |
    awk '/#include <...> search starts/{f=1;next} /End of search list/{f=0} f{gsub(/^ +/,"");print}')

if command -v run-clang-tidy >/dev/null 2>&1; then
    args=()
    for inc in "${isystem[@]}"; do args+=("-extra-arg=$inc"); done
    exec run-clang-tidy -quiet -p build "${args[@]}" 'src/.*\.(cpp|h)$' "$@"
fi

# Fallback: invoke clang-tidy per file, failing if any file reports findings.
args=()
for inc in "${isystem[@]}"; do args+=("--extra-arg=$inc"); done
status=0
while IFS= read -r file; do
    clang-tidy -p build "${args[@]}" "$file" "$@" || status=1
done < <(find src -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.h' \))
exit "$status"
