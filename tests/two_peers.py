#!/usr/bin/env python3
"""
two_peers.py — smoke test: create two carrier-cli instances, exchange a message
and a large RDF object.

Usage:
    python3 tests/two_peers.py [--cli PATH] [--data-root DIR] [--timeout N]

The script creates two Jami accounts, exchanges a trust request, sends one
text message, then sends a large RDF object as a content-addressed file and
verifies the received file's name matches the canonical SHA-256 of its content.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import threading
import time
import time


CLI_DEFAULT = os.path.join(os.path.dirname(__file__), "../build/carrier-cli")
DATA_ROOT_DEFAULT = "/tmp/carrier-test"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def log(tag, msg):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {tag}: {msg}", flush=True)


def extract_field(turtle_line, field):
    """Extract carrier:<field> "value" from a Turtle event line."""
    pattern = rf'carrier:{re.escape(field)} "([^"\\]*(?:\\.[^"\\]*)*)"'
    m = re.search(pattern, turtle_line)
    if m:
        return m.group(1).replace('\\"', '"').replace('\\\\', '\\')
    return None


def extract_type(turtle_line):
    m = re.search(r'\[\] a carrier:(\w+)', turtle_line)
    return m.group(1) if m else None


def generate_large_turtle(n_triples=500):
    """Generate a Turtle document with n_triples data triples."""
    lines = [
        "@prefix ex: <http://example.org/> .",
        "@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .",
        "",
    ]
    for i in range(n_triples):
        lines.append(
            f'ex:item{i} ex:index {i} ; '
            f'ex:label "item number {i}" ; '
            f'ex:value "{i * 3.14:.6f}"^^xsd:decimal .'
        )
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Carrier process wrapper
# ---------------------------------------------------------------------------

class CarrierProc:
    def __init__(self, name, cli, data_dir, account_id=None, display_name=None):
        self.name = name
        self.cli = cli
        self.data_dir = data_dir
        self.account_id = account_id   # libjami ID (not the Jami URI)
        self.self_uri = None           # jami:40hex URI
        self._proc = None
        self._lines = []
        self._lock = threading.Lock()
        self._new_line = threading.Condition(self._lock)

        args = [cli, "--data-dir", data_dir]
        if account_id:
            args += ["--account", account_id]
        elif display_name:
            args += ["--create-account", display_name]
        else:
            raise ValueError("need account_id or display_name")

        log(name, f"starting: {' '.join(args)}")
        self._proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

        self._reader = threading.Thread(target=self._read_stdout, daemon=True)
        self._reader.start()
        self._stderr_reader = threading.Thread(target=self._read_stderr, daemon=True)
        self._stderr_reader.start()

    def _read_stdout(self):
        for line in self._proc.stdout:
            line = line.rstrip("\n")
            if not line or line.startswith("@prefix"):
                continue
            with self._new_line:
                self._lines.append(line)
                self._new_line.notify_all()
            log(self.name, f"< {line}")

    def _read_stderr(self):
        for line in self._proc.stderr:
            line = line.rstrip("\n")
            if line:
                log(self.name, f"[stderr] {line}")

    def send(self, turtle):
        """Write a Turtle command to the process stdin."""
        turtle = turtle.strip()
        if not turtle.endswith("."):
            turtle += " ."
        log(self.name, f"> {turtle}")
        self._proc.stdin.write(turtle + "\n")
        self._proc.stdin.flush()

    def wait_for_type(self, event_type, timeout=60):
        """Block until an event of the given type arrives. Returns the line."""
        deadline = time.time() + timeout
        with self._new_line:
            idx = 0
            while True:
                while idx < len(self._lines):
                    line = self._lines[idx]
                    idx += 1
                    if extract_type(line) == event_type:
                        return line
                remaining = deadline - time.time()
                if remaining <= 0:
                    return None
                self._new_line.wait(timeout=min(remaining, 2.0))

    def wait_for_system_containing(self, substr, timeout=60):
        """Block until a System event whose text contains substr. Returns line."""
        deadline = time.time() + timeout
        with self._new_line:
            idx = 0
            while True:
                while idx < len(self._lines):
                    line = self._lines[idx]
                    idx += 1
                    if extract_type(line) == "System" and substr in line:
                        return line
                remaining = deadline - time.time()
                if remaining <= 0:
                    return None
                self._new_line.wait(timeout=min(remaining, 2.0))

    def stop(self):
        if self._proc and self._proc.poll() is None:
            self._proc.stdin.close()
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._proc.kill()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cli", default=CLI_DEFAULT)
    ap.add_argument("--data-root", default=DATA_ROOT_DEFAULT)
    ap.add_argument("--timeout", type=int, default=120,
                    help="seconds to wait for DHT events (default: 120)")
    ap.add_argument("--reuse", action="store_true",
                    help="reuse existing data dirs (skip account creation)")
    args = ap.parse_args()

    cli = os.path.abspath(args.cli)
    if not os.path.isfile(cli):
        print(f"ERROR: carrier-cli not found at {cli}", file=sys.stderr)
        sys.exit(1)

    data_a = os.path.join(args.data_root, "alice")
    data_b = os.path.join(args.data_root, "bob")
    id_file_a = os.path.join(args.data_root, "alice.id")
    id_file_b = os.path.join(args.data_root, "bob.id")

    os.makedirs(data_a, exist_ok=True)
    os.makedirs(data_b, exist_ok=True)

    timeout = args.timeout

    # ------------------------------------------------------------------
    # Phase 1: create or load accounts
    # ------------------------------------------------------------------

    if args.reuse and os.path.exists(id_file_a) and os.path.exists(id_file_b):
        acct_a = open(id_file_a).read().strip()
        acct_b = open(id_file_b).read().strip()
        log("main", f"reusing Alice={acct_a} Bob={acct_b}")
        alice = CarrierProc("Alice", cli, data_a, account_id=acct_a)
        bob   = CarrierProc("Bob",   cli, data_b, account_id=acct_b)
    else:
        log("main", "creating accounts for Alice and Bob")
        alice = CarrierProc("Alice", cli, data_a, display_name="Alice")
        bob   = CarrierProc("Bob",   cli, data_b, display_name="Bob")

    # ------------------------------------------------------------------
    # Phase 2: wait for AccountReady on both
    # ------------------------------------------------------------------

    log("main", "waiting for AccountReady on Alice…")
    line_a = alice.wait_for_type("AccountReady", timeout=timeout)
    if not line_a:
        print("FAIL: Alice never reached AccountReady", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    alice.self_uri = extract_field(line_a, "selfUri")
    alice.account_id = extract_field(line_a, "account") or alice.account_id
    log("main", f"Alice ready: uri={alice.self_uri} account={alice.account_id}")

    log("main", "waiting for AccountReady on Bob…")
    line_b = bob.wait_for_type("AccountReady", timeout=timeout)
    if not line_b:
        print("FAIL: Bob never reached AccountReady", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    bob.self_uri = extract_field(line_b, "selfUri")
    bob.account_id = extract_field(line_b, "account") or bob.account_id
    log("main", f"Bob ready: uri={bob.self_uri} account={bob.account_id}")

    # Persist IDs for --reuse
    open(id_file_a, "w").write(alice.account_id)
    open(id_file_b, "w").write(bob.account_id)

    # ------------------------------------------------------------------
    # Phase 3: trust exchange (skipped if accounts are reused / already trusted)
    # ------------------------------------------------------------------

    def already_has_conversation(peer):
        with peer._lock:
            return any(extract_type(l) == "ConversationReady" for l in peer._lines)

    if already_has_conversation(alice) and already_has_conversation(bob):
        log("main", "accounts already trusted and conversation ready — skipping trust exchange")
    else:
        log("main", "Alice sends trust request to Bob…")
        alice.send(
            f'[] a carrier:SendTrustRequest ; '
            f'carrier:account "{alice.account_id}" ; '
            f'carrier:contactUri "{bob.self_uri}" ; '
            f'carrier:message "hi from alice" .'
        )

        log("main", "waiting for Bob to receive trust request…")
        tr = bob.wait_for_type("TrustRequest", timeout=timeout)
        if not tr:
            print("FAIL: Bob never received TrustRequest", file=sys.stderr)
            alice.stop(); bob.stop(); sys.exit(1)

        from_uri = extract_field(tr, "contactUri") or extract_field(tr, "fromUri")
        log("main", f"Bob got TrustRequest from {from_uri}, accepting…")

        bob.send(
            f'[] a carrier:AcceptTrustRequest ; '
            f'carrier:account "{bob.account_id}" ; '
            f'carrier:contactUri "{alice.self_uri}" .'
        )

        log("main", "waiting for ConversationReady on both sides…")
        alice.wait_for_type("ConversationReady", timeout=timeout)
        bob.wait_for_type("ConversationReady", timeout=timeout)

    # ------------------------------------------------------------------
    # Phase 4: send a text message
    # ------------------------------------------------------------------

    msg = "hello from Alice over Jami"
    log("main", f"Alice sends: '{msg}'")
    alice.send(
        f'[] a carrier:SendMsg ; '
        f'carrier:account "{alice.account_id}" ; '
        f'carrier:contactUri "{bob.self_uri}" ; '
        f'carrier:text "{msg}" .'
    )

    # ------------------------------------------------------------------
    # Phase 5: verify Bob receives it
    # ------------------------------------------------------------------

    log("main", "waiting for Bob to receive TextMessage…")
    received = bob.wait_for_type("TextMessage", timeout=timeout)
    if not received:
        print("FAIL: Bob never received TextMessage", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    rx_text = extract_field(received, "text")
    log("main", f"Bob received: '{rx_text}'")

    if rx_text == msg:
        print("PASS (1/2): text message delivered correctly.", flush=True)
    else:
        print(f"\nFAIL: expected '{msg}', got '{rx_text}'", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    # ------------------------------------------------------------------
    # Phase 6: large RDF object — content-addressed file transfer
    # ------------------------------------------------------------------

    log("main", "generating large Turtle document (500 triples)…")
    turtle_content = generate_large_turtle(500)
    turtle_path = os.path.join(args.data_root, "large_object.ttl")
    recv_path   = os.path.join(args.data_root, "bob_received.ttl")
    with open(turtle_path, "w") as f:
        f.write(turtle_content)
    log("main", f"wrote {len(turtle_content)} bytes to {turtle_path}")

    # Get the conversation ID from Alice's ConversationReady (already received)
    conv_id = None
    with alice._lock:
        for line in alice._lines:
            if extract_type(line) == "ConversationReady":
                conv_id = extract_field(line, "conversationId")
                break
    if not conv_id:
        print("FAIL: could not find conversationId", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)
    log("main", f"using conversationId={conv_id}")

    log("main", "Alice sends RDF object…")
    alice.send(
        f'[] a carrier:SendRdfObject ; '
        f'carrier:account "{alice.account_id}" ; '
        f'carrier:conversationId "{conv_id}" ; '
        f'carrier:path "{turtle_path}" ; '
        f'carrier:tmpDir "{args.data_root}" .'
    )

    log("main", "waiting for Alice System event with hash…")
    sent_event = alice.wait_for_system_containing("RdfObjectSent", timeout=30)
    if not sent_event:
        print("FAIL: Alice never emitted RdfObjectSent system event", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    m = re.search(r'RdfObjectSent hash=([0-9a-f]{64})', sent_event)
    if not m:
        print(f"FAIL: could not parse hash from: {sent_event}", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)
    sent_hash = m.group(1)
    log("main", f"Alice reports canonical hash: {sent_hash}")

    log("main", "waiting for Bob to receive FileRecv…")
    file_recv = bob.wait_for_type("FileRecv", timeout=timeout)
    if not file_recv:
        print("FAIL: Bob never received FileRecv", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    recv_filename = extract_field(file_recv, "filename")
    recv_msg_id   = extract_field(file_recv, "messageId")
    recv_file_id  = extract_field(file_recv, "fileId")
    log("main", f"Bob got FileRecv: filename={recv_filename} messageId={recv_msg_id}")

    # Filename should be "<hash>.ttl"
    expected_filename = f"{sent_hash}.ttl"
    if recv_filename != expected_filename:
        print(f"FAIL: filename mismatch: got '{recv_filename}', expected '{expected_filename}'",
              file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)
    log("main", "filename matches canonical hash — PASS")

    log("main", f"Bob accepts file to {recv_path}…")
    bob.send(
        f'[] a carrier:AcceptFile ; '
        f'carrier:account "{bob.account_id}" ; '
        f'carrier:conversationId "{conv_id}" ; '
        f'carrier:messageId "{recv_msg_id}" ; '
        f'carrier:fileId "{recv_file_id}" ; '
        f'carrier:path "{recv_path}" .'
    )

    log("main", "waiting for FileComplete on Bob…")
    complete = bob.wait_for_type("FileComplete", timeout=timeout)
    if not complete:
        print("FAIL: Bob never got FileComplete", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    status = extract_field(complete, "status")
    log("main", f"FileComplete status={status}")
    if status != "finished":
        print(f"FAIL: unexpected FileComplete status: {status}", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    # Verify the received file content and size
    with open(recv_path, "rb") as f:
        recv_bytes = f.read()
    log("main", f"received file size: {len(recv_bytes)} bytes (sent {len(turtle_content)} bytes)")

    if recv_bytes != turtle_content.encode():
        print("FAIL: received file content does not match sent content", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    # Parse it with rdflib if available, or just check it starts with @prefix
    if recv_bytes.startswith(b"@prefix"):
        log("main", "received file is valid Turtle (starts with @prefix)")
    else:
        print("FAIL: received file does not look like Turtle", file=sys.stderr)
        alice.stop(); bob.stop(); sys.exit(1)

    print(f"PASS (2/2): large RDF object delivered, filename=hash verified.", flush=True)
    print(f"\nAll checks passed.", flush=True)

    alice.stop()
    bob.stop()


if __name__ == "__main__":
    main()
