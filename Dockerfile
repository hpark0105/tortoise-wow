FROM ubuntu:22.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates libace-dev \
    libmysqlclient-dev libssl-dev zlib1g-dev libbz2-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY dep ./dep
COPY modules ./modules
COPY tools ./tools
ARG BUILD_JOBS=2
RUN --mount=type=cache,target=/build cmake -S . -B /build -G Ninja \
    -DCMAKE_INSTALL_PREFIX=/opt/tortoise -DCMAKE_BUILD_TYPE=Release \
    -DUSE_EXTRACTORS=ON -DUSE_DISCORD_BOT=OFF -DUSE_LIBCURL=OFF \
    -DALLOW_TURTLE_ADDONS=ON \
    -DMODULES=static -DUSE_PCH=OFF \
    && cmake --build /build --parallel ${BUILD_JOBS} \
    && cmake --install /build

FROM ubuntu:22.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libace-7.0.6 libmysqlclient21 libssl3 zlib1g libbz2-1.0 python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home tortoise \
    && mkdir -p /state/logs /state/honor /state/pdump /state/patches \
    && chown -R tortoise:tortoise /state
COPY --from=build /opt/tortoise /opt/tortoise
# The module updater uses this compile-time source path.
COPY --from=build /source/modules /source/modules
COPY sql/database_updates /opt/tortoise/sql/database_updates
COPY tools/dbc_verification /opt/tortoise/dbc_verification
COPY docker/server.py docker/verify_dbc.py /opt/tortoise/
ENV PATH="/opt/tortoise/bin:${PATH}"
WORKDIR /state
USER tortoise
ENTRYPOINT ["python3", "/opt/tortoise/server.py"]
CMD ["world"]
