# S10: SQLite document file, kill during save, lock file, network share

Run on 2026-11-17.

## Result in short

| Acceptance item | Result |
|---|---|
| 1. 500 kills during save, local disk, zero unreadable or inconsistent files | PASS (0 failures in 500) |
| 2. Second opener sees who holds the file; stale lock detected and recoverable | PASS (local and loopback SMB) |
| 3. Save cost, 200 MB file, 50 KB changed | Measured: 8 to 17 ms and 132 to 259 KB written per save (local) |
| 4. Same on a network share | PARTLY DONE: loopback SMB only (`\\localhost\C$`). A real NAS or file server was not tested. |

One design problem was found that the acceptance items do not cover: the "snapshot copy" that PLAN.md 4.3 gives to
read-only openers is corrupt most of the time if the copy overlaps a save (see "Snapshot copy for readers").

## Question

Does the PLAN.md section 4 design hold: one `.raycad` SQLite file, rollback journal, exclusive lock, save as one
transaction, a `<name>.raycad.lock` file for the second opener, and is an incremental save of a large file cheap?

## Method

One program, `main.cpp` (`build/s10.exe`), with sub-commands. SQLite 3.54.1 amalgamation
(`sqlite-amalgamation-3540100.zip` from sqlite.org, public domain, SHA3-256
`92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`, matches the download page), default compile options,
MSVC 14.51, Release.

Settings used everywhere, as in PLAN.md 4.1, and read back from SQLite:
`journal_mode=DELETE`, `synchronous=FULL` (2), `locking_mode=EXCLUSIVE`, `page_size=4096`, `cache_size=-2000` (2 MB, default).

Schema: the seven tables of PLAN.md 4.1, unchanged. Objects are keyed by SHA-256 of their bytes. Blobs are random
bytes (stands in for zstd output; zstd itself is not used). A document state is a manifest object listing one blob hash
per slot. A commit id is the hash of (parent, root, author, time, message).

**Kill test.** The writer opens the file and saves in a loop with no pause. Each save is one transaction: 1 to 4 new
blobs of 1 KB to 256 KB (10 % of blobs are 2 to 6 MB, which is larger than the page cache and forces SQLite to write
database pages before the commit), a new manifest, a commit row, a parent row, and the move of `heads/main`.
The harness starts the writer, waits a uniform random 0 to 200 ms, calls `TerminateProcess`, then:

- reads the raw `-journal` file header before anything reopens the database;
- one time in five, starts a second writer on the un-recovered file and kills it within 0 to 25 ms (kill during
  hot-journal rollback);
- runs the checker: `PRAGMA integrity_check` must return `ok`; `heads/main` must point to a commit; the whole commit
  chain back to commit 0 is walked; every commit id, every manifest and every blob is re-hashed and compared;
  the `objects` table must hold no row that no commit reaches (a partial save would leave one); the commit count must
  match; and the head save number must be either the last save the writer reported as committed or the one it had
  begun (nothing acknowledged may be lost, nothing unbegun may appear).

The file is deleted and restarted when it passes 24 MB, to keep the check fast.

**Lock test.** A holder process creates `<name>.raycad.lock` with `CREATE_NEW`, writes user, host, pid, start time,
heartbeat time and a counter (fixed 256 bytes, rewritten in place and flushed on every heartbeat), keeps the handle
open, opens the document, later saves once. The test process then acts as second opener. Heartbeat 0.5 s and stale
limit 2 s were used so the test runs in seconds; the plan's values are 30 s and 120 s, same code. Heartbeat age is
computed on the file server's clock: modification time of a probe file just created next to the lock, minus the
modification time of the lock file. No client clock is compared with another client's clock.

**Incremental save.** A 200 MB file is built, reopened, and 20 saves are made that each replace one object with
50,000 new random bytes. Time is from `BEGIN IMMEDIATE` to the return of `COMMIT` (includes hashing).
Bytes written is the process `WriteTransferCount` delta, so it counts journal plus database writes.
Two layouts: 200 objects of 1 MB (manifest 6.4 KB) and 4000 objects of 50 KB (manifest 128 KB).

**Snapshot copy.** A writer saves continuously (blobs 1 to 8 KB) while another process copies the database file with
`CopyFile` and runs the checker on the copy.

## Machine

Core i9-13900K, 64 GB RAM, Windows 11 Home 10.0.26200. Drive C: Samsung SSD 990 PRO 2 TB, NVMe, NTFS
(about 20 GB free). Process not elevated. "Loopback SMB" means the path `\\localhost\C$\...\spikes\s10-sqlite-smb\data`,
the same folder on the same disk reached through the existing administrative share. Windows reports that path as
`DRIVE_REMOTE`, and the "SMB Server Shares(c$) Current Open File Count" counter showed 1 to 2 open files during the run.
The SMB request counters read 0 (possibly because the process is not elevated), so the amount of traffic that really
went through the SMB protocol could not be confirmed. No share was created and no setting was changed.

