# Storage, concurrency, and failure boundaries

## Request path

```text
loopback accept
    │ bounded pending connection queue
    ▼
fixed worker threads ─── strict line parser + network deadlines
    │
    ├─ RANGE ── shared index lock ── stream map.lower_bound(from, 0)
    │                              └─ copy up to limit rows through inclusive upper bound
    │
    └─ PUT ─── exclusive index lock
                 ├─ validate/cap check
                 ├─ append one framed WAL record
                 ├─ fsync (always policy)
                 ├─ publish owned record and ordered index pointer
                 └─ unlock, then send acknowledgement
```

The linearization point of a successful write is its index publication while holding the exclusive lock. Durability precedes that publication in `always` mode. Read calls hold a shared lock for traversal and copying, then release it before formatting/network transmission. Slow network readers do not hold the storage lock. A writer can block readers while `fsync` executes; there is no group commit, lock-free structure, or claim of reader/writer fairness.

The engine owns records in a vector of `unique_ptr<Record>`. Each stream owns a `map<(timestamp, sequence), const Record*>`; pointees remain stable when the vector grows. Stream lookup costs `O(log S)`, range positioning `O(log Ns)`, and result collection `O(K)`, where `S` is stream count, `Ns` that stream's record count, and `K` the returned row count. Appending costs ordered insertion plus WAL I/O. Recovery scans the WAL and reinserts each record, so total reconstruction includes index insertion cost. The benchmark's baseline scans all `N` owned records, sorts its matching rows, and applies the same limit; it is intentionally a straightforward full-scan implementation.

If append, sync, or post-WAL allocation throws, the engine enters a poisoned state. It returns no successful acknowledgement for that operation and refuses subsequent storage operations. It cannot safely know whether a failed filesystem call committed all, some, or none of the record. Restart validates the WAL and rebuilds a fresh index. A valid but unacknowledged frame may reappear; callers requiring exactly-once writes need an additional idempotency design.

## WAL version 1

All integer fields are unsigned little-endian. CRC32C uses the Castagnoli polynomial, reflected form `0x82f63b78`, initial value `0xffffffff`, final XOR `0xffffffff`. CRC protects accidental damage; it is not authentication.

The 16-byte file header:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | ASCII `STRATA01` |
| 8 | 4 | Format version, currently 1 |
| 12 | 4 | CRC32C over bytes 0–11 |

Each record frame:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII `REC1` |
| 4 | 4 | Total frame size including both checksums |
| 8 | 8 | Global sequence, starting at 1 with no gaps |
| 16 | 8 | Timestamp, at most `INT64_MAX` |
| 24 | 4 | Stream byte count, 1–64 |
| 28 | 4 | Value byte count, 1–1,024 |
| 32 | 4 | CRC32C over frame bytes 0–31 |
| 36 | variable | ASCII stream followed immediately by raw value bytes |
| end−4 | 4 | CRC32C over all prior bytes in the frame |

The fixed metadata checksum is verified before lengths can trigger an allocation or guide recovery. A frame is at most 1,128 bytes. `write_all` loops over short writes and retries `EINTR`. `pread` is used for replay so read position cannot interfere with append mode. The encoder writes explicit byte order, not an in-memory C++ struct with platform-dependent padding.

The Python suite contains a literal independently generated frame for stream `cpu`, sequence 1, timestamp 7, and value bytes `00ff`. Its reference checksum computation was checked against the CRC32C `123456789 → e3069283` known vector. This avoids relying only on encoder/decoder agreement.

## Recovery decisions

| Observation | Action |
| --- | --- |
| Zero-byte WAL | Initialize and sync a new file header |
| Nonzero incomplete or invalid file header | Fail startup, preserve file |
| Partial final frame header with matching magic prefix | Truncate to last complete frame, fsync |
| Complete valid frame header, incomplete body/trailer | Truncate to last complete frame, fsync |
| Bad magic, complete header checksum, lengths, complete frame checksum, stream, timestamp, or sequence | Fail startup, preserve file |
| More recovered records than configured capacity | Fail startup, preserve file |

Incomplete final headers cannot prove whether bytes were lost or damaged. The rule is explicit; there is no claim of detecting arbitrary corruption when the bytes needed for verification are missing. Complete frame corruption is never treated as an innocuous torn tail, including at EOF. There is no automatic search for a later frame magic or silent skipping of damaged records. Recovery reads the complete valid prefix before truncating any incomplete suffix.

A crash during initial nonempty header creation can leave a partial header that requires operator inspection. This project favors stopping over guessing how to repair that rare state. An empty newly created file can be initialized. Creation and recovered truncation are synced. On recovery, a final `fsync` establishes a fresh durable watermark for valid buffered records that survived the previous process.

## Process ownership and filesystem model

The process opens the database directory and uses `openat` for `writer.lock` and `data.wal`, refusing symlinked leaf files and checking that both are regular files. It holds a nonblocking exclusive `flock` on the lock file throughout the engine lifetime. The lock file is never unlinked, avoiding replacement-inode lock races between cooperating processes. File descriptors close through RAII even if construction fails. The lock is released by the OS after SIGKILL.

Database creation syncs parent directories so the newly created path is persisted, and syncs the database directory after opening the files. The WAL header is synced before readiness. This is a local POSIX filesystem contract, not a guarantee for NFS, malicious local filesystem mutation, or multiple hard-linked paths to one WAL. It also does not emulate sudden power failure, defective hardware, or device-level flush semantics.

## Network admission and shutdown

A single accept loop owns the listening socket. It places accepted sockets into a bounded deque; fixed workers service one connection at a time. When the deque is full, the accept loop attempts a nonblocking `ERR BUSY` and closes the socket. The OS listen backlog is also configured to the queue size; kernel rejection may occur before application rejection. At most `workers + queue + 1` accepted application descriptors exist at once (the extra descriptor is being admitted/rejected), excluding the listener and storage descriptors.

Each worker maintains a bounded receive buffer, preserving bytes across partial reads and preserving subsequent commands after a newline. A command's network input deadline is absolute; byte-by-byte drip traffic cannot reset it. Output uses nonblocking sends with its own absolute deadline. The maximum query response is bounded by the row and value caps. A slow reader can occupy a worker until its output deadline, but cannot block the accept loop or hold the index lock.

SIGINT/SIGTERM set a signal-safe flag. The accept loop observes it within its poll interval. Shutdown closes queued sockets, calls `shutdown` on active connections to release socket waits, wakes idle workers, and joins threads. It can still wait on an in-progress filesystem call. SIGPIPE is ignored so disconnects become send failures instead of terminating the process.

## Evidence interpretation

The checked-in benchmark reports three complete local runs, including query percentiles, WAL size, replay time, peak process RSS, native durable write latency, and persistent-connection TCP durable latency. Inputs are deterministic. Query order alternates index-first and scan-first; both return copied records. Results are compared outside the timed sections. This controls major implementation mistakes, while leaving real limitations: one machine, warm RAM, no concurrent benchmark clients, OS page/device caches, allocator behavior, and other local processes can influence results.

The tests validate a six-writer/read workload, preservation of all 180 acknowledgements after SIGKILL, stream isolation, duplicate ordering, exact boundaries, parsing limits, path rejection, exclusive writer ownership, queue overload, deadlines, binary payloads, and partial file-write failure. ASan/UBSan provide memory/undefined-behavior checks, and a separate ThreadSanitizer run exercises the real server's threads. Sanitizers cover observed executions and are not formal proofs. A Linux CI recipe is supplied, but only observed commands are recorded as completed evidence.
