#!/usr/bin/env python3
"""Kill a real writer; verify acknowledgements, truncate a torn tail, reject corruption."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_e2e import Server, BIN

parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, default=ROOT / "evidence/crash-demo.json")
args = parser.parse_args()
result = {"measured_at_utc": datetime.now(timezone.utc).isoformat(), "failure": "real SIGKILL, not graceful shutdown",
          "limitation": "process crash on a running OS, not machine power loss"}
with tempfile.TemporaryDirectory(prefix="strata-crash-") as temporary:
    directory = Path(temporary) / "db"
    server = Server(directory)
    try:
        for i in range(100):
            assert server.command(f"PUT temperature {i} {i:02x}") == f"OK {i+1}"
    finally:
        server.stop(crash=True)
    result["acknowledged_before_sigkill"] = 100
    result["process_returncode"] = server.proc.returncode
    server = Server(directory)
    try:
        rows = server.command("RANGE temperature 0 99 1000")
        assert rows == [(i+1, i, f"{i:02x}") for i in range(100)]
        result["records_verified_after_restart"] = len(rows)
    finally:
        server.stop()
    wal = directory / "data.wal"
    good = wal.read_bytes()
    wal.write_bytes(good + b"REC1\x30\x00")
    server = Server(directory)
    try:
        result["torn_tail_bytes_removed"] = json.loads(server.command("STATS"))["recovered_tail_bytes"]
        assert result["torn_tail_bytes_removed"] == 6
        assert wal.read_bytes() == good
    finally:
        server.stop()
    corrupted = bytearray(good)
    corrupted[-1] ^= 1
    wal.write_bytes(corrupted)
    process = subprocess.run([str(BIN), "serve", "--dir", str(directory), "--port", "0"],
                             capture_output=True, text=True, timeout=5)
    assert process.returncode != 0 and "checksum" in process.stderr
    assert wal.read_bytes() == corrupted
    result["corrupt_complete_frame"] = {"startup_exit_code": process.returncode,
                                        "error": process.stderr.strip(), "file_preserved": True}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
