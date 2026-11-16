# The container this repository defines for its own build.
# It installs a runner; the floor that runner must meet is declared by the workspace.
FROM docker.io/library/gcc:16-trixie
ARG JUST_VERSION=1.58.0
# The build system, and the headers and libraries the library compiles against: protobuf's, and gRPC's
# C++ API; and Qt 6's QML, which the QML binding is built with.
RUN apt-get update && apt-get install -y --no-install-recommends cmake pkg-config libprotobuf-dev libgrpc++-dev libssl-dev nlohmann-json3-dev \
  qt6-declarative-dev qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://just.systems/install.sh | bash -s -- --tag "${JUST_VERSION}" --to /usr/local/bin
WORKDIR /src
COPY . .
