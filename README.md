# Strata

A C++20 telemetry store with a real TCP service, checksummed write-ahead log, explicit durability boundaries, and ordered time-range queries. It is a small, inspectable systems project: the difficult behavior is in crash recovery, storage errors, concurrency, and resource admission.

Strata runs on one machine. Each database directory has one writer process. It uses only the C++ standard library and POSIX system calls; the test and demonstration clients use Python's standard library.

## Run

Requirements: clang++ with C++20, Make, Python 3.10+, and a local POSIX filesystem. Linux and macOS are supported targets. The checked-in evidence was measured on the environment recorded in [benchmark.json](evidence/benchmark.json); the Linux workflow is supplied for reproduction and is not a claim that hosted CI has run.

On Ubuntu 24.04, install `clang make python3 libclang-rt-18-dev`; the last package supplies the sanitizer runtimes. macOS can use the clang++ included with Xcode Command Line Tools.

```sh
make
./build/strata serve --dir /tmp/strata-demo --port 7201
```

In another terminal:

```sh
python3 scripts/client.py 'PUT temperature 1700000000000 32312e35'
python3 scripts/client.py 'PUT temperature 1700000001000 32322e30'
python3 scripts/client.py 'RANGE temperature 1700000000000 1700000001000 100'
python3 scripts/client.py 'STATS'
```

The server prints `READY <port>` after locking and recovering storage. `--port 0` selects a free port. It binds to **127.0.0.1 only**. Stop it with Ctrl-C, restart with the same directory, and the records remain. Keep the directory to preserve data; choose a fresh directory to start an independent database.

## Verify

```sh
make test                          # Native comparisons and real process/socket/file tests
make sanitize                      # AddressSanitizer + UndefinedBehaviorSanitizer
make thread-sanitize               # ThreadSanitizer against the real TCP/process suite
python3 scripts/crash_demo.py       # Actual SIGKILL/restart, torn tail, complete-frame corruption
make benchmark                     # Three runs; 250k records, 1k queries, 1k durable writes per path
```

`make benchmark` writes [evidence/benchmark.json](evidence/benchmark.json), including the environment, actual build metadata and binary hashes, source hashes, all run results, and methodology. Sanitizer targets use separate executables and preserve optimized builds. Each indexed result is compared with the scan result **field for field**, including duplicate timestamps and sequence order. The scan traverses the entire in-memory record set. These measurements describe warm in-memory reads; they are not cold disk query measurements. Dataset loading uses buffered writes followed by explicit sync. Separate native and TCP write measurements use one `fsync` per record.

The process-crash demonstration writes 100 acknowledged records, kills the actual process with SIGKILL, verifies every recovered record, appends an incomplete tail, and then damages a complete frame. Its machine-readable record is [crash-demo.json](evidence/crash-demo.json). The test suite additionally exercises every byte boundary of a truncated final frame and a real partial `write` failure induced with `RLIMIT_FSIZE`.

For an isolated Linux run, build the image and run its tests with networking disabled (the image build downloads the compiler):

```sh
docker build -t strata-check .
docker run --rm --network none --cpus 2 --memory 512m strata-check
```

## Recorded local measurements

Three optimized runs on an Apple M4 Pro / Darwin 27.0.0 / Apple clang 21, using 250,000 records over 128 streams with 32-byte values:

| Measurement | Recorded result |
| --- | ---: |
| Median of indexed query p50s | 7.834 µs |
| Median of full-scan query p50s | 1,844.042 µs |
| Median of the three per-run p50 speedup ratios | 246× |
| Query results checked | 1,000 queries / 17,079 returned rows per run, identical |
| Median native fsync-per-record throughput | 15,302 writes/s |
| Median loopback TCP fsync-per-record throughput, one client | 6,808 writes/s |
| Replay time for 250,000 records | 0.550–0.663 s |
| Peak native benchmark process RSS | 76.2–78.6 MiB |

These are warm-memory queries and single-client, 1,000-write batches on one local machine, not sustained-load capacity measurements. Other local processes can influence results. The short write latency reflects this OS/filesystem's `fsync` behavior; it does not establish a device power-loss durability rate. All raw runs and percentiles are in [benchmark.json](evidence/benchmark.json), and the benchmark aborts on any result mismatch.

## Protocol

UTF-8/ASCII command lines terminated by `\n`; responses also use `\n`. TCP packet boundaries have no significance. A connection may carry multiple commands, including pipelined commands, which execute in order. There is no request multiplexing within a connection.

| Command | Response |
| --- | --- |
| `PING` | `PONG` |
| `PUT <stream> <timestamp> <hex-value>` | `OK <sequence>` |
| `RANGE <stream> <from> <through> <limit>` | `DATA <count>`, then `ROW <sequence> <timestamp> <hex-value>` for each row, then `END` |
| `SYNC` | `OK <durable-sequence>` |
| `STATS` | One JSON object with storage and connection counters |

