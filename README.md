# SP2026 HW1 — csieLedger

Revision 2026-09-29: four TODOs, total **7 points**.
Full student protocol: [SPEC.md](SPEC.md). Changes: [CHANGELOG.md](CHANGELOG.md).

| TODO | Work | Points |
|---|---|---:|
| 1 | read/exit and update → add → close sessions; complete response writes | 1 |
| 2 | Nonblocking fcntl record locks, local ownership, cleanup | 3 |
| 3 | select, fragments, multiple commands per send | 1 |
| 4 | Two-client confirmed transfer and deferred record acquisition | 2 |

## Build and develop

Linux, GCC, make, Python 3.9+ (standard library only).
Implement **1 → 2 → 3 → 4**; rebuild after editing source.

```bash
make
python3 checker.py --task 1
python3 checker.py --task 2
python3 checker.py --task 3
python3 checker.py --task 4
python3 checker.py
```

Complete send_text first; it sends the initial welcome message. The provided
sequential loop supports developing Tasks 1–2. Implement select for Tasks 3–4.
There is no stage3 driver or special build. The starter compiles but does not
pass until its TODOs are implemented.

The checker temporarily rewrites/restores accountRecord. Do not run other
checkers or manual servers against this directory while a checker runs.

## Update session

READY accepts `read <id>`, `update <id>`, `receive <id>`,
`transfer <source> <target> <amount>`, and `exit`.

An acquired update enters WAIT_UPDATE:

```text
update 902001
add 100
add -30
close
read 902001
exit
```

Starting at 500, the adds immediately write 600 and then 570. They keep the
write lock. close releases it, returns READY, and leaves the socket open and
balance 570. exit disconnects. No update path rolls back completed adds.
An out-of-range add keeps the old balance, state, and lock for another attempt.

cancel is invalid in WAIT_UPDATE. There is no blocking update mode: only
`update <id>` is valid, and any lock conflict immediately returns Locked.
Transfer and receive still accept cancel in their specified states.

## Transfer

B registers with `receive 902002`; A sends `transfer 902001 902002 100`.
Both clients must connect to the same server. Transfer waits for conflicting
records with F_SETLK retries and the supplied timer, while select continues
serving other clients. Never keep the first lock when the second conflicts.
Read balances only after both locks are acquired. Then send the offer; keep
both locks until B accepts/rejects or either participant cancels/leaves.
Only acceptance writes the transfer. A competing update returns Locked.

## Provided versus student work

Socket setup, parsing, record I/O, input buffers, dispatch, transfer timer,
and deferred close infrastructure are provided. Students implement handlers,
lock/state ownership and cleanup, drain_commands, and select integration.
Exact messages, invalid-input rules, and transfer waiting states are in SPEC.

## Submission

Submit `<student_id>_hw1.zip` to NTU COOL with server.c, server.h, Makefile,
and any required extra C/header files. Exclude binaries, tests, accountRecord,
and TA material. GitHub Issues is for Q&A; Classroom is not required.
