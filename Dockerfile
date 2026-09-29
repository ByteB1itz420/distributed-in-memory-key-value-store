FROM node:22-bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target kvserver -j2

FROM node:22-bookworm-slim

ENV NODE_ENV=production \
    KV_HOST=127.0.0.1 \
    KV_PORT=6380

WORKDIR /app
COPY gateway/package*.json ./
RUN npm ci --omit=dev
COPY gateway/ ./
COPY --from=build /workspace/build/kvserver /usr/local/bin/kvserver

EXPOSE 8080
CMD ["node", "index.js"]
