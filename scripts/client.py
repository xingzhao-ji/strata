#!/usr/bin/env python3
"""A small protocol client: client.py --port 7201 'PUT cpu 100 6869'."""
import argparse
import socket

parser = argparse.ArgumentParser()
parser.add_argument("--port", type=int, default=7201)
parser.add_argument("command")
args = parser.parse_args()
with socket.create_connection(("127.0.0.1", args.port), timeout=5) as sock:
    sock.sendall((args.command+"\n").encode())
    with sock.makefile("rb") as source:
        response = source.readline().decode().strip()
        print(response)
        if response.startswith("DATA "):
            for _ in range(int(response.split()[1])+1):
                print(source.readline().decode().strip())
