#!/usr/bin/env python3
"""
archive_round_trip.py — exercise the carrier_create_account / export / import
/ remove surface added for ISSUE-123 (Cut A).

Drives carrier-cli subprocesses end-to-end against libjami. Five scenarios:

  1. round_trip          : create → AccountReady → export → import in fresh dir
                           → AccountReady with the SAME selfUri
  2. corrupted_archive   : feed garbage as --import-account → AccountError
                           ; cause within {corrupted, wrong-pin, unknown}
                           (libjami's exact mapping for non-archive bytes
                           is observed empirically)
  3. export_before_ready : carrier:ExportAccount with an unknown account_id
                           → AccountError ; cause "not-ready"
  4. import_not_found    : --import-account /nonexistent → AccountError
                           ; cause "not-found" (pre-validation, no libjami)
  5. remove_then_reimport: create → export → remove → re-import → same selfUri

Usage:
    python3 tests/archive_round_trip.py [--cli PATH] [--data-root DIR] [--timeout N]
"""

import argparse
import os
import re
import subprocess
import sys
import threading
import time

CLI_DEFAULT = os.path.join(os.path.dirname(__file__), "../build/carrier-cli")
DATA_ROOT_DEFAULT = "/tmp/carrier-archive-test"


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
        self.cli = cli
        self.data_dir = data_dir
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

    def wait_for_type(self, type_name, timeout=60):
        deadline = time.time() + timeout
        with self._cv:
            idx = 0
            while True:
                while idx < len(self._lines):
                    line = self._lines[idx]
                    idx += 1
                    if extract_type(line) == type_name:
                        return line
                remaining = deadline - time.time()
                if remaining <= 0:
                    return None
                self._cv.wait(timeout=min(remaining, 2.0))

    def wait_exit(self, timeout=30):
        try:
            return self._proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            return -1

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


def case_round_trip(cli, root, timeout):
    log("CASE", "round_trip")
    data_a = fresh_dir(root, "alice")
    data_b = fresh_dir(root, "bob")
    archive = os.path.join(root, "alice.gz")
    if os.path.exists(archive):
        os.remove(archive)

    # 1. Create account in data_a, wait for AccountReady, capture selfUri.
    a = CarrierProc("a-create", cli, data_a, ["--create-account", "alice"])
    ready = a.wait_for_type("AccountReady", timeout)
    if not ready:
        a.stop()
        raise AssertionError("create: AccountReady never fired")
    self_uri = extract_field(ready, "selfUri")
    log("CASE", f"created selfUri={self_uri}")

    # 2. ExportAccount via Turtle command.
    account = ready.split('"')[1] if 'carrier:account' in ready else None
    # The header form is `carrier:Connected.AccountReady carrier:account "<id>"`;
    # extract via the carrier:account predicate to be robust.
    m = re.search(r'carrier:account "([^"]+)"', ready)
    assert m, "AccountReady missing carrier:account"
    account_id = m.group(1)
    a.send(
        f'[] a carrier:ExportAccount ; carrier:account "{account_id}" ; '
        f'carrier:path "{archive}" .'
    )
    ar = a.wait_for_type("AccountArchiveReady", 30)
    if not ar:
        a.stop()
        raise AssertionError("export: AccountArchiveReady never fired")
    assert extract_field(ar, "path") == archive
    assert os.path.exists(archive), f"archive blob not on disk at {archive}"
    a.stop()
    a.wait_exit(10)

    # 3. Import into data_b — must yield the SAME selfUri.
    b = CarrierProc("b-import", cli, data_b, ["--import-account", archive])
    ready_b = b.wait_for_type("AccountReady", timeout)
    if not ready_b:
        b.stop()
        raise AssertionError("import: AccountReady never fired")
    self_uri_b = extract_field(ready_b, "selfUri")
    b.stop()
    b.wait_exit(10)

    if self_uri_b != self_uri:
        raise AssertionError(
            f"selfUri mismatch after import: src={self_uri} dst={self_uri_b}"
        )
    log("CASE", f"round_trip OK: selfUri preserved ({self_uri})")


def case_corrupted_archive(cli, root, timeout):
    """Feed a non-archive file as --import-account. libjami's addAccount
    refuses with an empty id (pre-REGISTERED), which carrier_create_account
    translates to AccountError cause="corrupted". Exercises the same
    end-to-end cause-token plumbing as wrong-pin without requiring libjami
    to first produce a passworded archive (the password-protected source
    path needs further libjami config we haven't fully pinned down yet —
    flagged in the Cut A checkpoint as a follow-up)."""
    log("CASE", "corrupted_archive")
    data = fresh_dir(root, "garbage-archive")
    garbage = os.path.join(root, "garbage.gz")
    with open(garbage, "wb") as f:
        f.write(b"this is not a gzip nor a jami archive\n" * 16)

    a = CarrierProc("ga", cli, data, ["--import-account", garbage])
    err = a.wait_for_type("AccountError", 60)
    rc = a.wait_exit(15)
    if not err:
        raise AssertionError("corrupted: AccountError never fired")
    cause = extract_field(err, "cause")
    # libjami's failure mode for non-archive bytes is observed empirically;
    # we accept any closed-vocab token *except* "not-found" (that path is
    # exercised by case_import_not_found and would mean we never reached
    # libjami).
    if cause in ("not-found",):
        raise AssertionError(
            f"corrupted: pre-validation fired (cause={cause}); "
            "expected libjami to be involved")
    if cause not in ("corrupted", "wrong-pin", "unknown"):
        raise AssertionError(f"corrupted: unexpected cause={cause}")
    if rc == 0:
        raise AssertionError("corrupted: cli exited 0, expected non-zero")
    log("CASE", f"corrupted_archive OK: cause={cause}, cli exited non-zero")