## Numbers

### 1 and 4. Kill during save

| | Local NTFS | Loopback SMB |
|---|---|---|
| Iterations (kill, then full check) | 500 | 500 |
| Extra early kills on the un-recovered file | 104 | 132 |
| Total `TerminateProcess` calls | 604 | 632 |
| **Check failures** | **0** | **0** |
| Writer exited by itself (any SQLite error) | 0 | 0 |
| `SQLITE_BUSY` retries on reopen | 0 | 0 |
| Saves completed by writers | 2275 | 1731 |
| Kill landed inside a transaction (statements or `COMMIT`) | 468 | 438 |
| of which inside the `COMMIT` call | 192 | 151 |
| **Hot journal present after the kill** | **183** | **164** |
| Extra kills that again left a hot journal | 27 | 36 |
| Kill landed while idle between saves | 18 | 9 |
| Kill landed before the writer finished opening | 14 | 53 |
| Mean check time | 235 ms | 936 ms |
| Wall time | 179 s | 530 s |

"Hot journal present" means the journal file had a valid header with synced page records when the process died.
That is the state in which the database file may already be partly overwritten and must be rolled back.
With `synchronous=FULL` SQLite writes the journal header magic only when it syncs the journal, so a kill earlier in the
transaction shows a zeroed header and needs no rollback.

Raw data: `results/kill_local.csv`, `results/kill_smb_loopback.csv` (one row per iteration), summaries in the `.txt` files.

### 2. Second opener and stale lock (same result on local NTFS and loopback SMB)

- Second opener while the holder lives: `IN_USE: in use by user@HOST since 2026-11-17T19:17:14Z (heartbeat 0.0 s ago, pid 43880)`.
- A live lock cannot be broken: the break request returns `IN_USE`.
- Holder killed: for the next 2.0 s the heartbeat rule still says "in use" (expected with this design; with the plan's
  values that window is up to 120 s). At 1.95 s after the kill: `STALE: stale lock left by user@HOST since ... (last heartbeat 2.0 s ago, limit 2.0 s)`.
- Break, re-acquire, open, check: file intact at the holder's last save; the new owner can save.
- Clean release deletes the lock file; the next opener acquires with no prompt.
- All 11 assertions pass. Output: `results/lock_local.txt`, `results/lock_smb_loopback.txt`.

Facts observed on the way:

- The holder keeps the lock file handle open without delete sharing. A request for delete access then fails while the
  holder lives and succeeds the moment the process is dead. This "owner alive" probe reported "dead" immediately after
  the kill, on both paths, with no wait for the heartbeat limit. Not tested: what a real file server does with that
  handle when the client machine loses power or network (the server may keep it for some time).
- SQLite's own lock, ignoring the lock file: after the holder's first write, a second connection gets
  `SQLITE_BUSY` (rc 5) for both read and write for the rest of the session. Before the holder's first write, the second
  connection can read, and can `BEGIN IMMEDIATE`, but its `COMMIT` fails with `SQLITE_BUSY`. So `locking_mode=EXCLUSIVE`
  only becomes exclusive at the first write.
- In exclusive mode the `-journal` file is not deleted after each save. It stays next to the document with a zeroed
  header for the whole session, is deleted on clean close, and is left behind after a kill.

### 3 and 4. Incremental save, 200 MB file, 50,000 bytes changed, 20 saves each

| Path | Layout | Save time median (min to max) | Bytes written median (min to max) | File growth per save | Open + read head |
|---|---|---|---|---|---|
| Local | 200 x 1 MB | 16.9 ms (14.6 to 33.7) | 131,696 (107,096 to 189,088) | about 59.6 KB | 5.3 ms |
| Local | 4000 x 50 KB | 7.9 ms (7.0 to 42.0) | 258,672 (229,976 to 336,568) | about 182 KB | 81.4 ms |
| Loopback SMB | 200 x 1 MB | 27.0 ms (21.6 to 33.6) | 135,792 (107,096 to 176,792) | about 59.6 KB | 14.2 ms |
| Loopback SMB | 4000 x 50 KB | 9.9 ms (8.7 to 21.9) | 279,176 (254,576 to 336,568) | about 183 KB | 91.9 ms |

- Save cost does not depend on the 200 MB already in the file. Bytes written are 2.6 to 5.6 times the changed bytes:
  the new blob, the new manifest, and the journal copy of each B-tree page touched.
- The flat manifest is rewritten on every save. With 4000 objects it is 128 KB, more than the 50 KB change.
- The "open + read head" times were taken right after the file was written, so the OS file cache was warm.
  Cold-cache open time was not measured.
- All four files passed the full check afterwards (about 200 MB hashed). Output: `results/bench.txt`.

### Snapshot copy for readers (not an acceptance item; a finding)

PLAN.md 4.3 says readers on a network path open "a snapshot copy". Measured with a writer saving while the copy runs:

