# System Programming 2026: csieLedger

> Revision: 2026-09-29 — four TODOs, update sessions with `close`, total 7 points.

###### tags: `NTU-SP`

[TOC]

:::danger
## Important Information

- **Deadline:** `{YYYY/MM/DD HH:MM}`
- **Discussion / Q&A:** `{COURSE_DISCUSSION_LINK}`
- **Submission:** `{NTU_COOL_SUBMISSION_LINK}`
- **Grading environment:** `{linux1 / designated workstation}`
- **Release package:** `SP2026_HW1_release_4todo.zip`

Spaces, capitalization, newlines, and prompts are part of the protocol.
:::

## 0. Quick Summary

Complete a C account server that uses a fixed-record file, POSIX record locks,
and one `select()` event loop.

| TODO / Public task | Work | Points |
|---:|---|---:|
| 1 | Complete response writes, read/exit, update → add → close sessions | 1 |
| 2 | Nonblocking `fcntl()` record locks and local ownership | 3 |
| 3 | `select()`, concurrent clients, fragmented and batched input | 1 |
| 4 | Two-client confirmed transfer, deferred two-record acquisition, cleanup | 2 |
| | **Total** | **7** |

Implement **1 → 2 → 3 → 4**. Use one build throughout:

```bash
make
python3 checker.py --task 1
python3 checker.py --task 2
python3 checker.py --task 3
python3 checker.py --task 4
python3 checker.py
```

Rebuild with `make` after changing source code. The sequential starter allows
Tasks 1 and 2 to be developed before implementing the Task 3 event loop.

The main rules are:

- `update <id>` either acquires the account immediately or replies `Locked`.
  There is no blocking update mode and no update waiting queue.
- After acquiring an update lock, the client can send multiple `add <delta>`
  commands. Each successful add writes immediately and **retains the lock**.
- `close` ends the update session, releases its lock, and returns to READY.
  It does **not** undo earlier adds and does **not** close the client socket.
- `exit`, invalid input, and disconnection clean up the connection; previously
  successful adds remain in the account file.
- One client may send multiple complete commands in one write. Process them
  in order and retain any incomplete tail for later input.
- Transfer needs two clients on the **same server**. Transfer lock conflicts
  may wait and retry while `select()` continues serving other clients.
  This transfer-only waiting rule is different from `update`'s immediate failure.

:::info
### Changes from the previous release

Old TODOs 1 and 2 are now TODO 1. Old TODOs 3, 4, 5 become TODOs 2, 3, 4.
The old 3B update mode and stage3 development build have been removed.
In WAIT_UPDATE, use `close` instead of `cancel`. An `add` no longer ends the
session; both successful adds and balance-range failures stay in WAIT_UPDATE.
Transfer/receive cancellation still uses `cancel` where allowed.
:::

## 1. Goal and Architecture

One server process manages multiple TCP client connections. Multiple server
processes can open the **same `accountRecord` file** and must coordinate with
`fcntl()` byte-range locks. For local multi-client exclusion, also maintain
`record_owner[]` because traditional POSIX record locks belong to a process.

Every accepted connection has its own `conn_fd`, state, input buffer, and
operation fields. One connection may issue many commands over its lifetime.
An fd may be reused after disconnection; reset all connection fields on reuse.

Different clients may update different accounts in overlapping sessions.
Only one updater or acquired transfer may own a particular account at a time.

Use a single process with a single event loop per server. Do not use fork,
threads, busy waiting, or `F_SETLKW`. The grader starts separate server
processes on different ports for interprocess lock tests.

## 2. Release Files and Build

```text
SP2026_HW1_release_4todo/
├── Makefile
├── README.md
├── SPEC.md
├── CHANGELOG.md
├── accountRecord
├── checker.py
├── server.c
└── server.h
```

`SPEC.md` is identical to this assignment. Search `server.c` for TODO 1–4.
The starter compiles but intentionally does not pass the tests.

Supplied infrastructure includes socket creation/bind/listen, opening the
record file, per-client storage, strict parsers, record I/O helpers,
`append_input()`/`pop_command()`, command dispatch, deferred failed-notification
cleanup, and a monotonic transfer-retry timer. You implement command handlers,
lock/state cleanup, buffer draining, and the select loop.

```bash
make                     # produces ./server
./server 7777
# Another terminal:
nc 127.0.0.1 7777
# Another server sharing the same accountRecord, for manual lock tests:
./server 7778
make clean
```

