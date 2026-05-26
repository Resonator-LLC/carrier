#!/usr/bin/env python3
"""
history_replay.py — ISSUE-127 regression. After a Station-style restart
(carrier-cli quit + relaunch against the same data dir + same account id),
the Saved-Messages chat must replay the historical text commits as
`carrier:GroupMessage` events instead of staying empty.

Drives two carrier-cli subprocesses against libjami:

  1. Process A creates `alice`, mints Saved Messages, commits two text
     messages, then exits cleanly.
  2. Process B re-loads the same account against the same data dir. The
     `on_registration_state` cold-load loop must:
       a) Emit `carrier:ConversationReady` for the saved swarm (already
          working pre-127).
       b) Kick `libjami::loadConversation` per swarm; the SwarmLoaded
          handler must synthesise `carrier:GroupMessage` events for both
          historical text commits (the fix).

Usage:
    python3 tests/history_replay.py [--cli PATH] [--data-root DIR]
"""

import argparse
import os
import re
import subprocess
import sys
import threading
import time

CLI_DEFAULT = os.path.join(os.path.dirname(__file__), "../build/carrier-cli")
DATA_ROOT_DEFAULT = "/tmp/carrier-history-replay-test"

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

    def snapshot(self):
        with self._lock:
            return list(self._lines)

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
    log("CASE", "Saved Messages history replay across carrier-cli restart")
    data = fresh_dir(root, "history-replay")

    # ── Process A: mint account + saved swarm, commit two texts ──────────
    a = CarrierProc("a", cli, data, ["--create-account", "alice"])
    ready = a.wait_for_type("AccountReady", timeout)
    if not ready:
        a.stop()
        raise AssertionError("A: AccountReady never fired")
    account_id = extract_field(ready, "account")
    self_uri = extract_field(ready, "selfUri")
    assert account_id, "A: AccountReady missing carrier:account"
    assert self_uri, "A: AccountReady missing carrier:selfUri"
    log("CASE", f"A: account={account_id} self={self_uri}")

    cursor = a.line_count()
    a.send(
        f'[] a carrier:GetSavedConversation ; '
        f'carrier:account "{account_id}" .'
    )
    saved_line, cursor = a.wait_for_type_after("SavedConversation", cursor,
                                               timeout=15)
    if not saved_line:
        a.stop()
        raise AssertionError("A: SavedConversation never fired")
    saved_conv = extract_field(saved_line, "conversationId")
    assert HEX40.match(saved_conv or ""), (
        f"A: expected 40-hex conversationId; got {saved_conv!r}")
    log("CASE", f"A: saved_conv={saved_conv}")

    # Commit two text messages. Own-author commits surface as MessageSent
    # on the live path (the body is dropped — ISSUE-127's symptom).
    # Send one, wait for its MessageSent, send the next: libjami's per-
    # swarm commit thread occasionally hits 'mutex lock failed: Invalid
    # argument' when back-to-back sends race against a freshly-minted
    # swarm's internal lock; the wait-between-sends pattern matches what
    # carrier_cli_mint_race documents.
    sent_a_cursor = cursor
    for body in ["hello self", "second note"]:
        a.send(
            f'[] a carrier:SendConversationMsg ; '
            f'carrier:account "{account_id}" ; '
            f'carrier:conversationId "{saved_conv}" ; '
            f'carrier:text "{body}" .'
        )
        line, sent_a_cursor = a.wait_for_type_after("MessageSent",
                                                    sent_a_cursor, timeout=15)
        if not line or extract_field(line, "conversationId") != saved_conv:
            a.stop()
            raise AssertionError(
                f"A: MessageSent missing for body={body!r}")

    # Sanity: on the live path the body must NOT appear as a GroupMessage —
    # this is what makes the cold-start replay necessary.
    for line in a.snapshot():
        if extract_type(line) == "GroupMessage":
            text = extract_field(line, "text")
            if text in ("hello self", "second note"):
                a.stop()
                raise AssertionError(
                    "A: live SwarmMessageReceived emitted GroupMessage for "
                    "own commit (should collapse to MessageSent): " + line)

    a.stop()
    log("CASE", "A: stopped cleanly")

    # ── Process B: cold-start against the same data dir ──────────────────
    b = CarrierProc("b", cli, data, ["--account", account_id])
    b_ready = b.wait_for_type("AccountReady", timeout)
    if not b_ready:
        b.stop()
        raise AssertionError("B: AccountReady never fired")
    log("CASE", "B: AccountReady fired")

    # ConversationReady must surface (synthetic, fired by cold-load loop).
    conv_ready, _ = b.wait_for_type_after("ConversationReady", 0, timeout=10)
    if not conv_ready:
        b.stop()
        raise AssertionError("B: ConversationReady never fired")
    assert extract_field(conv_ready, "conversationId") == saved_conv, (
        f"B: ConversationReady for unexpected swarm: {conv_ready}")
    log("CASE", "B: ConversationReady fired for saved swarm")

    # The fix: the cold-load loop must also trigger replay, surfacing both
    # historical text commits as GroupMessage events.
    deadline = time.time() + 15
    seen_bodies = set()
    cursor_b = 0
    while time.time() < deadline and seen_bodies != {"hello self", "second note"}:
        line, cursor_b = b.wait_for_type_after("GroupMessage", cursor_b,
                                               timeout=2)
        if not line:
            continue
        if extract_field(line, "conversationId") != saved_conv:
            continue
        text = extract_field(line, "text")
        author = extract_field(line, "contactUri")
        if author != self_uri:
            b.stop()
            raise AssertionError(
                f"B: replayed GroupMessage carries unexpected author "
                f"(want {self_uri}, got {author}): {line}")
        if text in ("hello self", "second note"):
            seen_bodies.add(text)

    b.stop()

    missing = {"hello self", "second note"} - seen_bodies
    if missing:
        raise AssertionError(
            "B: cold-load replay missed " + ", ".join(sorted(missing)) +
            " — ISSUE-127 regression.")
    log("CASE", "B: both historical text commits replayed as GroupMessage")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cli", default=CLI_DEFAULT)
    ap.add_argument("--data-root", default=DATA_ROOT_DEFAULT)
    ap.add_argument("--timeout", type=int, default=90)
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
    print("\nPASS: history_replay")


if __name__ == "__main__":
    main()
