#!/usr/bin/env python3
"""Public feedback for SP2026 HW1 (7 points).

Public passes are NOT awarded grades. TAs use additional hidden cases and
code review. Run --task 1 2 to check selected tasks; omitted tasks are NOT RUN.
"""

from __future__ import annotations

import argparse
import errno
from contextlib import ExitStack, contextmanager
import fcntl
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable


ROOT = Path(__file__).resolve().parent
SERVER_PATH = ROOT / "server"
RECORD_PATH = ROOT / "accountRecord"

ACCOUNT_ID_START = 902001

WELCOME = (
    b"================================\n"
    b" Welcome to CSIE Ledger System \n"
    b"================================\n"
    b"Please enter your command: "
)
READY_PROMPT = b"Please enter your command: "
UPDATE_PROMPT = b"Please enter add <delta> or close: "

DEFAULT_BALANCES = [
    500, 1200, 0, 999999, 350, 42, 7600, 18, 910, 1000000,
    240, 87, 650, 3000, 1, 777, 25000, 64, 880, 150,
]


class JudgeError(RuntimeError):
    pass


def write_records(balances: list[int]) -> None:
    data = b"".join(
        struct.pack("=ii", ACCOUNT_ID_START + index, balance)
        for index, balance in enumerate(balances)
    )
    RECORD_PATH.write_bytes(data)


def read_balance(account_id: int) -> int:
    record_size = struct.calcsize("=ii")
    offset = (account_id - ACCOUNT_ID_START) * record_size
    data = RECORD_PATH.read_bytes()[offset : offset + record_size]
    stored_id, balance = struct.unpack("=ii", data)
    if stored_id != account_id:
        raise JudgeError(f"record {account_id} has unexpected id {stored_id}")
    return balance


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class ServerProcess:
    def __init__(self) -> None:
        self.port = free_port()
        self.proc: subprocess.Popen[bytes] | None = None

    def __enter__(self) -> "ServerProcess":
        if not SERVER_PATH.is_file():
            raise JudgeError("./server is missing. Run make first.")

        self.proc = subprocess.Popen(
            [str(SERVER_PATH), str(self.port)],
            cwd=ROOT,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise JudgeError("server exited before accepting connections")
            try:
                with socket.create_connection(
                    ("127.0.0.1", self.port), timeout=0.1
                ):
                    return self
            except OSError:
                time.sleep(0.03)

        self.__exit__()
        raise JudgeError("server did not start within 2 seconds")

    def __exit__(self, *_: object) -> None:
        if self.proc is None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=2.0)


class Client:
    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
        self.sock.settimeout(2.0)
        self.buffer = b""
        self.expect(WELCOME)

    def close(self) -> None:
        self.sock.close()

    def expect(self, expected: bytes) -> None:
        while len(self.buffer) < len(expected):
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout as exc:
                raise JudgeError(
                    f"timeout; expected {expected!r}, received {self.buffer!r}"
                ) from exc
            if not chunk:
                raise JudgeError(
                    f"connection closed; expected {expected!r}, "
                    f"received {self.buffer!r}"
                )
            self.buffer += chunk

        actual = self.buffer[: len(expected)]
        self.buffer = self.buffer[len(expected) :]
        if actual != expected:
            raise JudgeError(
                f"output mismatch\nexpected: {expected!r}\nactual:   {actual!r}"
            )

    def command(self, command: str, expected: bytes) -> None:
        self.sock.sendall(command.encode() + b"\n")
        self.expect(expected)

    def fragments(self, pieces: list[bytes], expected: bytes) -> None:
        for piece in pieces:
            self.sock.sendall(piece)
            time.sleep(0.03)
        self.expect(expected)

    def command_and_expect_close(self, command: str, expected: bytes) -> None:
        self.sock.sendall(command.encode() + b"\n")
        self.expect(expected)
        if self.buffer:
            raise JudgeError(f"unexpected output before close: {self.buffer!r}")
        try:
            extra = self.sock.recv(1)
        except socket.timeout as exc:
            raise JudgeError("server did not close the connection") from exc
        if extra:
            raise JudgeError(f"unexpected output before close: {extra!r}")


