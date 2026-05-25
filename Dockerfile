# syntax=docker/dockerfile:1
ARG AASDK_REF=newdev

FROM debian:bookworm-slim AS aasdk-build
ARG AASDK_REF
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git \
    build-essential cmake \
    libboost-all-dev libusb-1.0-0-dev libssl-dev \
    libprotobuf-dev protobuf-compiler \
    libabsl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
RUN git clone --depth 1 --branch "${AASDK_REF}" https://github.com/opencardev/aasdk.git aasdk
WORKDIR /build/aasdk
RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j"$(nproc)" && \
    make install

FROM debian:bookworm-slim AS aa-handler-build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libboost-all-dev libusb-1.0-0-dev libssl-dev \
    libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*
COPY --from=aasdk-build /usr/local /usr/local
WORKDIR /build/aa-handler
COPY aa-handler/ ./
RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_WITH_AASDK=ON .. && \
    make -j"$(nproc)" && \
    make install

FROM node:20-bookworm-slim AS server-build
WORKDIR /build
COPY package.json ./
COPY server/package.json ./server/
RUN npm install --workspace=server
COPY server/ ./server/
COPY frontend/ ./frontend/
COPY scripts/ ./scripts/
RUN node scripts/generate-fixtures.js
RUN npm run build --workspace=server

FROM node:20-bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libusb-1.0-0 libssl3 libprotobuf32 \
    libboost-system1.74.0 libboost-log1.74.0 \
    curl usbutils ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=aasdk-build /usr/local/lib /usr/local/lib
COPY --from=aasdk-build /usr/local/bin /usr/local/bin
RUN ldconfig || true
COPY --from=aa-handler-build /usr/local/bin/aa-handler /usr/local/bin/aa-handler
COPY --from=server-build /build/server/dist /app/server/dist
COPY --from=server-build /build/server/package.json /app/server/package.json
COPY --from=server-build /build/node_modules /app/node_modules
COPY --from=server-build /build/frontend /app/frontend
COPY --from=server-build /build/scripts /app/scripts
COPY config/config.yaml /config/config.yaml
COPY scripts/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
WORKDIR /app
ENV CONFIG_PATH=/config/config.yaml
EXPOSE 8080
ENTRYPOINT ["/entrypoint.sh"]
