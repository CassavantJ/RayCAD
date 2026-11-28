# Known flakes

Failures seen once that did not repeat when the test was rerun alone. The development PC's CPU (an i9-13900K on a 2022
BIOS) is known to misexecute under sustained load, so each entry records the load it ran under. Naming blocks run at
most 2 at a time since 2026-11-27.

| Date | Platform | Test | Parallelism | What happened | Rerun alone |
|---|---|---|---|---|---|
| 2026-11-27 | Windows, MSVC Release | `[random-directedit-holdout1]` (with placements) | 4 naming blocks at once, 8 workers each | exit 0xC0000374 (heap corruption) at start-up, before any sequence ran | passed, WRONG 0 |
| 2026-11-26 | Windows, MSVC Release | `[random-directedit-holdout2]` (with placements) | 4 naming blocks at once | exit 0xC0000374 once, on an earlier build of the round | passed in later runs |