Use Linux, GCC, make, and Python 3.9+ (standard library only). Compile without
warnings. The default `make` target must generate an executable named `server`.
Do not modify the public checker to make your solution pass. Do not hard-code
the released balances. A checker temporarily rewrites and restores the record
file, so do not run it alongside another checker or a manual server in the
same directory.

## 3. Account File

There are 20 fixed-size records, IDs `902001` through `902020`:

```c
typedef struct {
    int id;
    int balance;
} account_record;
```

On the grading platform this is an 8-byte record with two 32-bit integers.
Balances are between **0 and 1,000,000**, inclusive.

```c
index  = account_id - ACCOUNT_ID_START;
offset = index * sizeof(account_record);
```

Use the supplied `read_record_at()`/`write_record_at()` helpers (`pread`/`pwrite`).
Do not overwrite the ID or unrelated records. Use a wide intermediate type
when adding a delta or validating a transfer; signed overflow is not allowed.

The initial public data are:

```text
500 1200 0 999999 350 42 7600 18 910 1000000
240 87 650 3000 1 777 25000 64 880 150
```

These correspond to ascending IDs. Other tests may use different balances.

## 4. Protocol and Per-Client State

Every command ends with `\n`; `\r\n` is also accepted. Commands are
case-sensitive, have no leading/trailing whitespace, and use exactly one
ASCII space between tokens. Integers are decimal values representable by
32-bit signed `int`; a leading sign is allowed. Account IDs must be in range.
Transfer amounts must be strictly positive; add deltas may be positive, zero,
or negative. Inputs are ordinary text without embedded NUL bytes.

TCP is a byte stream: a command may span multiple reads, and one read may
contain multiple complete commands. A client's outstanding input burst is
at most 512 bytes including line endings and a partial tail. Test clients read
responses normally; full nonblocking output queues are outside scope.

On connection, send exactly:

```text
================================
 Welcome to CSIE Ledger System 
================================
Please enter your command: 
```

The welcome line has a space before its newline. Prompts have a trailing space
and **no terminating newline**. Their exact C strings are:

```c
"Please enter your command: "                 /* READY */
"Please enter add <delta> or close: "          /* WAIT_UPDATE */
```

| Client state | Commands | State after operation |
|---|---|---|
| READY | read, update, receive, transfer, exit | Depends on command |
| WAIT_UPDATE | add, close, exit | add stays; close → READY; exit disconnects |
| WAIT_RECEIVE | cancel, exit | cancel → READY; exit disconnects |
| WAIT_TRANSFER_LOCK | cancel, exit | Paired transfer waiting for records |
| WAIT_TRANSFER_PEER | reject, exit | Receiver reserved by that waiting transfer |
| WAIT_TRANSFER_OUT | cancel, exit | Sender awaiting receiver decision |
| WAIT_TRANSFER_IN | accept, reject, exit | Receiver deciding an acquired offer |

State is per client. Other commands in that state are invalid and disconnect
that client after cleanup. There is no `WAIT_UPDATE_LOCK` state.

## 5. TODO 1 / Public Task 1 — Commands and Update Sessions (1 point)

Complete `send_text()` first because the welcome message uses it. Write the
entire string, handling short writes and `EINTR`. Return 0 on success and -1
on failure, including a write that makes no progress.

### 5.1 READY commands

`read <id>` reads a record and stays READY. For `read 902001` with balance 500:

```c
">>> Account 902001 balance: 500\nPlease enter your command: "
```

`update <id>` begins a session for that account. After acquiring the lock under
Task 2, read its current balance and enter WAIT_UPDATE. With initial balance 500:

```c
">>> Account 902001 balance: 500\n>>> Update lock acquired.\nPlease enter add <delta> or close: "
```

Only the short `update <id>` form is valid. `update <id> blocking` and
`update <id> nonblocking` contain extra arguments and are invalid.

`exit` replies `>>> Client exit.\n`, cleans up, and closes the client socket.
No READY prompt follows. The listening socket and other clients remain active.

### 5.2 add: apply immediately and keep the session

In WAIT_UPDATE, `add <delta>` applies to the **current balance** of this session.
Write each successful result immediately, update the cached current balance,
and stay WAIT_UPDATE. Keep the record lock and local ownership.

For an add that changes 500 to 600:

```c
">>> Update successful.\n>>> Account 902001 balance: 600\nPlease enter add <delta> or close: "
```

A later `add -30` must start from 600 and produce 570. Do not reuse the balance
from the start of the session. Do not defer writing until close.

If the resulting balance would be outside 0–1,000,000, do not change it. Reply:

