#!/usr/bin/env bash
# Run clang-tidy over first-party sources in the local (Nix) dev environment.
#
# The Nix clang-tidy uses its own clang frontend, which doesn't know where the
# GCC libstdc++ headers live, so <string> etc. aren't found out of the box. We
# discover g++'s own C++ header dirs at runtime and inject them as -isystem
# (paths are never hardcoded — they change on every `devenv update`).
#
# CI runs `run-clang-tidy` directly against a system toolchain and does not need
# this wrapper.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f build/compile_commands.json ]; then
    echo "build/compile_commands.json not found; run: cmake -B build" >&2
    exit 1
fi

# Mirror g++'s include search path, minus its compiler-internal C headers under
# /lib/gcc/ — clang ships its own and GCC's stddef.h (__float128) clashes.
extra=()
while IFS= read -r dir; do
    case "$dir" in
    */lib/gcc/*) ;;
    *) extra+=(-extra-arg=-isystem"$dir") ;;
    esac
done < <(g++ -E -x c++ - -v </dev/null 2>&1 |
    awk '/#include <...> search starts/{f=1;next} /End of search list/{f=0} f{gsub(/^ +/,"");print}')

exec run-clang-tidy -quiet -p build "${extra[@]}" "src/.*\.(cpp|h)$" "$@"
