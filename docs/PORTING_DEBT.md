# Porting debt

Windows is the only gating platform until the app is finished. macOS and Linux findings noted in passing, one line each,
to be fixed afterwards.

- Linux (WSL, GCC Release): the naming blocks `[random-directedit-holdout2]` and `[random]` (with placements) and
  `[random-directedit-review1]` (without) segfaulted when run 4 at a time (8 workers each); each passed alone. No core
  or backtrace was captured (T-081b round 7).
- Linux: the replay case keyway_tslot_1 #0 vertex 0 ("joined blend pieces") is Broken (a miss) since the round-8
  post-condition; Exact on Windows. Accepted as a miss off Windows (`Case::exactOn`, random_directedit_test.cpp).
- macOS: `[random]` 2:2 face 14 is judged WRONG ghost on macOS for an answer the reviewer ruled right (Exact {20}); the
  T-081a test now prints its scripts (RAYCAD_DUMP_SCRIPTS) so the macOS sequence can be pinned.
- macOS: holdout3 4:7 edge 26 (Exact {22} on macOS, ruled wrong) is pinned only as a Windows analogue
  (directedit_test.cpp); not verified on macOS.