```c
">>> [Error] Balance out of range.\nPlease enter add <delta> or close: "
```

Keep the session and lock so the client can try another add or close.
A syntactically invalid delta instead follows the invalid-command rule.

### 5.3 close: release without rollback

In WAIT_UPDATE, `close` releases the update record lock, clears its matching
local owner and session fields, and returns to READY on the **same connection**:

```c
">>> Update closed.\nPlease enter your command: "
```

It is valid even if no add occurred. It does not undo any successful add.
`cancel` is invalid in WAIT_UPDATE; `close` is invalid in READY and transfer states.

Example (arrows label sent commands, not protocol text):

```text
update 902001  → balance 500; WAIT_UPDATE
add 100        → file balance 600; still WAIT_UPDATE, still locked
add -30        → file balance 570; still WAIT_UPDATE, still locked
close          → balance stays 570; unlocked; READY
read 902001    → balance 570
exit           → disconnect
```

### 5.4 Connection cleanup does not undo writes

If a client exits, sends invalid input, or disconnects during WAIT_UPDATE,
release its lock and owner. Preserve all successful adds already written.
For example, after `add 25` changes 500 to 525, a disconnect leaves 525.

Task 1 combines the former basic-command and update-workflow tasks. Its public
cases include repeated adds, immediate persistence, range errors, close with
and without adds, continued use of the same connection, and exit/invalid cleanup.

## 6. TODO 2 / Public Task 2 — Nonblocking Record Locks (3 points)

Use traditional POSIX `fcntl(record_fd, F_SETLK, &lock)` record locks.
The call must attempt the lock immediately; do not use `F_SETLKW`, sleep until
an update lock is free, or enqueue an update for later retry.

Lock exactly the requested record:

```c
l_whence = SEEK_SET;
l_start  = index * sizeof(account_record);
l_len    = sizeof(account_record);
```

| Operation | Lock type | Lifetime |
|---|---|---|
| read | F_RDLCK | Acquire before reading; release after reading |
| update | F_WRLCK | Before initial read, through all adds, until close/cleanup |
| transfer | Two F_WRLCK locks | See Task 4 |

For `read`/`update`, either a local-owner conflict or `F_SETLK` failure with
`EACCES`/`EAGAIN` returns immediately:

```c
">>> Locked.\nPlease enter your command: "
```

The requester remains READY and acquires no ownership. Other syscall failures
must follow error cleanup rather than being treated as a successful acquisition.
A failed attempt must never release somebody else's lock.

Maintain `record_owner[index]`: `-1` means unowned; otherwise it is the owning
client fd. Check it before reading or taking a lock, because fcntl cannot
separate clients inside the same process. An add success or range error must
not clear this entry. On cleanup, clear/unlock only ownership belonging to
that operation.

Shared read locks held by different processes are compatible. A write lock
conflicts with another process's read or write lock. Different records remain
independent. Keep the original `record_fd` open for the server lifetime;
opening/closing another fd for the same file can release this process's locks.

Concurrency checks hold a client in WAIT_UPDATE. This makes lock conflicts
repeatable without relying on two brief operations happening at the same time.
The public Task 2 uses two server processes, each with one active client, so
it works with the sequential starter. Same-server ownership is also checked
after Task 3 is implemented and in additional grading cases.

## 7. TODO 3 / Public Task 3 — Multiplexing and Input (1 point)

Replace the sequential `serve_clients()` loop with a `select()` event loop.
Monitor the listener and all connected clients. Use a master fd_set and a
fresh working copy for each select call. Remove disconnected fds from the
master set and reset their request entries.

Maintain a separate state and buffer for every client. A partial command or a
client waiting for its next add must not stall everyone else.

Implement `drain_commands()` to consume **all complete lines** in order using
`pop_command()` and `handle_command()`. Preserve a partial tail. Evaluate each
line in the state left by the preceding command. For example, a single send:

```text
read 902001
update 902001
add 20
add -5
close
read 902001
```

With initial balance 500, return six normal responses, in order. The two adds
show 520 and 515 with UPDATE prompts; close has a READY prompt; the last read
shows 515. Close does not stop draining the connection.

`exit` or invalid input ends that connection: send the terminal response and
discard all later commands in the batch. Retain earlier successful adds.
If a later read line is incomplete, save it and let other clients run.

For Task 4 integration, the supplied transfer timer provides
`lock_retry_timeout(&timeout)` for select and `service_lock_waiters()` after a
readable batch or timeout. Call `close_failed_clients(&master)` before select
and skip close_pending clients during processing. Failed-notification cleanup
must not invalidate another peer while its handler is still executing.

