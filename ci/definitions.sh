#!/usr/bin/env bash
# Generates the C++ definitions, messages and gRPC services, from the published definitions module, as
# definitions/record pins it.
#
# The module's zip is fetched from the Go module proxy and refused unless its h1: digest — the SHA-256 of
# the sorted list of every file's SHA-256 and name, in base64 — is the one the record names and the one
# the release manifest publishes for that version. The generators must be the versions the record names.
# Every contract the record names is generated, messages and services; the well-known types it imports
# are libprotobuf's own. A contract the generator cannot express is refused by name.
#
# Usage: definitions.sh generate [directory]   writes the tree, by default into definitions/
#        definitions.sh check                  regenerates, and fails on any difference from definitions/
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
record="${DEFINITIONS_RECORD:-$root/definitions/record}"
manifest="${YOKE_MANIFEST:-https://raw.githubusercontent.com/yoke-project/yoke/main/releases/manifest.jsonl}"
proxy="${YOKE_PROXY:-https://proxy.golang.org}"
mode="${1:-}"

field() { awk -v key="$1" '$1 == key { $1 = ""; sub(/^ /, ""); print }' "$record"; }

[[ -f "$record" ]] || { echo "definitions: there is no record at $record"; exit 1; }
module="$(field module)" version="$(field version)" digest="$(field digest)" contracts="$(field contracts)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The manifest publishes the module at that version with that digest.
lines="$(curl -fsSL "$manifest")" || { echo "definitions: the release manifest cannot be read from $manifest"; exit 1; }
line="$(grep -F "\"published\":\"$module\"" <<<"$lines" | grep -F "\"version\":\"$version\"" | head -n 1 || true)"
[[ -n "$line" ]] || { echo "definitions: the manifest publishes no $module $version"; exit 1; }
grep -qF "\"$digest\"" <<<"$line" || { echo "definitions: the manifest publishes $module $version with another digest than the record's $digest"; exit 1; }

# The zip is the one published.
curl -fsSL -o "$work/module.zip" "$proxy/$module/@v/$version.zip" || { echo "definitions: $module $version cannot be fetched from $proxy"; exit 1; }
mkdir "$work/zip"
(cd "$work/zip" && unzip -q "$work/module.zip")
computed="h1:$(cd "$work/zip" && find . -type f | sed 's|^\./||' | LC_ALL=C sort |
  while read -r f; do printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done |
  openssl dgst -sha256 -binary | base64)"
if [[ "$computed" != "$digest" ]]; then
  echo "definitions: $module $version is not the module published (computed $computed, published $digest); nothing generated"
  exit 1
fi
source="$work/zip/$module@$version"

# The generators are the ones recorded.
found="$(protoc --version 2>/dev/null | awk '{print $2}')" || true
[[ "$found" == "$(field protoc)" ]] || { echo "definitions: protoc ${found:-is not on PATH}${found:+ is not the version recorded}, the record names $(field protoc)"; exit 1; }
# grpc_cpp_plugin says no version of its own: the package that installed it does.
found="$(dpkg-query -W -f '${Version}' protobuf-compiler-grpc 2>/dev/null | sed -E 's/^([0-9.]+).*/\1/')" || true
command -v grpc_cpp_plugin > /dev/null || found=""
[[ "$found" == "$(field grpc_cpp_plugin)" ]] || { echo "definitions: grpc_cpp_plugin ${found:-is not on PATH, or not from a package}${found:+ is not the version recorded}, the record names $(field grpc_cpp_plugin)"; exit 1; }
include="$(dirname "$(command -v protoc)")/../include"

# Every contract the record names, messages and services.
generate() {
  local out="$1" contract files
  for contract in $contracts; do
    files="$(cd "$source" && { find "yoke/$contract" -name '*.proto' 2>/dev/null || true; } | LC_ALL=C sort)"
    [[ -n "$files" ]] || { echo "definitions: $module $version has no contract $contract"; return 1; }
    mkdir -p "$work/contract"
    # shellcheck disable=SC2086
    if ! said="$(protoc -I "$source" -I "$include" --cpp_out="$work/contract" --grpc_out="$work/contract" \
      --plugin=protoc-gen-grpc="$(command -v grpc_cpp_plugin)" $files 2>&1)"; then
      echo "definitions: the generator cannot express the contract $contract: $said"
      return 1
    fi
    cp -r "$work/contract/." "$out/"
    rm -rf "$work/contract"
  done
}

case "$mode" in
  generate)
    out="${2:-$root/definitions}"
    mkdir -p "$out"
    find "$out" -mindepth 1 -maxdepth 1 ! -name record -exec rm -rf {} +
    generate "$out"
    echo "definitions: generated $contracts from $module $version"
    ;;
  check)
    mkdir "$work/out"
    generate "$work/out"
    if ! differs="$(diff -r --exclude=record "$work/out" "$root/definitions" 2>&1)"; then
      echo "definitions: the committed tree is not what $module $version generates:"
      echo "$differs" | head -n 20
      exit 1
    fi
    echo "definitions: the committed tree is what $module $version generates"
    ;;
  *)
    echo "usage: definitions.sh generate [directory] | check"
    exit 2
    ;;
esac
