#!/usr/bin/env python3
"""
saved_conversation.py — exercise the carrier:GetSavedConversation /
carrier:SavedConversation surface added for messenger2's "Saved Messages"
workspace.

Drives a single carrier-cli subprocess against libjami. Two scenarios:

  1. creates_on_first_call : create account → AccountReady →
                             carrier:GetSavedConversation →
                             carrier:SavedConversation with a non-empty
                             40-hex conversationId
  2. idempotent            : same process, second GetSavedConversation →
                             reply carries the SAME conversationId

Usage:
    python3 tests/saved_conversation.py [--cli PATH] [--data-root DIR]
"""

import argparse
import os
import re
import subprocess
import sys
import threading
import time

CLI_DEFAULT = os.path.join(os.path.dirname(__file__), "../build/carrier-cli")
DATA_ROOT_DEFAULT = "/tmp/carrier-saved-test"

HEX40 = re.compile(r"^[0-9a-f]{40}$")


def log(tag, msg):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {tag}: {msg}", flush=True)


def extract_field(line, field):
    m = re.search(rf'carrier:{re.escape(field)} "([^"\\]*(?:\\.[^"\\]*)*)"', line)
    if not m:
        return None
    return m.group(1).replace('\\"', '"').replace('\\\\', '\\')


def extract_type(line):
    m = re.search(r"\[\] a carrier:(\w+)", line)
    return m.group(1) if m else None


class CarrierProc:
    def __init__(self, name, cli, data_dir, extra_args):
        self.name = name
        self._lines = []
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)

        args = [cli, "--data-dir", data_dir] + list(extra_args)
        log(name, f"start: {' '.join(args)}")
        self._proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self):
        for line in self._proc.stdout:
            line = line.rstrip("\n")
            if not line or line.startswith("@prefix"):
                continue
            with self._cv:
                self._lines.append(line)
                self._cv.notify_all()
            log(self.name, f"< {line}")

    def _read_stderr(self):
        for line in self._proc.stderr:
            line = line.rstrip("\n")
            if line:
                log(self.name, f"[stderr] {line}")

    def send(self, turtle):
        turtle = turtle.strip()
        if not turtle.endswith("."):
            turtle += " ."
        log(self.name, f"> {turtle}")
        self._proc.stdin.write(turtle + "\n")
        self._proc.stdin.flush()

    def wait_for_type_after(self, type_name, start_idx, timeout=30):
        """Block until a new event of `type_name` arrives strictly after
        index `start_idx` in the captured-line buffer. Returns (line, idx)."""
        deadline = time.time() + timeout
        with self._cv:
            idx = start_idx
            while True:
                while idx < len(self._lines):
                    line = self._lines[idx]
                    idx += 1
                    if extract_type(line) == type_name:
                        return line, idx
                remaining = deadline - time.time()
                if remaining <= 0:
                    return None, idx
                self._cv.wait(timeout=min(remaining, 2.0))

    def wait_for_type(self, type_name, timeout=60):
        line, _ = self.wait_for_type_after(type_name, 0, timeout)
        return line

    def line_count(self):
        with self._lock:
            return len(self._lines)

    def stop(self):
        if self._proc.poll() is None:
            try:
                self._proc.stdin.close()
            except (BrokenPipeError, OSError):
                pass
            try:
                self._proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._proc.kill()


def fresh_dir(root, name):
    path = os.path.join(root, name)
    if os.path.exists(path):
        subprocess.run(["rm", "-rf", path], check=True)
    os.makedirs(path)
    return path


def run(cli, root, timeout):
    log("CASE", "creates_on_first_call + idempotent (one process)")
    data = fresh_dir(root, "saved")

    a = CarrierProc("a", cli, data, ["--create-account", "alice"])

    ready = a.wait_for_type("AccountReady", timeout)
    if not ready:
        a.stop()
        raise AssertionError("AccountReady never fired")
    m = re.search(r'carrier:account "([^"]+)"', ready)
    assert m, "AccountReady missing carrier:account"
    account_id = m.group(1)
    log("CASE", f"account ready: {account_id}")

    # ----- 1. First call: a single-member-self swarm is minted. -----
    cursor = a.line_count()
    a.send(
        f'[] a carrier:GetSavedConversation ; '
        f'carrier:account "{account_id}" .'
    )
    reply, cursor = a.wait_for_type_after("SavedConversation", cursor, timeout=15)
    if not reply:
        a.stop()
        raise AssertionError("SavedConversation never fired (first call)")
    conv_a = extract_field(reply, "conversationId")
    if not conv_a or not HEX40.match(conv_a):
        a.stop()
        raise AssertionError(
            f"expected 40-hex conversationId; got {conv_a!r}"
        )
    log("CASE", f"first call OK: convId={conv_a}")

    # ----- 2. Second call: must return the same convId. -----
    a.send(
        f'[] a carrier:GetSavedConversation ; '
        f'carrier:account "{account_id}" .'
    )
    reply2, _ = a.wait_for_type_after("SavedConversation", cursor, timeout=15)
    if not reply2:
        a.stop()
        raise AssertionError("SavedConversation never fired (second call)")
    conv_b = extract_field(reply2, "conversationId")
    if conv_b != conv_a:
        a.stop()
        raise AssertionError(
            f"idempotency violation: first={conv_a} second={conv_b}"
        )
    log("CASE", f"idempotent OK: convId stable across calls")

    a.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cli", default=CLI_DEFAULT)
    ap.add_argument("--data-root", default=DATA_ROOT_DEFAULT)
    ap.add_argument("--timeout", type=int, default=90,
                    help="per-AccountReady wait (seconds)")
    args = ap.parse_args()

    cli = os.path.abspath(args.cli)
    if not os.path.isfile(cli):
        print(f"ERROR: carrier-cli not found at {cli}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.data_root, exist_ok=True)

    try:
        run(cli, args.data_root, args.timeout)
    except AssertionError as e:
        log("FAIL", str(e))
        sys.exit(1)
    print("\nPASS: saved_conversation")


if __name__ == "__main__":
    main()
