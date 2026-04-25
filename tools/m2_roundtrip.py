#!/usr/bin/env python3
"""
m2_roundtrip.py — End-to-end smoke test for the M2 exit criterion.

Spawns two `carrier-cli` instances (alice + bob), creates fresh Jami accounts,
exchanges a trust request, then sends a 1:1 text message from alice to bob and
verifies bob receives it. Pass = M2 backend works on this host.

Usage:
    python3 carrier/tools/m2_roundtrip.py
    python3 carrier/tools/m2_roundtrip.py --keep   # don't rm -rf the data dirs

Exit codes:
    0  pass
    1  failure (see stderr for stage that timed out)

Notes:
    - Requires `carrier/build/carrier-cli` to exist (run `make` in carrier/ first).
    - Each fresh account boots ~10–60 s on the first DHT round-trip.
      Trust+message phases add ~10–30 s. Stage 3 (text round-trip) retries
      libjami's best-effort swarm push every 60 s up to 6 attempts, so the
      worst-case wall time is ~7 min on pathological cold-DHT runs and
      ~90 s on the happy path.
    - Network connectivity to `bootstrap.jami.net:4222` is required until
      `mother.resonator.network` ships the dhtnode (D13).
    - This is not a substitute for D9's committed alice/bob archives — those
      arrive in M3. For now, accounts are generated fresh per run.
"""

import argparse
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

CARRIER_CLI = Path(__file__).resolve().parent.parent / "build" / "carrier-cli"


class Carrier:
    """One carrier-cli subprocess driven over stdin/stdout."""

    def __init__(self, name: str, data_dir: Path):
        self.name = name
        cmd = [
            str(CARRIER_CLI),
            "--data-dir", str(data_dir),
            "--create-account", name,
            "--log", "warn",
        ]
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.events: queue.Queue = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._stderr_pump, daemon=True).start()

    def _reader(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if not line or line.startswith("@prefix") or line.startswith("@base"):
                continue
            self.events.put(line)
        self.events.put(None)

    def _stderr_pump(self):
        for line in self.proc.stderr:
            sys.stderr.write(f"[{self.name} stderr] {line}")

    def send(self, turtle: str):
        print(f"[{self.name} <-] {turtle}")
        self.proc.stdin.write(turtle.rstrip() + "\n")
        self.proc.stdin.flush()

    def wait_for(self, predicate, timeout: float, label: str):
        """Block until an event line matches predicate(line). Returns the line."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                line = self.events.get(timeout=min(1.0, remaining))
            except queue.Empty:
                continue
            if line is None:
                raise RuntimeError(f"[{self.name}] EOF while waiting for {label}")
            print(f"[{self.name} ->] {line}")
            if predicate(line):
                return line
        raise TimeoutError(f"[{self.name}] timeout ({timeout}s) waiting for {label}")

    def quit(self):
        try:
            self.proc.stdin.write("[] a carrier:Quit .\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def predicate_value(line: str, pred: str) -> str | None:
    """Pull `carrier:<pred>` value (string literal or unquoted) from a Turtle line."""
    m = re.search(rf'carrier:{pred}\s+"([^"\\]*(?:\\.[^"\\]*)*)"', line)
    return m.group(1) if m else None


def has_type(line: str, t: str) -> bool:
    return f"a carrier:{t}" in line or f"carrier:{t} ;" in line.replace(" .", " ;")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true",
                    help="don't delete the temp data dirs on exit")
    ap.add_argument("--root", default="/tmp/carrier-m2-roundtrip",
                    help="parent dir for alice/bob data dirs")
    args = ap.parse_args()

    if not CARRIER_CLI.exists():
        sys.exit(f"carrier-cli not found at {CARRIER_CLI} — run `make` in carrier/")

    root = Path(args.root)
    if root.exists():
        shutil.rmtree(root)
    alice_dir = root / "alice"
    bob_dir = root / "bob"
    alice_dir.mkdir(parents=True)
    bob_dir.mkdir(parents=True)

    alice = Carrier("alice", alice_dir)
    bob = Carrier("bob", bob_dir)

    try:
        # --- Stage 1: account registration --------------------------------
        # Each instance was started with --create-account so AccountReady
        # fires unprompted once REGISTERED on the DHT.
        print("\n=== Stage 1: account registration ===")
        a_ready = alice.wait_for(
            lambda l: "carrier:AccountReady" in l, timeout=90,
            label="alice AccountReady")
        b_ready = bob.wait_for(
            lambda l: "carrier:AccountReady" in l, timeout=90,
            label="bob AccountReady")

        a_account = predicate_value(a_ready, "account")
        a_uri     = predicate_value(a_ready, "selfUri")
        b_account = predicate_value(b_ready, "account")
        b_uri     = predicate_value(b_ready, "selfUri")
        print(f"\n  alice: account={a_account}  uri={a_uri}")
        print(f"  bob:   account={b_account}  uri={b_uri}\n")
        if not all([a_account, a_uri, b_account, b_uri]):
            sys.exit("could not parse account/selfUri from AccountReady events")

        # --- Stage 2: trust exchange --------------------------------------
        print("\n=== Stage 2: trust exchange ===")
        alice.send(
            f'[] a carrier:SendTrustRequest ; '
            f'carrier:account "{a_account}" ; '
            f'carrier:contactUri "{b_uri}" ; '
            f'carrier:message "hi from alice" .')

        bob.wait_for(
            lambda l: "carrier:TrustRequest" in l and a_uri in l,
            timeout=120, label="bob receives TrustRequest from alice")

        bob.send(
            f'[] a carrier:AcceptTrustRequest ; '
            f'carrier:account "{b_account}" ; '
            f'carrier:contactUri "{a_uri}" .')

        # --- Stage 3: text message round-trip -----------------------------
        # Once trust is accepted, libjami creates a 1:1 Swarm; carrier's shim
        # caches (account, peer_uri) -> conversation_id. SendMsg resolves via
        # that cache. Wait a few seconds for the swarm to materialize before
        # alice sends.
        #
        # libjami's swarm sync over DHT is best-effort and unbounded — bob's
        # SwarmManager may not pull alice's commit until well after the first
        # send on cold accounts. Resend on a fixed cadence; libjami dedupes
        # by commit hash, so duplicate commits are harmless. Each resend gives
        # the DHT another nudge.
        print("\n=== Stage 3: text message round-trip ===")
        time.sleep(5)

        send_msg = (
            f'[] a carrier:SendMsg ; '
            f'carrier:account "{a_account}" ; '
            f'carrier:contactUri "{b_uri}" ; '
            f'carrier:text "hello bob from m2 roundtrip" .')

        attempts = 6
        per_attempt = 60.0
        delivered = False
        for n in range(1, attempts + 1):
            print(f"\n--- Stage 3 attempt {n}/{attempts} ---")
            alice.send(send_msg)
            try:
                bob.wait_for(
                    lambda l: "carrier:TextMessage" in l and
                              "hello bob from m2 roundtrip" in l,
                    timeout=per_attempt,
                    label=f"bob receives TextMessage (attempt {n})")
                delivered = True
                break
            except TimeoutError as e:
                print(f"  attempt {n} timed out: {e}")

        if not delivered:
            raise TimeoutError(
                f"bob did not receive TextMessage after {attempts} attempts "
                f"({attempts * per_attempt:.0f}s total)")

        print("\n✓ M2 round-trip PASSED")

    except Exception as e:
        print(f"\n✗ M2 round-trip FAILED: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        alice.quit()
        bob.quit()
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)
        else:
            print(f"\n(kept data dirs at {root})")


if __name__ == "__main__":
    main()
