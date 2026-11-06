#!/usr/bin/env python3
"""Real sockets, processes, and files. No database or network doubles."""
import concurrent.futures
import json
import os
from pathlib import Path
import resource
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("STRATA_BIN", ROOT / "build/strata"))


class Server:
    def __init__(self, directory, *args, file_limit=None):
        assert BIN.is_file(), f"Missing Strata executable: {BIN}; server behavior is not implemented"
        def limit_file_size():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (file_limit, file_limit))
        self.proc = subprocess.Popen([str(BIN), "serve", "--dir", str(directory),
                                      "--port", "0", "--timeout-ms", "600", *args],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                     preexec_fn=limit_file_size if file_limit else None)
        ready, _, _ = select.select([self.proc.stdout], [], [], 5)
        line = self.proc.stdout.readline() if ready else ""
        if not line.startswith("READY "):
            self.proc.kill()
            _, err = self.proc.communicate(timeout=3)
            raise AssertionError(f"Server failed to become ready: {line!r} {err}")
        self.port = int(line.split()[1])

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=3)

    def command(self, cmd):
        with self.connect() as sock:
            sock.sendall((cmd + "\n").encode())
            with sock.makefile("rb") as fp:
                first = fp.readline().decode().rstrip("\n")
                if first.startswith("DATA "):
                    rows = [fp.readline().decode().strip().split() for _ in range(int(first.split()[1]))]
                    assert fp.readline() == b"END\n"
                    return [(int(row[1]), int(row[2]), row[3]) for row in rows]
                return first

    def stop(self, crash=False):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
        out, err = self.proc.communicate(timeout=5)
        if not crash:
            assert self.proc.returncode == 0, (self.proc.returncode, out, err)
        return err


class StrataTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="strata-test-")
        self.directory = Path(self.tmp.name) / "db"
        self.servers = []

    def tearDown(self):
        for server in self.servers:
            if server.proc.poll() is None:
                server.stop()
        self.tmp.cleanup()

    def server(self, *args):
        server = Server(self.directory, *args)
        self.servers.append(server)
        return server

    def expect_start_failure(self, *args):
        assert BIN.is_file(), "Strata executable is not implemented"
        result = subprocess.run([str(BIN), "serve", "--dir", str(self.directory),
                                 "--port", "0", *args], capture_output=True, text=True, timeout=4)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("READY", result.stdout)
        return result.stderr

    # Catches an unordered index, exclusive end bound, lost duplicate, or cross-stream leak.
    def test_order_duplicates_inclusive_bounds_and_limit(self):
        s = self.server()
        for cmd in ["PUT cpu 30 63", "PUT cpu 10 61", "PUT disk 20 78", "PUT cpu 20 62", "PUT cpu 20 64"]:
            self.assertTrue(s.command(cmd).startswith("OK "))
        self.assertEqual(s.command("RANGE cpu 10 20 10"), [(2, 10, "61"), (4, 20, "62"), (5, 20, "64")])
        self.assertEqual(s.command("RANGE cpu 20 30 1"), [(4, 20, "62")])
        self.assertEqual(s.command("RANGE missing 0 100 5"), [])

    # Catches accepting numeric coercion, overflowing timestamps, unsafe stream/value syntax.
    def test_validation_and_connection_remains_usable(self):
        s = self.server()
        invalid = ["PUT cpu -1 ff", "PUT cpu +1 ff", "PUT cpu 1x ff",
                   "PUT cpu 9223372036854775808 ff", "PUT cpu 184467440737095516160 ff",
                   "PUT ../cpu 1 ff", "PUT " + "x" * 65 + " 1 ff", "PUT cpu 1 f",
                   "PUT cpu 1 gg", "PUT cpu 1 " + "ff" * 1025, "PUT cpu 1 ff extra",
                   "RANGE cpu 2 1 1", "RANGE cpu 0 1 0", "RANGE cpu 0 1 10001",
                   "RANGE cpu -1 2 3", "PONG", "PUT cpu 1", "PING extra"]
        for command in invalid:
            self.assertTrue(s.command(command).startswith("ERR "), command)
        self.assertEqual(s.command("PUT cpu 9223372036854775807 00"), "OK 1")
        self.assertEqual(s.command("RANGE cpu 0 9223372036854775807 1"), [(1, 9223372036854775807, "00")])
        with s.connect() as sock, sock.makefile("rb") as fp:
            sock.sendall(b"PUT cpu -2 ff\nPING\n")
            self.assertTrue(fp.readline().startswith(b"ERR "))
            self.assertEqual(fp.readline(), b"PONG\n")

    # Catches acknowledged records lost across process restart, wrong order, or reset sequences.
    def test_sigkill_restart_preserves_acknowledged_records(self):
        s = self.server()
        for i in range(30):
            self.assertEqual(s.command(f"PUT cpu {29-i} {i:02x}"), f"OK {i+1}")
        s.stop(crash=True)
        s = self.server()
        expected = [(30-i, i, f"{29-i:02x}") for i in range(30)]
        self.assertEqual(s.command("RANGE cpu 0 29 100"), expected)
        self.assertEqual(s.command("PUT cpu 30 ff"), "OK 31")

    # Catches recovery discarding a committed record or retaining an incomplete append.
    def test_torn_tail_header_and_body_are_truncated(self):
        s = self.server()
        self.assertEqual(s.command("PUT cpu 1 abcd"), "OK 1")
        s.stop()
        wal = self.directory / "data.wal"
        good = wal.read_bytes()
        for suffix in [b"REC", good[16:16+36+2]]:
            wal.write_bytes(good + suffix)
            s = self.server()
            self.assertEqual(s.command("RANGE cpu 0 2 10"), [(1, 1, "abcd")])
            self.assertEqual(wal.read_bytes(), good)
            stats = json.loads(s.command("STATS"))
            self.assertEqual(stats["recovered_tail_bytes"], len(suffix))
            s.stop()

    # Catches silently skipping a corrupt complete frame, including the final frame.
    def test_checksum_corruption_is_fatal_and_preserves_file(self):
        s = self.server()
        s.command("PUT cpu 1 abcd")
        s.command("PUT cpu 2 ef01")
        s.stop()
        wal = self.directory / "data.wal"
        good = wal.read_bytes()
        for offset in [0, 16, 16+8, 16+36+3, len(good)-1]:
            damaged = bytearray(good)
            damaged[offset] ^= 0x40
            wal.write_bytes(damaged)
            self.assertIn("corrupt", self.expect_start_failure().lower())
            self.assertEqual(wal.read_bytes(), damaged)
        wal.write_bytes(good)

    # Catches two processes appending the same directory or inadvertently stealing its lock.
    def test_exclusive_writer_lock(self):
        s = self.server()
        self.assertIn("locked", self.expect_start_failure().lower())
        self.assertEqual(s.command("PUT cpu 1 ff"), "OK 1")
        s.stop()
        self.assertEqual(self.server().command("PUT cpu 2 ff"), "OK 2")

    # Catches loss of limits across restart and cap enforcement after WAL publication.
    def test_record_cap_and_restart_with_lower_cap(self):
        s = self.server("--max-records", "2")
        self.assertEqual(s.command("PUT cpu 1 ff"), "OK 1")
        self.assertEqual(s.command("PUT cpu 2 ff"), "OK 2")
        size = (self.directory / "data.wal").stat().st_size
        self.assertIn("CAPACITY", s.command("PUT cpu 3 ff"))
        self.assertEqual((self.directory / "data.wal").stat().st_size, size)
        s.stop()
        self.assertIn("capacity", self.expect_start_failure("--max-records", "1").lower())

    # Catches lost writes/racing sequences and invalid read snapshots while writers run.
    def test_concurrent_writes_and_reads(self):
        s = self.server("--workers", "8", "--queue", "64")
        def writer(w):
            sequences = []
            for n in range(30):
                response = s.command(f"PUT shared {w*30+n} {w:02x}")
                self.assertTrue(response.startswith("OK "), response)
                sequences.append(int(response.split()[1]))
                rows = s.command("RANGE shared 0 1000 1000")
                self.assertEqual(rows, sorted(rows, key=lambda row: (row[1], row[0])))
                self.assertEqual(len({row[0] for row in rows}), len(rows))
            return sequences
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            sequences = sum(pool.map(writer, range(6)), [])
        self.assertEqual(sorted(sequences), list(range(1, 181)))
        self.assertEqual(len(s.command("RANGE shared 0 1000 1000")), 180)
        s.stop(crash=True)
        self.assertEqual(len(self.server().command("RANGE shared 0 1000 1000")), 180)

    # Catches treating TCP recv boundaries as command boundaries and input-size omissions.
    def test_partial_pipelined_and_oversized_clients(self):
        s = self.server()
        with s.connect() as sock, sock.makefile("rb") as fp:
            sock.sendall(b"PUT cpu 1 a")
            time.sleep(0.03)
            sock.sendall(b"b\nPING\nRANGE cpu 0 2 3\n")
            self.assertEqual(fp.readline(), b"OK 1\n")
            self.assertEqual(fp.readline(), b"PONG\n")
            self.assertEqual(fp.readline(), b"DATA 1\n")
            self.assertEqual(fp.readline(), b"ROW 1 1 ab\n")
            self.assertEqual(fp.readline(), b"END\n")
        with s.connect() as sock:
            sock.sendall(b"x" * 5000)
            self.assertTrue(sock.recv(1024).startswith(b"ERR TOO_LARGE"))
        self.assertEqual(s.command("PING"), "PONG")

    # Catches unbounded accepted queues; establish worker ownership before the burst.
    def test_bounded_queue_rejects_excess_connections(self):
        s = self.server("--workers", "1", "--queue", "1", "--timeout-ms", "1000")
        sockets = []
        try:
            active = s.connect()
            sockets.append(active)
            active.sendall(b"PING\n")
            self.assertEqual(active.recv(1024), b"PONG\n")
            active.sendall(b"P")
            sockets.append(s.connect())
            rejected = 0
            for _ in range(10):
                try:
                    sock = s.connect()
                    sockets.append(sock)
                    if sock.recv(1024).startswith(b"ERR BUSY"):
                        rejected += 1
                except (ConnectionResetError, ConnectionRefusedError):
                    # Kernel backlog rejection may precede application-level BUSY.
                    rejected += 1
            self.assertGreaterEqual(rejected, 9)
        finally:
            for sock in sockets:
                sock.close()

    # Catches a drip client resetting its command deadline on each received byte.
    def test_absolute_partial_command_timeout(self):
        s = self.server("--workers", "1", "--queue", "1", "--timeout-ms", "300")
        self.assertEqual(s.command("PING"), "PONG")
        with s.connect() as sock:
            start = time.monotonic()
            sock.sendall(b"P")
            time.sleep(0.18)
            sock.sendall(b"I")
            response = sock.recv(1024)
            self.assertEqual(response, b"ERR TIMEOUT\n")
            self.assertLess(time.monotonic() - start, 0.55)

    # Catches following an unexpected WAL symlink and treating non-directories as databases.
    def test_invalid_storage_paths(self):
        self.directory.write_text("not a directory")
        self.assertNotEqual(self.expect_start_failure(), "")
        self.directory.unlink()
        self.directory.mkdir()
        target = Path(self.tmp.name) / "target"
        target.write_text("keep me")
        (self.directory / "data.wal").symlink_to(target)
        self.assertNotEqual(self.expect_start_failure(), "")
        self.assertEqual(target.read_text(), "keep me")

    # Catches opening special storage files without checking their actual inode type.
    def test_nonregular_storage_paths_fail_without_hanging(self):
        self.directory.mkdir()
        for name in ("writer.lock", "data.wal"):
            path = self.directory / name
            if path.exists():
                path.unlink()
            os.mkfifo(path)
            self.assertIn("regular file", self.expect_start_failure())
            path.unlink()

    # Catches an indefinitely blocked network send holding the only worker.
    def test_slow_reader_output_timeout_releases_worker(self):
        s = self.server("--workers", "1", "--durability", "buffered", "--timeout-ms", "300")
        with s.connect() as sock, sock.makefile("rb") as fp:
            for i in range(10000):
                sock.sendall(f"PUT bulk {i} {'ab'*1024}\n".encode())
                self.assertEqual(fp.readline(), f"OK {i+1}\n".encode())
        with s.connect() as slow:
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.sendall(b"RANGE bulk 0 9999 10000\n")
            time.sleep(0.6)  # Twice the network deadline, with no reads on the 20 MB response.
            self.assertEqual(s.command("PING"), "PONG")

    # Catches permissive CLI integer parsing and silent unknown-option fallback.
    def test_cli_rejects_invalid_options(self):
        for args in [("--port", "-1"), ("--workers", "0"), ("--queue", "0"),
                     ("--max-records", "0"), ("--timeout-ms", "0"), ("--wat", "5"),
                     ("--durability", "sometimes"), ("--port", "99999")]:
            self.expect_start_failure(*args)

    # Catches advancing the durable watermark without sync, or SYNC failing to flush a batch.
    def test_buffered_policy_has_explicit_sync_boundary(self):
        s = self.server("--durability", "buffered")
        self.assertEqual(s.command("PUT cpu 1 ff"), "OK 1")
        self.assertEqual(json.loads(s.command("STATS"))["durable_sequence"], 0)
        self.assertEqual(s.command("SYNC"), "OK 1")
        self.assertEqual(json.loads(s.command("STATS"))["durable_sequence"], 1)
        s.stop(crash=True)
        self.assertEqual(self.server().command("RANGE cpu 0 2 1"), [(1, 1, "ff")])

    # Catches continuing after a partial OS write and acknowledges nothing after failed storage.
    def test_partial_os_write_failure_poison_and_restart(self):
        s = Server(self.directory, file_limit=70)
        self.servers.append(s)
        self.assertEqual(s.command("PUT cpu 1 ff"), "OK 1")
        self.assertTrue(s.command("PUT cpu 2 aa").startswith("ERR STORAGE"))
        self.assertIn("STORAGE_UNAVAILABLE", s.command("PUT cpu 3 bb"))
        self.assertIn("STORAGE_UNAVAILABLE", s.command("RANGE cpu 0 3 3"))
        s.stop(crash=True)
        s = self.server()
        self.assertEqual(s.command("RANGE cpu 0 3 3"), [(1, 1, "ff")])
        self.assertEqual(s.command("PUT cpu 4 cc"), "OK 2")

    # Catches off-by-one replay reads at every possible byte of a final frame.
    def test_every_truncated_frame_boundary_recovers_only_complete_prefix(self):
        s = self.server()
        s.command("PUT cpu 1 00ff")
        s.command("PUT cpu 2 aabbcc")
        s.stop()
        wal = self.directory / "data.wal"
        good = wal.read_bytes()
        first_end = 16 + 36 + 3 + 2 + 4
        for end in range(first_end + 1, len(good)):
            wal.write_bytes(good[:end])
            s = self.server()
            self.assertEqual(s.command("RANGE cpu 0 3 3"), [(1, 1, "00ff")], end)
            self.assertEqual(wal.read_bytes(), good[:first_end], end)
            s.stop()

    # Independent, literal fixture: CRC32C fields are not produced by the C++ writer.
    def test_replays_independent_wal_fixture(self):
        self.directory.mkdir()
        (self.directory / "data.wal").write_bytes(bytes.fromhex(
            "535452415441303101000000022ca2e2"
            "524543312d000000010000000000000007000000000000000300000002000000"
            "55725cea63707500ff8d9ea98a"))
        s = self.server()
        self.assertEqual(s.command("RANGE cpu 0 10 10"), [(1, 7, "00ff")])


if __name__ == "__main__":
    unittest.main(verbosity=2)