The timer sleeps through select; it does not spin. It retries transfer record
locks every 50 ms using a monotonic deadline, so other incoming socket traffic
cannot indefinitely reset the retry deadline. Under the test load, a pending
transfer should progress within 2 seconds once the records remain available.
No FIFO or cross-process fairness guarantee is required.

Task 3 includes concurrent clients, same-server ownership, fragmented input,
and one send containing multiple complete commands plus an incomplete tail.
EOF/error paths must also remove the socket and release any owned records.

## 8. TODO 4 / Public Task 4 — Confirmed Transfer with Deferred Locks

B registers to receive; A requests a transfer; B accepts before money moves.
Both participants connect to the same server. Other servers share the same
record file and may temporarily hold the records this transfer needs.

**Transfer-only waiting:** a transfer encountering a local or cross-server write-lock
conflict waits and retries. It must not immediately fail with `Locked`.
Only after both locks are acquired may it read, validate, and offer the transfer.

### 8.1 Registration

In READY, B sends `receive <target_id>`. For `receive 902002`, reply:

```text
>>> Ready to receive on account 902002.
Waiting for transfer; enter cancel or exit: 
```

B enters WAIT_RECEIVE. This takes no record lock, and other operations on the
account remain possible. Each account has at most one registered receiver per
server, including a receiver reserved by a queued transfer or an active offer.
A duplicate registration replies:

```c
">>> [Error] Receiver already registered.\nPlease enter your command: "
```

The new requester stays READY; the original registration is unchanged.
In WAIT_RECEIVE, `cancel` removes the registration and replies:

```c
">>> Receive canceled.\nPlease enter your command: "
```

`exit`, invalid input, and detected disconnection also remove it. Registration
is local to the server and is only connection matching, not bank authentication.
A receiver on another server cannot be matched. No login is required.

### 8.2 Request and acquisition

A sends in READY:

```text
transfer <source_id> <target_id> <amount>
```

Both IDs must be valid and different; amount must be a positive base-10 `int`.
A leading plus is allowed. Zero, negatives, overflow, and invalid arguments
follow the usual invalid-command-and-close rule.

Apply these checks in order:

1. Validate syntax and arguments.
2. Find a different registered client in WAIT_RECEIVE on this server. If none
   is available, reply `>>> [Error] Receiver unavailable.\n` plus READY prompt.
3. Attempt the source write lock, then the target write lock, checking both
   local ownership entries before the syscalls. If either conflicts, enter
   the paired lock wait below. **Release any first lock before waiting.**
4. After both locks are held, read both balances and require source balance
   at least amount and target balance plus amount at most MAX_BALANCE. Use
   wide arithmetic. Never cache the balances before acquisition.
5. If valid, create an offer and keep both locks through the decision.

No failed acquisition attempt may leave either record owned by the waiter.
Do not hold the first lock while waiting for the second. This rule applies to
every retry as well as the initial attempt. Unrelated records remain usable.

If the initial acquisition succeeds but the balances are invalid, release both
locks and reply **only to A**:

```c
">>> [Error] Balance out of range.\nPlease enter your command: "
```

A stays READY, B remains WAIT_RECEIVE, and B receives no notification.

### 8.3 Paired lock wait

If acquisition conflicts, reserve B for this request. A enters
WAIT_TRANSFER_LOCK; B enters WAIT_TRANSFER_PEER. Send once to A:

```text
>>> Waiting for transfer locks.
Waiting for locks; enter cancel or exit: 
```

Send once to B, using the actual IDs and amount:

```text
>>> Transfer queued: 902001 -> 902002, amount: 100
Waiting for sender locks; enter reject or exit: 
```

There is no READY prompt and no balance modification. At most one transfer can
reserve a given receiver. Another sender targeting that receiver gets
`Receiver unavailable`, rather than joining the same offer.

Retry through the supplied timer integrated in Task 3. Do not repeatedly notify clients
on each failed attempt. There are no held record locks between failed attempts,
so other clients may still use an otherwise available source/target account.
When retry succeeds, read **both latest balances** and validate again.

- Valid: send the normal offer notifications below and keep both locks.
- Invalid: release the two locks, remove registration, reset both clients to
  READY, and send `Balance out of range` plus READY prompt to **both**. Unlike
  immediate validation failure, this ends an already paired request.

