#!/usr/bin/env python3
"""Record the actual compiler invocation next to the binary after a successful link."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--compiler", required=True)
parser.add_argument("--flags", required=True)
parser.add_argument("--source", action="append", type=Path, required=True)
args = parser.parse_args()
metadata = {"built_at_utc": datetime.now(timezone.utc).isoformat(), "compiler_command": args.compiler,
            "compiler_version": subprocess.check_output(shlex.split(args.compiler)+["--version"], text=True).splitlines()[0],
            "flags": args.flags, "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            "source_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in args.source}}
Path(str(args.binary)+".build.json").write_text(json.dumps(metadata, indent=2)+"\n")