Streams contain 1–64 ASCII letters, digits, `_`, `-`, or `.`. Values contain 1–1,024 arbitrary bytes, encoded as an even-length hexadecimal token. Responses normalize hex to lowercase. Timestamps are unsigned decimal integers in `[0, 9223372036854775807]`; their unit is the caller's choice, used consistently within a stream. Negative, signed, overflowing, or partially parsed numbers are rejected.

Range bounds are inclusive, `from <= through`, and the limit is 1–10,000. Results sort by `(timestamp, sequence)`; duplicate timestamps are preserved. A range call sees one consistent index snapshot. Missing streams return `DATA 0` followed by `END`. Writes have globally increasing sequences that survive restart. `PUT` is append-only and does not deduplicate retries.

Errors begin with `ERR INVALID`, `ERR STORAGE`, `ERR TOO_LARGE`, `ERR TIMEOUT`, or `ERR BUSY`. Malformed complete commands leave a connection usable. Oversized commands and network timeouts close it. Overload rejection is best effort: the client can also observe a reset/refusal at the kernel backlog. After an uncertain storage failure, storage operations return `STORAGE_UNAVAILABLE` until restart; the server does not append past a possibly torn record.

## Durability and recovery

The default `--durability always` executes WAL append → `fsync` → index publication → acknowledgement, under the writer lock. An acknowledged write has crossed the OS `fsync` boundary. The directory entries are synced when created. The project does **not** claim power-loss testing or guarantees beyond the filesystem, OS, and device's implementation of `fsync`; macOS `F_FULLFSYNC` is not used.

`--durability buffered` acknowledges an OS write without per-record `fsync`. `SYNC` establishes a durable sequence boundary for all prior published writes. Buffered acknowledgements may be lost after a machine crash. Graceful shutdown does not implicitly sync buffered writes. A valid record can survive even if the client did not receive an acknowledgement; a retry may therefore create a duplicate.

At startup, Strata verifies the file header and every complete frame, rebuilds the in-memory index, and restores sequence numbers. Only an incomplete terminal frame is truncated. An invalid magic, header checksum, complete-frame checksum, sequence, or length is fatal and preserves the WAL for inspection. An incomplete header has an inherent ambiguity: missing bytes prevent verifying its checksum, so a valid magic prefix at the final partial header is treated as a torn append. See [ARCHITECTURE.md](ARCHITECTURE.md) for the exact disk format and failure model.

## Resource bounds and tradeoffs

| Setting | Default | Accepted range |
| --- | ---: | ---: |
| Worker threads / active serviced connections | 4 | 1–64 |
| Pending connection queue | 64 | 1–4,096 |
| Command and response network timeout | 2,000 ms | 1–60,000 ms |
| Total stored records | 1,000,000 | 1–10,000,000 |
| Command bytes before newline | 2,304 | Fixed |
| Value bytes per record | 1,024 | Fixed |
| Rows returned per range | 10,000 | Fixed |

CLI flags are `--workers`, `--queue`, `--timeout-ms`, and `--max-records`. All allocation-dependent limits should be selected for the host's memory. Records and the full ordered index reside in memory. The WAL grows until the record cap is reached; there is no deletion, retention, compaction, compression, or pagination token. The result cap and payload cap bound a response, but a maximum-size response can still consume roughly 21 MB per active worker in addition to copied records.

Threads share a `std::shared_mutex`: readers can run concurrently; writes serialize WAL and index publication. The connection queue, pending input, and output duration are bounded. A command's input deadline starts when its connection is accepted (including queue time), and receiving another byte does not extend that command deadline. Network deadlines do not interrupt a blocking filesystem operation. Active clients can occupy workers indefinitely by making continuous valid requests; this is not a fair multitenant scheduler.

No authentication, TLS, replication, distributed consensus, or remote deployment is provided. The service is intended for trusted loopback clients and an exclusively owned local directory. Symlinked WAL/lock files and nonregular storage files are rejected; hostile local users, network filesystems, and hard-link aliasing are outside the supported storage model.

## What to inspect in an interview

- [engine.cpp](src/engine.cpp): on-disk framing, checksums, replay classification, fail-closed storage errors, process ownership, and range index.
- [server.cpp](src/server.cpp): partial TCP input, bounded queue, connection admission, absolute deadlines, graceful shutdown, and parser validation.
- [test_e2e.py](tests/test_e2e.py): real SIGKILL, bit corruption, writer exclusion, concurrent readers/writers, and partial disk writes.
- [benchmark.cpp](src/benchmark.cpp): identical-result checks, alternating measurement order, explicit sync costs, and peak process RSS.

The central tradeoff is inspectability: one append-only WAL, one process lock, and one shared index lock make publication and recovery understandable. The resulting serialized durable write path, full-memory index, and replay cost are deliberate limits that the evidence makes visible.