def update_started(account_id: int, balance: int) -> bytes:
    return (
        f">>> Account {account_id} balance: {balance}\n".encode()
        + b">>> Update lock acquired.\n"
        + UPDATE_PROMPT
    )


def update_succeeded(account_id: int, balance: int) -> bytes:
    return (
        b">>> Update successful.\n"
        + f">>> Account {account_id} balance: {balance}\n".encode()
        + UPDATE_PROMPT
    )


RECEIVE_PROMPT = b"Waiting for transfer; enter cancel or exit: "
TRANSFER_PROMPT = b"Waiting for receiver; enter cancel or exit: "
ACCEPT_PROMPT = b"Please enter accept or reject: "
LOCKED = b">>> Locked.\n" + READY_PROMPT
INVALID = b">>> [Error] Invalid command.\n"
PEER_GONE = b">>> Transfer canceled: peer disconnected.\n" + READY_PROMPT


CLOSED = b">>> Update closed.\n" + READY_PROMPT
RANGE = b">>> [Error] Balance out of range.\n" + UPDATE_PROMPT

def receiving(account_id: int) -> bytes:
    return f">>> Ready to receive on account {account_id}.\n".encode() + RECEIVE_PROMPT


def offer(source: int, target: int, amount: int) -> bytes:
    return (f">>> Transfer offer: {source} -> {target}, amount: {amount}\n".encode()
            + ACCEPT_PROMPT)


def completed(source: int, target: int, source_balance: int, target_balance: int) -> bytes:
    return (b">>> Transfer completed.\n"
            + f">>> Account {source} balance: {source_balance}\n".encode()
            + f">>> Account {target} balance: {target_balance}\n".encode()
            + READY_PROMPT)


def assert_balances(expected: list[int]) -> None:
    expected_bytes = b"".join(struct.pack("=ii", ACCOUNT_ID_START + i, b)
                              for i, b in enumerate(expected))
    if RECORD_PATH.read_bytes() != expected_bytes:
        raise JudgeError("accountRecord contents differ from expected records")


@contextmanager
def transfer_world():
    write_records(DEFAULT_BALANCES)
    with ExitStack() as stack:
        first = stack.enter_context(ServerProcess())
        second = stack.enter_context(ServerProcess())
        clients = []
        for port in [first.port, first.port, first.port, second.port]:
            client = Client(port)
            stack.callback(client.close)
            clients.append(client)
        yield clients  # sender, receiver, local observer, remote observer


def start_offer(sender: Client, receiver: Client, source=902001, target=902002, amount=100):
    receiver.command(f"receive {target}", receiving(target))
    sender.command(f"transfer {source} {target} {amount}",
                   b">>> Transfer requested.\n" + TRANSFER_PROMPT)
    receiver.expect(offer(source, target, amount))


def probe_free(client: Client, account_id: int, balance: int) -> None:
    client.command(f"update {account_id}", update_started(account_id, balance))
    client.command("close", CLOSED)


def test_transfer_accept() -> None:
    with transfer_world() as (sender, receiver, local, remote):
        start_offer(sender, receiver)
        assert_balances(DEFAULT_BALANCES)  # no early debit/credit
        for observer in [local, remote]:
            for account in [902001, 902002]:
                observer.command(f"read {account}", LOCKED)
                observer.command(f"update {account}", LOCKED)
            probe_free(observer, 902003, 0)
        receiver.command("accept", completed(902001, 902002, 400, 1300))
        sender.expect(completed(902001, 902002, 400, 1300))
        balances = DEFAULT_BALANCES.copy()
        balances[0], balances[1] = 400, 1300
        assert_balances(balances)
        for observer in [local, remote]:
            probe_free(observer, 902001, 400)
            probe_free(observer, 902002, 1300)
        # One-shot registration is removed; a new receiver may register.
        local.command("receive 902002", receiving(902002))
        local.command("cancel", b">>> Receive canceled.\n" + READY_PROMPT)


def test_transfer_reject_cancel() -> None:
    with transfer_world() as (sender, receiver, local, remote):
        for action in ["reject", "cancel"]:
            start_offer(sender, receiver)
            result = (b">>> Transfer rejected.\n" if action == "reject"
                      else b">>> Transfer canceled.\n") + READY_PROMPT
            actor, peer = (receiver, sender) if action == "reject" else (sender, receiver)
            actor.command(action, result)
            peer.expect(result)
            assert_balances(DEFAULT_BALANCES)
            for observer in [local, remote]:
                probe_free(observer, 902001, 500)
                probe_free(observer, 902002, 1200)


