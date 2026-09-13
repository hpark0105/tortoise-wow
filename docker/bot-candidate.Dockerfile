# Build-only compatibility probe. Context must be the separately pinned candidate
# checkout, never the personal server worktree. No game data or credentials.
FROM ubuntu:22.04 AS configure
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates libace-dev \
    libmysqlclient-dev libssl-dev zlib1g-dev libbz2-dev \
    libboost-thread-dev libboost-filesystem-dev libboost-system-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY dep ./dep
COPY modules ./modules
COPY tools ./tools
COPY tests/architecture ./tests/architecture
RUN cmake -S . -B /build -G Ninja \
    -DCMAKE_INSTALL_PREFIX=/opt/tortoise -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_PLAYERBOTS=ON -DALLOW_TURTLE_ADDONS=ON \
    -DBUILD_ELUNA=OFF -DBUILD_ELUNA_TESTS=OFF \
    -DMODULES=disabled -DUSE_PCH=OFF -DENABLE_LTO=OFF \
    -DUSE_EXTRACTORS=OFF -DUSE_DISCORD_BOT=OFF -DUSE_LIBCURL=OFF

FROM configure AS checks
RUN cmake -S tests/architecture -B /checks -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build /checks --parallel 4 --target \
       BotCreationLifecycleTest BotMovementDispatchTest BotRetryIntegrationTest TravelRoutePolicyTest \
    && ctest --test-dir /checks --output-on-failure \
       -R '^(BotCreationLifecycleTest|BotMovementDispatchTest|BotRetryIntegrationTest|TravelRoutePolicyTest)$'

# Independent result: a broken upstream test configuration must be reported,
# while still allowing the actual server's compile compatibility to be measured.
FROM configure AS server
ARG BUILD_JOBS=4
RUN cmake --build /build --parallel ${BUILD_JOBS} --target mangosd
