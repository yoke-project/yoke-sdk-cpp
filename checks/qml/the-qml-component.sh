#!/usr/bin/env bash
# The case of the-qml-binding.std.md that reaches the package as installed: a consumer's configuration
# alone outlasts the bound a case program holds its cases to, so it is checked here.
# Each function prints why it failed and returns non-zero; checks/run.sh reports them.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# std: yoke-sdk-cpp:the-qml-binding.08
check_a_consumer_finds_the_binding_as_a_component_of_the_package() {
  local dir prefix path
  dir="$(mktemp -d)"
  prefix="$dir/prefix"
  cmake -S "$root" -B "$dir/build" -DCMAKE_BUILD_TYPE=Release -DYOKE_SDK_CPP_QML=ON > /dev/null 2>&1 &&
    cmake --build "$dir/build" --parallel --target yoke-cpp-qml qml_load > /dev/null 2>&1 &&
    cmake --install "$dir/build" --prefix "$prefix" > /dev/null ||
    { echo "the family does not build and install with its QML binding"; return 1; }
  mkdir -p "$dir/consumer"
  cat > "$dir/consumer/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.25)
project(consumer LANGUAGES CXX)
find_package(yoke-sdk-cpp REQUIRED COMPONENTS qml)
message(STATUS "import path: ${YOKE_SDK_CPP_QML_IMPORT_PATH}")
CMAKE
  path="$(cmake -S "$dir/consumer" -B "$dir/consumer/build" -DCMAKE_PREFIX_PATH="$prefix" 2>&1 |
    sed -n 's/^-- import path: //p')"
  [[ -n "$path" ]] || { echo "the package was not found with its component qml, or named no import path"; return 1; }
  [[ "$path" == "$prefix"/* ]] || { echo "the import path $path is not under the prefix $prefix"; return 1; }
  printf 'import Yoke.Interface\nAttachment { active: false }\n' > "$dir/doc.qml"
  local out
  out="$(LC_ALL=C.UTF-8 "$dir/build/qml_load" "$path" "$dir/doc.qml" 2>&1)"
  [[ "$out" == loaded* ]] || { echo "a document importing Yoke.Interface did not load: $out"; return 1; }
}
