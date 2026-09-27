#!/usr/bin/env bash
# requires: gcc elf
# 802_a_host_build_applies_its_host_row.sh — mcpp#704.
#
# A build without `--target` targets the host, and `[target.<host-triple>]`
# describes that target as it describes any other. The row used to be read
# only when a target was named, so its `cxx_runtime` had no effect on a plain
# `mcpp build`, while `--target <host>` honoured it. A program linking Qt's
# libraries (which require libstdc++.so.6 and carry RUNPATH $ORIGIN) then
# embedded its own libstdc++ and failed at start.
#
# Criteria:
#   1. without the row, a plain build is self-contained (no NEEDED
#      libstdc++.so.6), the engine default;
#   2. with `[target.<host>] cxx_runtime = "toolchain-coupled"`, a plain build
#      needs libstdc++.so.6, exactly as `--target <host>` does;
#   3. the row is found under another spelling of the host triple.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

host="$(uname -m)-linux-gnu"

write_manifest() {   # $1 = the row's selector, empty for no row
    cat > mcpp.toml <<EOF
[package]
name    = "cxxrt"
version = "0.1.0"

[language]
standard   = "c++23"
import_std = false

[targets.cxxrt]
kind = "bin"
main = "src/main.cpp"
EOF
    if [[ -n "$1" ]]; then
        printf '\n[target.%s]\ncxx_runtime = "toolchain-coupled"\n' "$1" >> mcpp.toml
    fi
}

mkdir -p src
cat > src/main.cpp <<'EOF'
#include <cstdio>
#include <string>
int main() { std::string s = "RT_OK"; std::printf("%s\n", s.c_str()); }
EOF

needs_libstdcxx() {
    local bin
    bin="$(find target -path '*/bin/cxxrt' -type f | head -1)"
    [[ -n "$bin" ]] || { echo "FAIL: no program built"; exit 1; }
    readelf -d "$bin" | grep -q 'NEEDED.*libstdc++\.so'
}

# ── 1 ──
write_manifest ""
"$MCPP" build > b1.log 2>&1 || { cat b1.log; echo "FAIL: 1: build failed"; exit 1; }
if needs_libstdcxx; then
    echo "FAIL: 1: the default build is not self-contained"; exit 1
fi
echo "ok: 1"

# ── 2 ──
rm -rf target
write_manifest "$host"
"$MCPP" build > b2.log 2>&1 || { cat b2.log; echo "FAIL: 2: build failed"; exit 1; }
needs_libstdcxx || {
    cat b2.log; echo "FAIL: 2: [target.$host] cxx_runtime was not applied to a host build"; exit 1; }
[[ "$("$MCPP" run 2>&1 | tail -1)" == "RT_OK" ]] || { echo "FAIL: 2: the program does not run"; exit 1; }
echo "ok: 2"

# ── 3 ──
rm -rf target
write_manifest "$(uname -m)-unknown-linux-gnu"
"$MCPP" build > b3.log 2>&1 || { cat b3.log; echo "FAIL: 3: build failed"; exit 1; }
needs_libstdcxx || {
    cat b3.log; echo "FAIL: 3: the row was not matched under another spelling"; exit 1; }
echo "ok: 3"

echo "PASS: 802_a_host_build_applies_its_host_row"
