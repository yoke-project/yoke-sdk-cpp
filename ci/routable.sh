#!/usr/bin/env bash
# L3: the interface library across a routable channel of the published Core, signing in. Prints one line
# per case, as checks/run.sh does, and exits non-zero if it failed.
#
# The Core and the program it asks the host through are the published host archive of the newest release
# the manifest names, authenticated against it before they run, and never built. The case is the program
# `just build` made, build/published_test.
# Usage: routable.sh [version] [results directory]
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="${1:-}"
results="${2:-$root/.results}"
manifest="${YOKE_MANIFEST:-https://raw.githubusercontent.com/yoke-project/yoke/main/releases/manifest.jsonl}"
downloads="${YOKE_DOWNLOADS:-https://github.com/yoke-project/yoke/releases/download}"
id=yoke-sdk-cpp:across-a-routable-address.03

work="$(mktemp -d)"
trap 'kill "$core" 2> /dev/null; wait 2> /dev/null; rm -rf "$work"' EXIT
core=""
mkdir -p "$results"
fail() { echo "FAIL  $id — $*" | tee "$results/routable.txt"; exit 1; }

case "$(uname -m)" in
  x86_64) architecture=amd64 ;;
  aarch64 | arm64) architecture=arm64 ;;
  *) fail "no release for $(uname -m)" ;;
esac
[[ -x "$root/build/published_test" ]] || fail "build/published_test is not built; \`just build\` makes it"
curl -fsSL "$manifest" -o "$work/manifest.jsonl" || fail "the release manifest cannot be obtained"
if [[ -z "$version" ]]; then
  version="$(jq -r --arg p "yoke-linux-$architecture" 'select(.line == "publication" and .published == $p) | .version' \
    "$work/manifest.jsonl" | sed 's/^v//' | sort -V | tail -n 1)"
fi
archive="yoke-$version-linux-$architecture.tar.gz"
curl -fsSL "$downloads/v$version/$archive" -o "$work/$archive" || fail "the host archive of $version cannot be obtained"
want="$(jq -r --arg p "yoke-linux-$architecture" --arg v "v$version" \
  'select(.line == "publication" and .published == $p and .version == $v) | .digests[0]' "$work/manifest.jsonl")"
got="sha256:$(sha256sum "$work/$archive" | cut -d' ' -f1)"
[[ -n "$want" && "$want" == "$got" ]] || fail "the archive's digest $got is not the manifest's $want"
tar -xzf "$work/$archive" -C "$work" yoke-core yoke-auth || fail "the archive holds no yoke-core and yoke-auth"

# The host's authentication service, as a stack of the case's own: alice, with hunter2x.
mkdir -p "$work/pam.d" "$work/state/tls" "$work/run"
printf '#!/bin/sh\nread -r secret\n[ "$PAM_USER" = alice ] && [ "$secret" = hunter2x ]\n' > "$work/check.sh"
chmod +x "$work/check.sh"
printf 'auth required pam_exec.so expose_authtok quiet %s\naccount required pam_permit.so\n' "$work/check.sh" > "$work/pam.d/yoke"

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 -subj "/CN=service" \
  -addext subjectAltName=IP:127.0.0.1 -keyout "$work/state/tls/service.key" -out "$work/state/tls/service.crt" 2> /dev/null \
  || fail "openssl made no certificate"
chmod 600 "$work/state/tls/"*
port="$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')"
printf 'state_dir: %s/state\nruntime_dir: %s/run\nplugins:\n  manifests: %s/plugins.d\n  executables: %s/plugins\n' \
  "$work" "$work" "$work" "$work" > "$work/core.yaml"
mkdir -p "$work/plugins.d" "$work/plugins"
cat > "$work/deployment.yaml" <<YAML
units: {}
channels:
  service:
    transport: local
    clients: multiple
    address: { class: routable, host: 127.0.0.1, port: "$port", security: { transport: encrypted, caller: authenticated } }
YAML
YOKE_CONFIG="$work/core.yaml" YOKE_AUTH_STACKS="$work/pam.d" "$work/yoke-core" --composition "$work/deployment.yaml" > "$work/core.log" 2>&1 &
core=$!
waited=0
until grep -q 'msg=channel.*name=service' "$work/core.log" || (( waited >= 30 )); do sleep 1; ((waited++)); done
grep -q 'msg=channel.*name=service' "$work/core.log" || fail "the Core bound no channel: $(tail -5 "$work/core.log")"

YOKE_L3_ADDRESS="127.0.0.1:$port" YOKE_L3_ROOTS="$work/state/tls/service.crt" "$root/build/published_test" \
  | tee "$results/routable.txt"
exit "${PIPESTATUS[0]}"