def case_export_before_ready(cli, root, timeout):
    log("CASE", "export_before_ready")
    data = fresh_dir(root, "ghost")
    archive = os.path.join(root, "ghost.gz")
    if os.path.exists(archive):
        os.remove(archive)
    # carrier-cli requires --account/--create-account/etc. There's no
    # "no-account" mode at the CLI surface. The C-level not-ready guard is
    # covered indirectly: export with a bogus account_id (any string that
    # isn't in c->accounts) hits the same not-ready branch.
    a = CarrierProc("ghost-create", cli, data, ["--create-account", "ghost"])
    a.wait_for_type("AccountReady", timeout)
    a.send(
        f'[] a carrier:ExportAccount ; '
        f'carrier:account "no-such-account-id" ; '
        f'carrier:path "{archive}" .'
    )
    err = a.wait_for_type("AccountError", 15)
    a.stop()
    a.wait_exit(10)
    if not err:
        raise AssertionError("export-not-ready: AccountError never fired")
    cause = extract_field(err, "cause")
    if cause != "not-ready":
        raise AssertionError(f"expected cause=not-ready, got {cause}")
    log("CASE", "export_before_ready OK: cause=not-ready")


def case_import_not_found(cli, root, timeout):
    log("CASE", "import_not_found")
    data = fresh_dir(root, "missing-archive")
    bogus = "/tmp/carrier-archive-test/definitely-not-here.gz"
    if os.path.exists(bogus):
        os.remove(bogus)

    a = CarrierProc("nf", cli, data, ["--import-account", bogus])
    err = a.wait_for_type("AccountError", 15)
    rc = a.wait_exit(10)
    if not err:
        raise AssertionError("import-not-found: AccountError never fired")
    cause = extract_field(err, "cause")
    if cause != "not-found":
        raise AssertionError(f"expected cause=not-found, got {cause}")
    if rc == 0:
        raise AssertionError("import-not-found: cli exited 0")
    log("CASE", "import_not_found OK: cause=not-found")


def case_remove_then_reimport(cli, root, timeout):
    log("CASE", "remove_then_reimport")
    data = fresh_dir(root, "remove-rt")
    archive = os.path.join(root, "remove-rt.gz")
    if os.path.exists(archive):
        os.remove(archive)

    # 1. Create + export + capture id/selfUri + exit.
    a = CarrierProc("rt-create", cli, data,
                    ["--create-account", "alice", "--export-account", archive])
    if a.wait_exit(timeout) != 0:
        raise AssertionError("rt-create: cli exited non-zero")
    assert os.path.exists(archive)

    # Read back the AccountReady selfUri + account id from the captured lines.
    src_lines = a._lines
    ready = next((l for l in src_lines if extract_type(l) == "AccountReady"), None)
    if not ready:
        raise AssertionError("rt-create: no AccountReady in stdout")
    self_uri = extract_field(ready, "selfUri")
    m = re.search(r'carrier:account "([^"]+)"', ready)
    assert m
    account_id = m.group(1)

    # 2. --remove-account against the same data dir; account should be gone.
    rm = CarrierProc("rt-remove", cli, data, ["--remove-account", account_id])
    if rm.wait_exit(15) != 0:
        raise AssertionError("rt-remove: cli exited non-zero")

    # 3. Re-import the archive into the same data dir; libjami should accept it
    #    (no conflicting account) and emit AccountReady with the same selfUri.
    b = CarrierProc("rt-import", cli, data, ["--import-account", archive])
    ready_b = b.wait_for_type("AccountReady", timeout)
    if not ready_b:
        b.stop()
        raise AssertionError("rt-import: AccountReady never fired")
    self_uri_b = extract_field(ready_b, "selfUri")
    b.stop()
    b.wait_exit(10)

    if self_uri_b != self_uri:
        raise AssertionError(
            f"remove-then-reimport selfUri mismatch: src={self_uri} new={self_uri_b}"
        )
    log("CASE", f"remove_then_reimport OK: selfUri preserved ({self_uri})")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cli", default=CLI_DEFAULT)
    ap.add_argument("--data-root", default=DATA_ROOT_DEFAULT)
    ap.add_argument("--timeout", type=int, default=90,
                    help="per-AccountReady wait (seconds)")
    ap.add_argument("--only", action="append", default=None,
                    help="run only the named case (repeatable)")
    args = ap.parse_args()

    cli = os.path.abspath(args.cli)
    if not os.path.isfile(cli):
        print(f"ERROR: carrier-cli not found at {cli}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.data_root, exist_ok=True)

    cases = {
        "round_trip":           case_round_trip,
        "corrupted_archive":    case_corrupted_archive,
        "export_before_ready":  case_export_before_ready,
        "import_not_found":     case_import_not_found,
        "remove_then_reimport": case_remove_then_reimport,
    }
    selected = args.only or list(cases.keys())

    failures = []
    for name in selected:
        if name not in cases:
            print(f"unknown case: {name}", file=sys.stderr)
            sys.exit(1)
        try:
            cases[name](cli, args.data_root, args.timeout)
        except AssertionError as e:
            log("FAIL", f"{name}: {e}")
            failures.append(name)
        except Exception as e:  # noqa: BLE001
            log("FAIL", f"{name}: unexpected {type(e).__name__}: {e}")
            failures.append(name)

    if failures:
        print(f"\nFAILED ({len(failures)}/{len(selected)}): {', '.join(failures)}",
              file=sys.stderr)
        sys.exit(1)
    print(f"\nPASS: {len(selected)} cases")


if __name__ == "__main__":
    main()
