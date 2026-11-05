#!/usr/bin/env python3
"""Cross-compile the module for the Move (aarch64 Linux) and package it.

Works on Linux, macOS and Windows without make. Compiler choice:
  1. $CC if set (e.g. CC=aarch64-linux-gnu-gcc)
  2. aarch64-linux-gnu-gcc if on PATH
  3. zig cc -target aarch64-linux-gnu.2.17   (pins glibc 2.17; easiest on Windows)
"""
import os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(ROOT, "build", "arm", "audio-stream.so")
os.makedirs(os.path.dirname(out), exist_ok=True)

if os.environ.get("CC"):
    cc = os.environ["CC"].split()
elif shutil.which("aarch64-linux-gnu-gcc"):
    cc = ["aarch64-linux-gnu-gcc"]
elif shutil.which("zig"):
    cc = ["zig", "cc", "-target", "aarch64-linux-gnu.2.17"]
else:
    sys.exit("No cross compiler: install gcc-aarch64-linux-gnu or zig, or set CC.")

cmd = cc + ["-O2", "-Wall", "-Wextra", "-fPIC", "-shared", "-I" + os.path.join(ROOT, "src"),
            "-o", out, os.path.join(ROOT, "src", "audio_stream.c"), "-lpthread"]
print(" ".join(cmd))
subprocess.check_call(cmd)
subprocess.check_call([sys.executable, os.path.join(ROOT, "scripts", "package.py")])