def test_transfer_disconnect() -> None:
    for disconnect_sender in [True, False]:
        with transfer_world() as (sender, receiver, local, remote):
            start_offer(sender, receiver)
            gone, survivor = ((sender, receiver) if disconnect_sender else (receiver, sender))
            gone.close()
            # This notification synchronizes cleanup; no fixed sleep required.
            survivor.expect(PEER_GONE)
            assert_balances(DEFAULT_BALANCES)
            for observer in [local, remote]:
                probe_free(observer, 902001, 500)
                probe_free(observer, 902002, 1200)
            local.command("receive 902002", receiving(902002))
            local.command("cancel", b">>> Receive canceled.\n" + READY_PROMPT)


def test_transfer_exact_ranges() -> None:
    with transfer_world() as (sender, receiver, local, remote):
        # Hold the intervening record: a contiguous 1..3 lock is incorrect.
        remote.command("update 902002", update_started(902002, 1200))
        start_offer(sender, receiver, 902001, 902003, 100)
        # Probe every byte of each required record from the checker process.
        # These are POSIX locks, not flock() locks.
        with RECORD_PATH.open("r+b", buffering=0) as f:
            for account in [902001, 902003]:
                for byte in range(struct.calcsize("=ii")):
                    offset = (account - ACCOUNT_ID_START) * 8 + byte
                    try:
                        fcntl.lockf(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB,
                                    1, offset, os.SEEK_SET)
                    except OSError as exc:
                        if exc.errno not in (errno.EACCES, errno.EAGAIN):
                            raise
                    else:
                        fcntl.lockf(f.fileno(), fcntl.LOCK_UN, 1, offset, os.SEEK_SET)
                        raise JudgeError(f"byte {byte} of account {account} is not locked")
        receiver.command("reject", b">>> Transfer rejected.\n" + READY_PROMPT)
        sender.expect(b">>> Transfer rejected.\n" + READY_PROMPT)
        local.command("update 902002", LOCKED)
        remote.command("close", CLOSED)
        assert_balances(DEFAULT_BALANCES)




def assert_quiet(client, duration=0.20):
    """Check that a failed setup causes no unsolicited offer or retry response."""
    if client.buffer:
        raise JudgeError(f"unexpected unsolicited output: {client.buffer!r}")
    previous = client.sock.gettimeout()
    client.sock.settimeout(duration)
    try:
        data = client.sock.recv(4096)
    except socket.timeout:
        return
    finally:
        client.sock.settimeout(previous)
    raise JudgeError(f"expected a pending connection with no new output, got {data!r}")


def wait_update_free(client, account, balance):
    """Bounded EOF cleanup probe; replaces old fixed 0.1 second sleeps."""
    deadline = time.monotonic() + 2.0
    while True:
        client.sock.sendall(f"update {account}\n".encode())
        data = b""
        while not data.endswith((READY_PROMPT, UPDATE_PROMPT)):
            chunk = client.sock.recv(1)
            if not chunk: raise JudgeError("connection closed in lock probe")
            data += chunk
            if len(data) > 1024:
                raise JudgeError("oversized response in lock probe")
        if data == update_started(account, balance):
            return
        if data != LOCKED or time.monotonic() >= deadline:
            raise JudgeError(f"lock not released or unexpected reply: {data!r}")
        time.sleep(0.02)


def read_reply(account, balance):
    return f">>> Account {account} balance: {balance}\n".encode() + READY_PROMPT


def run_cases(cases):
    failures = []
    for check in cases:
        try:
            check()
        except (JudgeError, OSError, struct.error) as exc:
            print(f"  [FAIL] {check.__name__}: {exc}")
            failures.append(check.__name__)
        else:
            print(f"  [PASS] {check.__name__}")
    if failures:
        raise JudgeError("failed cases: " + ", ".join(failures))


