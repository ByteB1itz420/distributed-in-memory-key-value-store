FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . /workspace
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"

EXPOSE 6380

CMD ["/workspace/build/kvserver", "--port", "6380", "--threads", "4", "--maxmemory", "512mb", "--maxmemory-policy", "allkeys-lru"]
