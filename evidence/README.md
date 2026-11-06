# Evidence record

These are locally executed results, not a claim of hosted CI, production deployment, an external employer endorsement, or machine power-loss testing. Absolute project paths in logs have been normalized to `<project>` for portability; result text is otherwise retained.

## Current verification

| Evidence | What it establishes |
| --- | --- |
| [test.log](test.log) | Optimized macOS build, 528 native index/scan comparisons, and all 19 process/socket/file tests |
| [sanitize.log](sanitize.log) | Same native comparisons and 19 E2E tests with AddressSanitizer and UndefinedBehaviorSanitizer |
| [thread-sanitize.log](thread-sanitize.log) | All 19 E2E tests against a ThreadSanitizer-instrumented server |
| [linux-build.log](linux-build.log), [linux-environment.log](linux-environment.log) | Actual Ubuntu 24.04 / clang 18.1.3 / Linux aarch64 container build and environment |
| [linux-test.log](linux-test.log) | 528 native comparisons and 19 E2E tests in Linux, with container networking disabled, 2 CPUs, and 512 MiB memory limit |
| [linux-sanitize.log](linux-sanitize.log) | Linux ASan/UBSan native comparisons and 19 E2E tests under the same container limits |
| [benchmark.json](benchmark.json), [benchmark.log](benchmark.log) | Three runs of 250,000 records and 1,000 matched index/scan queries, plus native and TCP fsync-per-record costs |
| [crash-demo.json](crash-demo.json), [crash-demo.log](crash-demo.log) | A real SIGKILL, 100 acknowledged records checked after restart, torn-tail recovery, and fatal complete-frame corruption |
| [verification.json](verification.json) | Machine-readable final command results and source/test hashes |
| [independent-review.md](independent-review.md) | Separate agent's code review, focused probes, and disposition of findings |

`make sanitize` and `make thread-sanitize` use separate executables. Benchmarks read each linked binary's generated build metadata, verify its binary and compiled-source hashes, and record its actual compiler and flags. This avoids incorrectly labeling a reused sanitizer executable as an optimized build.

## Retained failures and limitations

- [tdd-red.log](tdd-red.log) records the initial 12 failing behavioral specifications before the implementation existed. Their first failure was the missing executable. It is evidence of test-first specification and unavailable functionality, not proof that every later assertion was independently observed failing.
- [initial-test.log](initial-test.log) records the first 12 passing tests and a signedness warning. The conversion was made explicit; the current optimized build has no compiler warnings.
- [overload-test-failure.log](overload-test-failure.log) records a harness assumption exposed by repeated overload testing: a connection can be reset during its handshake before the application can reply `BUSY`. The test now first establishes ownership of the active worker, treats kernel refusal/reset as overload rejection, and always closes opened sockets. Network-deadline testing is a separate case. The revised tests pass on both macOS and Linux.
- [linux-build-first-attempt.log](linux-build-first-attempt.log) records that the installed Docker CLI uses its legacy builder and rejects `--progress`. The runnable documented command uses `docker build` without that unsupported flag.
- [linux-sanitize-missing-runtime.log](linux-sanitize-missing-runtime.log) records the initial missing ASan runtime in the minimal Ubuntu image. Dockerfile, CI, and installation instructions now explicitly include `libclang-rt-18-dev`; the final Linux sanitizer run passes.

SIGKILL leaves the OS running, including its page cache. It proves process-restart behavior, not that removing `fsync` would be detected by that experiment. The source's sync-before-publication ordering, explicit buffered watermark test, and reported OS-level durability contract should be assessed together. Physical power loss, I/O hardware failures, starvation fairness, and arbitrary untested interleavings remain outside the evidence.
