#!/usr/bin/env python3
"""Compatibility entry point for directly compiled production render-role tests."""
import argparse
from pathlib import Path
from audio_component_runner import run

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("--compiler", default="/usr/bin/g++-15")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
run(args.source.resolve().parents[3], args.compiler, args.sanitize, "roles.")
