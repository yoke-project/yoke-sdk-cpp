#!/usr/bin/env bash
# Installs, at a stated version, the tools this repository's verbs use beyond its language toolchain.
set -euo pipefail
python -m pip install --disable-pip-version-check clang-format==18.1.8
# The generators definitions/record names, as ubuntu-24.04 carries them, and the headers they compile
# against.
sudo apt-get install -y --no-install-recommends protobuf-compiler protobuf-compiler-grpc libprotobuf-dev libgrpc++-dev
