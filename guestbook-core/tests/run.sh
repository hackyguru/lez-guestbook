#!/usr/bin/env bash
# Build the guestbook harness against the pinned LEZ wallet FFI and pass it the rest of the arguments.
#   tests/run.sh state | deploy ../guestbook-program/guestbook.bin | use ADDRESS | post NAME TEXT
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../src"
export PATH=/nix/var/nix/profiles/default/bin:$PATH
NIX="nix --extra-experimental-features nix-command --extra-experimental-features flakes"
LEZ_REV=411adc8fdb3f4c3af64354c4fc26d3e4a6a3d3f4
FFI=$($NIX build --no-link --print-out-paths "github:logos-blockchain/logos-execution-zone?rev=$LEZ_REV#wallet")
JSON=$($NIX build --no-link --print-out-paths --impure --expr '(import <nixpkgs> {}).nlohmann_json')
SDK=$($NIX build --no-link --print-out-paths "github:logos-co/logos-cpp-sdk")
if [ ! -x "$HERE/harness" ] || [ -n "$(find "$SRC" "$HERE/harness.cpp" -newer "$HERE/harness" 2>/dev/null)" ]; then
    clang++ -std=c++17 -O1 -Wall -o "$HERE/harness" "$HERE/harness.cpp" "$SRC/guestbook_impl.cpp" \
        -I"$SRC" -I"$FFI/include" -I"$JSON/include" -I"$SDK/include" \
        -L"$FFI/lib" -lwallet_ffi -Wl,-rpath,"$FFI/lib" \
        -framework Security -framework CoreFoundation -framework SystemConfiguration 2>&1 | grep -v "extern-c-compat\|typedef struct WalletHandle\|^ *[0-9]* |\|^ *| *\^\|warning generated" || true
fi
[ $# -eq 0 ] && exit 0
exec "$HERE/harness" "$@"