| Writer | Path | Plain copy of the database file: bad copies | Copy database then journal, let SQLite roll back: bad | Copy guarded by a save counter: bad |
|---|---|---|---|---|
| Saving non-stop | Local | 126 of 137 | 125 of 137 | none completed (every attempt overlapped a save) |
| Saving non-stop | Loopback SMB | 112 of 119 | 117 of 119 | none completed |
| 40 ms pause between saves | Local | 34 of 200 | 29 of 200 | 0 of 184 (16 gave up after 200 attempts) |
| 40 ms pause between saves | Loopback SMB | 58 of 71 | 62 of 71 | 0 of 10 (61 gave up) |

"Bad" means `integrity_check` failed or could not run (`SQLITE_CORRUPT`, rc 11), or a hash or reference was wrong.
This writer saves far more often than a person does, so these rates are a worst case, not an expected rate.
The point is that a copy that overlaps a save is usually corrupt, and copying the journal too does not fix it.
The "guarded" copy uses a 16-byte side file that the writer sets to "saving" before a save and to "idle, counter + 1"
after it; the reader reads it before and after the copy and retries if a save overlapped. All 194 guarded copies that
completed were good. Output: `results/copytest.txt`, `results/copytest_paused_writer.txt`.

## What was not measured

- A real network: no NAS, no Windows file server, no Samba, no second machine, no Wi-Fi, no latency. Loopback SMB is
  not a real network. Opportunistic locks, SMB client caching between two machines, and lock behaviour across machines
  were not exercised.
- Power loss, operating-system crash, cable pull or share disconnect during a save. `TerminateProcess` leaves the OS
  file cache intact, so this test says nothing about whether `synchronous=FULL` is honoured by the disk or the server.
- Clock skew between machines, two machines racing to break the same stale lock, and a holder whose machine dies
  without closing its handles.
- The plan's real heartbeat values (30 s, 120 s); the test used 0.5 s and 2 s.
- Antivirus or indexer interference beyond whatever Windows Defender did by default during the run.
- zstd compression, the `derived` and `thumbs` tables, `compact()` and VACUUM, format migration, the local recovery
  journal of PLAN.md 4.2.
- Files larger than 1 GB. (Guess, not tested: SQLite's lock bytes sit at offset 1 GB, so a plain copy of a file larger
  than that while a writer holds its lock may fail with a lock violation.)
- Cold-cache open and save times; Low-tier hardware; spinning disks.

## Recommendation

1. Keep the container settings of PLAN.md 4.1 (`DELETE` journal, `synchronous=FULL`, `locking_mode=EXCLUSIVE`) and
   save as one transaction. 1,000 checked kills (1,236 process kills in total, 347 with a hot journal) gave zero bad files.
2. Keep the lock file. Add to the design: create with `CREATE_NEW`; keep the handle open without delete sharing so a
   dead owner on the same server is detected at once; measure heartbeat age on the server's clock, not across client
   clocks; break by renaming the stale file away, then creating again.
3. Change PLAN.md 4.3: a reader's snapshot copy must not be a plain file copy. Either have the writer publish a save
   counter (in the lock file) that the reader checks before and after copying, or verify the copy and retry.
   Reading through SQLite is not available to the second opener, because the writer's exclusive lock returns
   `SQLITE_BUSY` for the whole session.
4. Take the SQLite write lock at open (for example an empty `BEGIN EXCLUSIVE; COMMIT`) instead of at first save, so the
   lock file is not the only guard between open and first save. This follows from the observation above; the fix itself
   was not tested.
5. Note in PLAN.md 4.1 that a `<name>.raycad-journal` file sits next to the document for the whole session and after a
   crash. It must never be deleted or separated from the document by the app, by cleanup code or by a sync tool.
6. Before M1 relies on this for shops: rerun `s10 kill`, `s10 locktest`, `s10 bench` and `s10 copytest` against a real
   share. Jake needs to provide a writable folder path on a real NAS or shop file server (ideally one Windows server
   share and one Samba or NAS share), reached from this PC over the network, with about 1 GB free.
   A cable-pull test during save needs a person at the machine.

## How to rerun

    cmd /c "spikes\build.cmd s10-sqlite-smb"
    spikes\s10-sqlite-smb\build\s10.exe kill     <folder>\k.raycad 500 results\kill.csv 200 1 24
    spikes\s10-sqlite-smb\build\s10.exe locktest <folder>\lock.raycad
    spikes\s10-sqlite-smb\build\s10.exe build    <folder>\big.raycad 200 1048576
    spikes\s10-sqlite-smb\build\s10.exe bench    <folder>\big.raycad 50000 20
    spikes\s10-sqlite-smb\build\s10.exe copytest <folder>\c.raycad 200 40

`<folder>` is an absolute path; a UNC path works. Peak test data on disk was about 620 MB. All test data in `data/` was
deleted after the run.
