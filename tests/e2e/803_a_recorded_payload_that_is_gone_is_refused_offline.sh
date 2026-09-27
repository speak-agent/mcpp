#!/usr/bin/env bash
# requires: gcc
# 803_a_recorded_payload_that_is_gone_is_refused_offline.sh — mcpp#716.
#
# The provisioning stamp records that a list of `[xlings.workspace]` entries
# was installed once. A payload removed afterwards (`xlings remove`, a pruned
# cache) left the stamp claiming it, so the build skipped provisioning and
# succeeded while `mcpp::xpkg_dir` answered "". The stamp now counts only while
# every address still resolves to a payload; offline, a missing one is refused
# by name.
#
# Criteria:
#   1. with no stamp, an offline build refuses the undeclared-and-uninstalled
#      entry (the behaviour before this change, kept);
#   2. with a stamp for the same list and no payload on disk, the offline build
#      is refused, naming the address and the record, where it used to pass.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi

mkdir -p app/src
cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[xlings.workspace]
mcpp-e2e-payload-that-is-gone = "1.0.0"
EOF
echo 'int main() {}' > app/src/main.cpp
cd app

# ── 1 ──
if "$MCPP" build --offline > b1.log 2>&1; then
    cat b1.log; echo "FAIL: 1: an uninstalled entry was accepted offline"; exit 1
fi
address="$(sed -n 's/^ *declared: //p' b1.log | head -1)"
[[ "$address" == *mcpp-e2e-payload-that-is-gone* ]] || {
    cat b1.log; echo "FAIL: 1: the refusal does not name the declared entry"; exit 1; }
echo "ok: 1"

# ── 2 ──
# The stamp a successful provisioning of this list writes: FNV-1a of the list,
# one address per line, as `provision_xlings_addresses` computes it.
stamp="$(python3 - "$address" <<'PY'
import sys
h = 1469598103934665603
for ch in (sys.argv[1] + "\n").encode():
    h ^= ch
    h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
print(f"xlings-deps-{h:016x}")
PY
)"
mkdir -p "$MCPP_HOME/provisioned"
printf '%s\n' "$address" > "$MCPP_HOME/provisioned/$stamp"

if "$MCPP" build --offline > b2.log 2>&1; then
    cat b2.log; echo "FAIL: 2: a recorded payload that is not installed was accepted"; exit 1
fi
grep -q "recorded as provisioned" b2.log && grep -q "$address" b2.log \
    && grep -q "$stamp" b2.log || {
    cat b2.log; echo "FAIL: 2: the refusal does not name the address and the record"; exit 1; }
echo "ok: 2"

echo "PASS: 803_a_recorded_payload_that_is_gone_is_refused_offline"