def test_update_session():
    write_records(DEFAULT_BALANCES)
    with ServerProcess() as server:
        c = Client(server.port)
        try:
            c.command("read 902001", read_reply(902001, 500))
            c.command("update 902001", update_started(902001, 500))
            for delta, balance in [(100, 600), (-30, 570), (0, 570)]:
                c.command(f"add {delta}", update_succeeded(902001, balance))
                # Each successful add is persisted BEFORE close.
                if read_balance(902001) != balance:
                    raise JudgeError("add must persist immediately")
            c.command("add -571", RANGE)
            c.command("add 2147483647", RANGE)
            c.command("add 10", update_succeeded(902001, 580))
            c.command("close", CLOSED)
            c.command("read 902001", read_reply(902001, 580))
            c.command("update 902002", update_started(902002, 1200))
            c.command("close", CLOSED)  # No add is required.
            c.command_and_expect_close("exit", b">>> Client exit.\n")
        finally:
            c.close()
    expected = DEFAULT_BALANCES.copy(); expected[0] = 580
    assert_balances(expected)


def test_update_exit_invalid():
    for terminal in ["exit", "cancel"]:
        write_records(DEFAULT_BALANCES)
        with ServerProcess() as server:
            c = Client(server.port)
            try:
                c.command("update 902001", update_started(902001, 500))
                c.command("add 25", update_succeeded(902001, 525))
                c.command_and_expect_close(terminal, b">>> Client exit.\n" if terminal == "exit" else INVALID)
            finally:
                c.close()
            with_client = Client(server.port)
            try:
                with_client.command("read 902001", read_reply(902001, 525))
                with_client.command_and_expect_close("exit", b">>> Client exit.\n")
            finally:
                with_client.close()


def task_1():
    run_cases(TASK_CASES["1"])

def test_remote_session_lock():
    write_records(DEFAULT_BALANCES)
    with ServerProcess() as s1, ServerProcess() as s2, ExitStack() as stack:
        a, b = Client(s1.port), Client(s2.port)
        stack.callback(a.close); stack.callback(b.close)
        a.command("update 902001", update_started(902001, 500))
        for command in ["read 902001", "update 902001"]:
            b.command(command, LOCKED)
        a.command("add 20", update_succeeded(902001, 520))
        b.command("update 902001", LOCKED)  # add must NOT release the lock.
        b.command("update 902002", update_started(902002, 1200))
        b.command("add -50", update_succeeded(902002, 1150))
        a.command("add -521", RANGE)
        with RECORD_PATH.open("r+b", buffering=0) as f:
            for offset in range(8):
                try:
                    fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, offset)
                except OSError as exc:
                    if exc.errno not in (errno.EACCES, errno.EAGAIN): raise
                else:
                    fcntl.lockf(f, fcntl.LOCK_UN, 1, offset)
                    raise JudgeError("update must protect every byte through close")
        a.command("close", CLOSED)
        a.command("update 902002", LOCKED)
        b.command("close", CLOSED)
        b.command("read 902001", read_reply(902001, 520))
        probe_free(a, 902002, 1150)
        # EOF releases ownership but never rolls back a completed add.
        a.command("update 902001", update_started(902001, 520))
        a.command("add 5", update_succeeded(902001, 525))
        a.close()
        wait_update_free(b, 902001, 525)
        b.command("close", CLOSED)


def test_shared_read_lock():
    write_records(DEFAULT_BALANCES)
    with ServerProcess() as server:
        c = Client(server.port)
        try:
            with RECORD_PATH.open("r+b", buffering=0) as f:
                fcntl.lockf(f, fcntl.LOCK_SH | fcntl.LOCK_NB, 8, 0)
                c.command("read 902001", read_reply(902001, 500))
                c.command("update 902001", LOCKED)
                c.command("read 902002", read_reply(902002, 1200))
                fcntl.lockf(f, fcntl.LOCK_UN, 8, 0)
            probe_free(c, 902001, 500)
        finally:
            c.close()


def task_2():
    run_cases(TASK_CASES["2"])

