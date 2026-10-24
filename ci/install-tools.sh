#!/usr/bin/env bash
# Installs, at a stated version, the tools this repository's verbs use beyond its language toolchain.
set -euo pipefail
python -m pip install --disable-pip-version-check clang-format==18.1.8
# The runner image's package index names what was current when it was built, which the archive may since
# have superseded.
sudo apt-get update
# The generators definitions/record names, as ubuntu-24.04 carries them, the headers and libraries the
# library compiles against, and its build system.
sudo apt-get install -y --no-install-recommends protobuf-compiler protobuf-compiler-grpc libprotobuf-dev libgrpc++-dev libssl-dev nlohmann-json3-dev \
  pkg-config cmake