A may `cancel`; B may `reject`; either may `exit` or disconnect while waiting.
Use the same paired cleanup and messages as an active offer. B cannot `accept`
before receiving an offer: that command in WAIT_TRANSFER_PEER is invalid.

### 8.4 Offer and confirmation

After both locks and valid balances, A enters WAIT_TRANSFER_OUT and receives:

```text
>>> Transfer requested.
Waiting for receiver; enter cancel or exit: 
```

B enters WAIT_TRANSFER_IN and receives:

```text
>>> Transfer offer: 902001 -> 902002, amount: 100
Please enter accept or reject: 
```

An immediately available transfer sends only these notifications. A queued
transfer sends these after its earlier waiting notifications. Cross-socket
notification order is not graded, but order within each socket is fixed.

**Do not write yet.** During WAIT_TRANSFER_OUT/IN, both records are owned by
the sender fd in `record_owner[]` and are write-locked across processes.
`read` and `update` on either account return Locked immediately, including
requests from another server. A client may issue a new command after the
transfer ends; there is no automatic update retry.

Only B may send `accept`. While retaining both locks, write source minus amount
and target plus amount. Then unlock both, clear registration/pairing, return
both clients to READY, and send the same result to both. For 500 and 1200
transferring 100:

```text
>>> Transfer completed.
>>> Account 902001 balance: 400
>>> Account 902002 balance: 1300
Please enter your command: 
```

List source first even if its ID is numerically larger. An unrelated or repeated
`accept` is invalid and cannot apply another transfer.

### 8.5 Paired cancellation and disconnection

These rules cover both a lock-waiting pair and an acquired offer:

| Action | Response to each still-connected participant | Balance change |
|---|---|---|
| A `cancel` | `>>> Transfer canceled.\n` + READY prompt | None |
| B `reject` | `>>> Transfer rejected.\n` + READY prompt | None |
| One participant leaves | Survivor receives peer-disconnected message below | None before acceptance |

For a departing client's `exit`, send only `>>> Client exit.\n` to that client.
For invalid input, send only `>>> [Error] Invalid command.\n` to that client.
Then close it. Send the survivor:

```text
>>> Transfer canceled: peer disconnected.
Please enter your command: 
```

EOF, partial-command disconnection, input errors, and failed notifications also
run paired cleanup. Release only locks actually acquired by this pair; a queued
waiter must never unlock another client's records. Reset both participants and
remove the registration so a reused fd cannot inherit an old request.

Use the supplied close_pending/deferred-close infrastructure when notifying a
peer fails. Clear paired state before sending terminal notifications so cleanup
cannot recursively cancel the same offer twice.

After any offer or paired wait ends, B must register again for another transfer.
A terminal action is processed in event-loop order. If acceptance completes
before EOF is detected, it is not undone. No special priority between truly
simultaneous cancel/accept requests is required.

### 8.6 State table

| State | Accepted commands | Held records |
|---|---|---|
| WAIT_RECEIVE | cancel, exit | None |
| WAIT_TRANSFER_LOCK (A) | cancel, exit | None between attempts |
| WAIT_TRANSFER_PEER (B) | reject, exit | None |
| WAIT_TRANSFER_OUT (A) | cancel, exit | Both write-locked, locally owned by A |
| WAIT_TRANSFER_IN (B) | accept, reject, exit | Owned by the paired sender |

Other commands in these transfer states are invalid. Transfer decisions must
wait for their notifications. Do not pipeline `receive` with `accept`, or an
`accept` before the offer. At most one pending operation per client is allowed.

### 8.7 Scope and grading cases

The record format stays unchanged. The starter provides the extra states,
receiver table, request storage, parsers, dispatcher, timer and deferred-close
infrastructure. The acquisition/retry/state/cleanup logic is student work.

No transfer-confirmation timeout or global fairness guarantee is required.
Both participants must use the same server; only record conflicts cross
processes. Use the original record fd throughout the server lifetime.

File I/O is assumed to succeed in graded cases. Two `pwrite()` calls do not
provide a crash-safe transaction. Disk failure, process termination during the
writes, journaling, and recovery are outside scope. Responsive clients and
small replies are assumed; full nonblocking output queues are not required.

Public cases cover acceptance, rejection/cancellation, either participant's
disconnection, exact nonadjacent record ranges, deferred transfer until an
updater sends close, and balance revalidation after the wait. They verify that
successful adds retain the updater's lock, so transfer cannot proceed until
that session closes or disconnects. Other tests may combine documented
waiting, batching, invalid commands, connection reuse, and independent pairs.