def test_batch_and_fragment():
    write_records(DEFAULT_BALANCES)
    with ServerProcess() as server, ExitStack() as stack:
        a, b = Client(server.port), Client(server.port)
        stack.callback(a.close); stack.callback(b.close)
        a.sock.sendall(b"read 902001\nupdate 902001\nadd 20\nadd -5\nclose\nread 902")
        a.expect(read_reply(902001, 500) + update_started(902001, 500)
                 + update_succeeded(902001, 520) + update_succeeded(902001, 515) + CLOSED)
        b.command("read 902001", read_reply(902001, 515))
        a.sock.sendall(b"001\nread 902003\nexit\n")
        a.expect(read_reply(902001, 515) + read_reply(902003, 0) + b">>> Client exit.\n")
        if a.sock.recv(1): raise JudgeError("exit must close socket")
        # A partial command must not stall other clients.
        b.sock.sendall(b"upda")
        c = Client(server.port); stack.callback(c.close)
        c.command("read 902002", read_reply(902002, 1200))
        b.sock.sendall(b"te 902001\n"); b.expect(update_started(902001, 515))
        # Test multiplexing on an independent record. Ownership belongs to
        # Task 2's private cases, which require the Task 3 event loop.
        c.command("read 902002", read_reply(902002, 1200))
        b.command("add 10", update_succeeded(902001, 525))
        c.command("read 902002", read_reply(902002, 1200))
        b.command("close", CLOSED)
        c.command("read 902001", read_reply(902001, 525))


def task_3():
    run_cases(TASK_CASES["3"])

def probe_external_free(account):
    # Independent process: local ownership alone cannot detect a leaked fcntl lock.
    with RECORD_PATH.open("r+b", buffering=0) as f:
        offset = (account - ACCOUNT_ID_START) * 8
        deadline = time.monotonic() + 2.0
        while True:
            try:
                fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB, 8, offset)
                break
            except OSError as exc:
                if exc.errno not in (errno.EACCES, errno.EAGAIN): raise
                if time.monotonic() >= deadline:
                    raise JudgeError("failed transfer leaked a partial record lock")
                time.sleep(0.01)
        fcntl.lockf(f, fcntl.LOCK_UN, 8, offset)


def test_transfer_conflict_returns_locked():
    for same_server in [True, False]:
        for locked_account in [902001, 902002]:
            with transfer_world() as (a, b, local, remote):
                owner = local if same_server else remote
                before = 500 if locked_account == 902001 else 1200
                owner.command(f"update {locked_account}", update_started(locked_account, before))
                b.command("receive 902002", receiving(902002))
                a.command("transfer 902001 902002 100", LOCKED)
                # No offer or queued message, and no leaked partial source lock.
                assert_quiet(b)
                if locked_account == 902002:
                    probe_external_free(902001)
                a.command("read 902003", read_reply(902003, 0))  # still READY
                owner.command("add 50", update_succeeded(locked_account, before + 50))
                a.command("transfer 902001 902002 100", LOCKED)  # add retains lock
                owner.command("close", CLOSED)
                # Unlocking does NOT resume the old failed request automatically.
                assert_quiet(a); assert_quiet(b)
                a.command("read 902001", read_reply(902001, 550 if locked_account == 902001 else 500))
                # Receiver registration remains, but sender must send a NEW request.
                a.command("transfer 902001 902002 100", b">>> Transfer requested.\n" + TRANSFER_PROMPT)
                b.expect(offer(902001, 902002, 100))
                src, dst = ((450, 1300) if locked_account == 902001 else (400, 1350))
                b.command("accept", completed(902001, 902002, src, dst))
                a.expect(completed(902001, 902002, src, dst))
                expected = DEFAULT_BALANCES.copy(); expected[0], expected[1] = src, dst
                assert_balances(expected)
                probe_free(remote, 902001, src); probe_free(local, 902002, dst)


def test_transfer_new_request_checks_latest_balance():
    with transfer_world() as (a, b, local, remote):
        remote.command("update 902004", update_started(902004, 999999))
        b.command("receive 902004", receiving(902004))
        a.command("transfer 902001 902004 1", LOCKED)
        probe_external_free(902001)
        remote.command("add 1", update_succeeded(902004, 1000000))
        remote.command("close", CLOSED)
        assert_quiet(a); assert_quiet(b)
        a.command("transfer 902001 902004 1", b">>> [Error] Balance out of range.\n" + READY_PROMPT)
        assert_quiet(b)  # failed setup still does not consume the receiver
        probe_free(local, 902001, 500); probe_free(remote, 902004, 1000000)
        b.command("cancel", b">>> Receive canceled.\n" + READY_PROMPT)






