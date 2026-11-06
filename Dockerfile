FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends clang libclang-rt-18-dev make python3 ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /work
COPY . .
RUN make all build/core-test build/strata-bench
CMD ["make", "test"]
