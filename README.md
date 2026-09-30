# SP2026 HW1 — csieLedger

Revision 2026-09-29: four TODOs, total **7 points**.
## Latest assignment specification

**Read the latest assignment specification on HackMD:**

[SP2026 HW1 — Assignment Specification](https://hackmd.io/9LiX65KYRPC6uxadugACkQ?both)

The complete assignment requirements, command protocol, exact responses, grading
criteria, and FAQ are maintained on HackMD. This README is a quick-start guide;
always refer to HackMD for the latest specification and clarifications.

A separate `SPEC.md` is no longer maintained in this repository. If you are
looking for `SPEC.md`, use the HackMD link above.

Changes: [CHANGELOG.md](CHANGELOG.md).

| TODO | Work | Points |
|---|---|---:|
| 1 | read/exit and update → add → close sessions; complete response writes | 1 |
| 2 | Nonblocking fcntl record locks, local ownership, cleanup | 3 |
| 3 | select, fragments, multiple commands per send | 1 |
| 4 | Two-client confirmed transfer and immediate lock failure | 2 |

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

TAs will use additional hidden tests for grading. Passing all public tests does
not guarantee full credit.

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
Both clients must connect to the same server. If either account is occupied,
reply Locked immediately to A, release any partially acquired lock, and leave
A READY and B registered in WAIT_RECEIVE. Do not create a pending request or
notify B. There is no timer or automatic retry; A must send a new transfer.

After both locks succeed, read/check current balances and send the offer. Keep
both locks until B accepts/rejects or either participant cancels/leaves. Only
acceptance writes the transfer. select continues serving other clients while
waiting for B's decision. A competing read/update/transfer cannot acquire the
owned records. Unrelated accounts remain usable.

## Provided versus student work

Socket setup, parsing, record I/O, input buffers, dispatch,
and deferred close infrastructure are provided. Students implement handlers,
lock/state ownership and cleanup, drain_commands, and select integration.
Exact messages, invalid-input rules, and transfer states are in the
[HackMD specification](https://hackmd.io/9LiX65KYRPC6uxadugACkQ).

## Submission

Submit `<student_id>_hw1.zip` to NTU COOL with server.c, server.h, Makefile,
and any required extra C/header files. Exclude binaries, tests, accountRecord,
and TA material. GitHub Issues is for Q&A; Classroom is not required.

## Questions and updates

Read the FAQ in HackMD and search existing issues before posting a question.
Please use [GitHub Issues](https://github.com/ntusp2026/SP2026_HW1_release/issues)
for assignment questions so students and TAs from both classes can share
clarifications. Do not publish your solution code in issues.

Check HackMD and GitHub Issues regularly for updates.