def task_4():
    run_cases(TASK_CASES["4"])

TASK_CASES = {
    "1": [test_update_session, test_update_exit_invalid],
    "2": [test_remote_session_lock, test_shared_read_lock],
    "3": [test_batch_and_fragment],
    "4": [test_transfer_accept, test_transfer_reject_cancel,
          test_transfer_disconnect, test_transfer_exact_ranges,
          test_transfer_conflict_returns_locked,
          test_transfer_new_request_checks_latest_balance],
}
TASKS: dict[str, Callable[[], None]] = {"1": task_1, "2": task_2, "3": task_3, "4": task_4}
POINTS = {"1": 1, "2": 3, "3": 1, "4": 2}
TASK_NAMES = {
    "1": "Commands and update sessions",
    "2": "Record locks and local ownership",
    "3": "Multiplexing and input buffering",
    "4": "Confirmed transfers and cleanup",
}
TEST_ERRORS = (JudgeError, OSError, struct.error)


def add_task_argument(parser):
    parser.add_argument("-t", "--task", nargs="+", choices=TASK_CASES,
                        default=list(TASK_CASES), metavar="{1,2,3,4}",
                        help="tasks to run (default: all); repeated IDs run once")


def selected_tasks(args):
    # Canonical ordering also prevents repeated selectors from inflating scores.
    return [task for task in TASK_CASES if task in args.task]


def run_task_suite(selected, cases_by_task, label):
    """Continue after failed cases/tasks; return case outcomes for each task."""
    results = {}
    for task in selected:
        print(f"\n[{label}] Task {task}: {TASK_NAMES[task]} ({POINTS[task]} points)", flush=True)
        outcomes = []
        for check in cases_by_task[task]:
            try:
                check()
            except TEST_ERRORS as exc:
                outcomes.append(False)
                print(f"  [FAIL] {check.__name__}: {exc}", flush=True)
            else:
                outcomes.append(True)
                print(f"  [PASS] {check.__name__}", flush=True)
        results[task] = outcomes
        state = "PASS" if outcomes and all(outcomes) else "FAIL"
        print(f"[{state}] {label} Task {task}: {sum(outcomes)}/{len(outcomes)} cases", flush=True)
    return results


def task_passed(results, task):
    return task in results and bool(results[task]) and all(results[task])


def result_cell(results, task):
    if task not in results:
        return "NOT RUN"
    values = results[task]
    state = "PASS" if task_passed(results, task) else "FAIL"
    return f"{state} {sum(values)}/{len(values)}"


def print_public_summary(results):
    print("\nPUBLIC SUMMARY (feedback only; NOT a final grade)")
    print(f"{'Task':<7}{'Public cases':<20}{'Public-only estimate':<23}")
    for task in TASK_CASES:
        score = f"{POINTS[task] if task_passed(results, task) else 0}/{POINTS[task]}" if task in results else "--"
        print(f"{task:<7}{result_cell(results, task):<20}{score:<23}")
    earned = sum(POINTS[t] for t in results if task_passed(results, t))
    possible = sum(POINTS[t] for t in results)
    print(f"Public-only estimate for selected tasks: {earned}/{possible}; full assignment: 7 points.")
    print("Each estimate uses all-or-nothing per task, NOT per-case partial credit.")
    print("TAs will grade with ADDITIONAL HIDDEN TESTS and code review.")
    print("Passing public tests does NOT guarantee the corresponding points.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_task_argument(parser)
    args = parser.parse_args()
    if not RECORD_PATH.is_file() or not SERVER_PATH.is_file():
        print("[ERROR] server or accountRecord is missing. Run make first.", file=sys.stderr)
        return 2
    original = RECORD_PATH.read_bytes()
    try:
        results = run_task_suite(selected_tasks(args), TASK_CASES, "PUBLIC")
    finally:
        RECORD_PATH.write_bytes(original)
    print_public_summary(results)
    return int(any(not task_passed(results, t) for t in results))


if __name__ == "__main__":
    raise SystemExit(main())