---

## 9. Invalid Input and Cleanup

Unknown commands, wrong-state commands, invalid IDs, missing/extra arguments,
malformed/overflowing integers, and incorrect whitespace are invalid. Reply:

```c
">>> [Error] Invalid command.\n"
```

Then clean up and disconnect that client. Do not send a READY prompt.
Wrong-state `cancel` in WAIT_UPDATE is invalid; cleanup still preserves earlier
successful adds. `close` in READY is invalid. Transfer/receive `cancel` remains
valid only in the states listed in Section 8.

A syntactically valid add with an out-of-range **result** stays WAIT_UPDATE
(Section 5.2). An invalid transfer amount is a protocol error; a valid amount
that cannot be covered by the balances is a balance error (Section 8).

EOF may occur while receiving a partial command. Discard that partial input,
release any acquired update/transfer locks, clear matching ownership and
registration, notify a paired survivor when applicable, close the fd, and
reset the request. Cleanup may be invoked through multiple paths and must be
safe when no lock was acquired. A queued transfer must not unlock the client
that is currently preventing its acquisition.

## 10. Public Checker and Grading

```bash
make
python3 checker.py --task 1
python3 checker.py --task 2
python3 checker.py --task 3
python3 checker.py --task 4
python3 checker.py --task 1 2 3 4
python3 checker.py
```

Valid task selectors are **1, 2, 3, 4** only. There are no 3N/3B selectors or
stage3 development build. The same executable `server` is used throughout.
Public checker results are feedback, not the full weighted final grade.

| Task | Points | Grading focus |
|---|---:|---|
| 1 | 1 | Basic commands, immediate and repeated adds, close/exit, no rollback |
| 2 | 3 | Exact fcntl lock ranges/types, local ownership, lock lifetime and cleanup |
| 3 | 1 | select, per-client state/buffers, fragments and multiple commands |
| 4 | 2 | Receiver matching, two-record protection, deferred transfer and cleanup |
| **Total** | **7** | |

Hidden tests may combine documented behaviors, dynamic balances, boundaries,
wrong-state inputs, terminal commands inside batches, disconnections, and
independent concurrent operations. Same-server ownership is attributed to
Task 2 even when its test needs Task 3's event loop. Passing a public task does
not guarantee full credit. Code review may check use of the required APIs,
absence of busy waiting, and cleanup/resource correctness.

The checker overwrites and restores accountRecord. Never run two checkers or
manual servers against the same directory at the same time.

## 11. Submission

Submit `<student_id>_hw1.zip` to NTU COOL. GitHub Issues is for questions;
GitHub Classroom or pushing a student repository is not required.

Include:

```text
server.c
server.h
Makefile
```

Include any additional C source/header files required to build your solution.
After extraction, `make` must generate `server` without development drivers.
Do not submit binaries, object files, accountRecord, checker.py, private
checkers, TA solutions, core dumps, or Python cache files.

Deadline, submission link, late rules, and the designated workstation will be
provided by the course announcement.

## 12. FAQ

**Does close close the TCP socket?**
No. It ends a WAIT_UPDATE session and returns the same client to READY.
Use exit to disconnect normally.

**Does close/cancel/disconnection roll back an earlier add?**
No. Each successful add is already written. In WAIT_UPDATE, cancel is invalid
and disconnects, but even then earlier successful adds remain.
Transfer cancellation prevents an unaccepted transfer from being applied;
it does not undo unrelated updates already committed by another operation.

**Why does add keep its write lock?**
The session remains active for further adds. Keeping the lock avoids exposing
the session's account to competing updates until close or connection cleanup.

**Is there a blocking update command?**
No. update immediately replies Locked if it cannot acquire the record.
An already held session continues until its client closes it or disconnects.

**Does a waiting transfer stop the server?**
No. F_SETLK attempts return immediately. If either lock conflicts, release any
partial acquisition, save the pair's waiting state, and return to select.
The provided timer schedules another attempt. select waits for socket events
or its timeout; it does not directly notify you of file-lock release.

**When are balances read for a transfer?**
Only after both locks have been acquired, including after a wait. An updater
may have performed several adds before close; use the latest resulting values.

**Can the two transfer clients connect to different servers?**
No. Receiver matching is local. Other servers participate only through shared
record-file locking; they do not exchange transfer notifications.

**Does this assignment guarantee crash-safe transfers?**
No. Locking coordinates cooperating processes, but two separate record writes
are not a crash-safe transaction. Storage failure recovery is outside scope.
