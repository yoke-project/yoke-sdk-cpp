#!/usr/bin/env bash
# The checks described by the-source-release.std.md, one function per case.
# Each function prints why it failed and returns non-zero; checks/run.sh reports them.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# published_prefix builds the family once and installs it under a prefix of its own, printing the prefix.
published_prefix() {
  if [[ -z "${published_installed:-}" ]]; then
    published_installed="$(mktemp -d)"
    cmake -S "$root" -B "$published_installed/build" -DCMAKE_BUILD_TYPE=Release > /dev/null &&
      cmake --build "$published_installed/build" --parallel --target yoke-cpp > /dev/null &&
      cmake --install "$published_installed/build" --prefix "$published_installed/prefix" > /dev/null ||
      { echo "the family does not build and install" >&2; return 1; }
  fi
  echo "$published_installed/prefix"
}

# published_program writes a program calling the library into a directory, printing the directory.
published_program() {
  local dir
  dir="$(mktemp -d)"
  cat > "$dir/main.cpp" <<'EOF'
#include <cstdio>
#include <yoke/plugin.hpp>
int main() { std::printf("contract %d\n", yoke::plugin_contract()); return 0; }
EOF
  echo "$dir"
}

# std: yoke-sdk-cpp:the-source-release.01
check_installing_puts_the_package_under_the_prefix() {
  local prefix lib
  prefix="$(published_prefix)" || return 1
  lib="$(find "$prefix" -name libyoke-cpp.a | head -n1)"
  [[ -n "$lib" ]] || { echo "no libyoke-cpp.a under the prefix"; return 1; }
  for f in include/yoke/base.hpp include/yoke/plugin.hpp; do
    [[ -f "$prefix/$f" ]] || { echo "no $f under the prefix"; return 1; }
  done
  [[ -n "$(find "$prefix" -name yoke-sdk-cpp-config.cmake)" ]] || { echo "no package config"; return 1; }
  [[ -n "$(find "$prefix" -name yoke-sdk-cpp-config-version.cmake)" ]] || { echo "no package version file"; return 1; }
  [[ -n "$(find "$prefix" -name yoke-sdk-cpp.pc)" ]] || { echo "no pkg-config file"; return 1; }
  [[ -z "$(find "$prefix" -name '*.pb.h')" ]] || { echo "a generated definition header is installed"; return 1; }
}

# std: yoke-sdk-cpp:the-source-release.02
check_a_cmake_build_resolves_it() {
  local prefix dir
  prefix="$(published_prefix)" || return 1
  dir="$(published_program)"
  cat > "$dir/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.25)
project(consumer LANGUAGES CXX)
find_package(yoke-sdk-cpp REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE yoke::yoke-cpp)
EOF
  cmake -S "$dir" -B "$dir/build" -DCMAKE_PREFIX_PATH="$prefix" > /dev/null 2>&1 &&
    cmake --build "$dir/build" > /dev/null 2>&1 || { echo "a CMake consumer does not build against the package"; return 1; }
  "$dir/build/consumer" | grep -qE '^contract [0-9]+$' || { echo "the consumer did not print the contract"; return 1; }
}

# std: yoke-sdk-cpp:the-source-release.03
check_any_other_build_resolves_it() {
  local prefix dir pc flags
  prefix="$(published_prefix)" || return 1
  dir="$(published_program)"
  pc="$(dirname "$(find "$prefix" -name yoke-sdk-cpp.pc | head -n1)")"
  flags="$(PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --cflags --libs yoke-sdk-cpp)" ||
    { echo "pkg-config does not resolve yoke-sdk-cpp"; return 1; }
  # shellcheck disable=SC2086
  c++ -std=c++17 -o "$dir/consumer" "$dir/main.cpp" $flags 2> /dev/null || { echo "a program does not build with the flags pkg-config gives"; return 1; }
  "$dir/consumer" | grep -qE '^contract [0-9]+$' || { echo "the consumer did not print the contract"; return 1; }
}

# std: yoke-sdk-cpp:the-source-release.04
check_the_family_states_one_version() {
  local stated line prefix configured pc
  stated="$(sed -n 's/^project(yoke-sdk-cpp VERSION \([0-9.]*\) .*/\1/p' "$root/CMakeLists.txt")"
  [[ -n "$stated" ]] || { echo "CMakeLists.txt states no version"; return 1; }
  line="$(sed -n 's/^inline constexpr const char \*sdk_line = "yoke-sdk-cpp \(.*\)";$/\1/p' "$root/include/yoke/base.hpp")"
  [[ "$line" == "$stated" ]] || { echo "base.hpp says $line and CMakeLists.txt $stated"; return 1; }
  prefix="$(published_prefix)" || return 1
  configured="$(sed -n 's/^set(PACKAGE_VERSION "\(.*\)")$/\1/p' "$(find "$prefix" -name yoke-sdk-cpp-config-version.cmake | head -n1)")"
  [[ "$configured" == "$stated" ]] || { echo "the package config carries $configured"; return 1; }
  pc="$(sed -n 's/^Version: //p' "$(find "$prefix" -name yoke-sdk-cpp.pc | head -n1)")"
  [[ "$pc" == "$stated" ]] || { echo "the pkg-config file carries $pc"; return 1; }
}

# std: yoke-sdk-cpp:the-source-release.05
check_a_release_tag_hands_the_source_over() {
  local body
  body="$(cd "$root" && just --show release)"
  [[ "$body" == *"cmd/yoke-release@"* && "$body" == *"-source yoke-sdk-cpp"* ]] ||
    { echo "the release verb does not run the published release verb with -source yoke-sdk-cpp"; return 1; }
  local workflow="$root/.github/workflows/release.yml"
  [[ -f "$workflow" ]] || { echo "there is no release workflow"; return 1; }
  grep -q "tags: \['v\*'\]" "$workflow" || { echo "the release workflow does not run on a v tag"; return 1; }
  grep -q 'just release > manifest-lines.jsonl' "$workflow" || { echo "the release workflow does not keep the lines"; return 1; }
}
