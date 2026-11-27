# The naming oracle and scenario tests (task T-081a), promoted from spike S4.
#
# ray_naming_oracle is the oracle alone: it links ray_kernel and ray_core, NOT ray_naming, so it cannot use the
# naming code it judges. The harness (harness.h) and the tests link both.
add_library(ray_naming_oracle STATIC oracle.h oracle.cpp)
target_link_libraries(ray_naming_oracle PUBLIC ray_kernel ray_core)
set_property(TARGET ray_naming_oracle PROPERTY FOLDER "tests")

# The harness and the S4 part, compiled once for the tests here and the document-level S4 tests (tests/unit/doc).
add_library(ray_naming_harness STATIC harness.h harness.cpp s4_part.h s4_part.cpp)
target_link_libraries(ray_naming_harness PUBLIC ray_naming_oracle ray_naming ray_kernel ray_core)
target_include_directories(ray_naming_harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ray_naming_harness PRIVATE RAY_ORACLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
set_property(TARGET ray_naming_harness PROPERTY FOLDER "tests")

add_executable(ray_naming_oracle_tests
    s4_test.cpp
    random_test.cpp
    acceptance_test.cpp
    directedit_test.cpp
    random_directedit_test.cpp)
target_link_libraries(ray_naming_oracle_tests PRIVATE ray_naming_harness ray_naming_oracle ray_naming ray_kernel
    ray_core Catch2::Catch2WithMain)
target_compile_definitions(ray_naming_oracle_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_ORACLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
# shape_testing.h (private to src/kernel, no OCCT header): the count of OCCT faults the kernel guard caught.
target_include_directories(ray_naming_oracle_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel")
set_property(TARGET ray_naming_oracle_tests PROPERTY FOLDER "tests")

# The scripted acceptance scenarios, and the replays that pin the T-081a gate fixes and the oracle's rules, in three
# processes so a parallel ctest runs them side by side (together they take about a minute and a half on one thread).
add_test(NAME ray_naming_oracle_tests COMMAND ray_naming_oracle_tests "[acceptance]~[t081a-gate]~[oracle-gate]")
add_test(NAME ray_naming_t081a_gate COMMAND ray_naming_oracle_tests "[acceptance][t081a-gate]")
add_test(NAME ray_naming_oracle_gate COMMAND ray_naming_oracle_tests "[acceptance][oracle-gate]")
ray_test_runtime_path(ray_naming_oracle_tests)
ray_test_runtime_path(ray_naming_t081a_gate)
ray_test_runtime_path(ray_naming_oracle_gate)
# The 47 S4 scenarios take about half a minute; their own CTest entry with its own timeout.
add_test(NAME ray_naming_s4 COMMAND ray_naming_oracle_tests "[s4]")
ray_test_runtime_path(ray_naming_s4)
set_tests_properties(ray_naming_s4 PROPERTIES TIMEOUT 3600)
# 1,000 judged randomised sequences on 20 corpus parts.
add_test(NAME ray_naming_random COMMAND ray_naming_oracle_tests "[random]")
ray_test_runtime_path(ray_naming_random)
set_tests_properties(ray_naming_random PROPERTIES TIMEOUT 7200)
# The first 3 sequences of every part of that block, for the quick tier (tests/tiers.cmake).
add_test(NAME ray_naming_random_smoke COMMAND ray_naming_oracle_tests "[random-smoke]")
ray_test_runtime_path(ray_naming_random_smoke)
set_tests_properties(ray_naming_random_smoke PROPERTIES TIMEOUT 1800)
# References replayed on the part read back in another session.
add_test(NAME ray_naming_session COMMAND ray_naming_oracle_tests "[random-session]")
ray_test_runtime_path(ray_naming_session)
set_tests_properties(ray_naming_session PROPERTIES TIMEOUT 3600)
# A holdout block of 200 sequences whose seeds were never run during development; reported separately.
add_test(NAME ray_naming_random_holdout COMMAND ray_naming_oracle_tests "[random-holdout]")
ray_test_runtime_path(ray_naming_random_holdout)
set_tests_properties(ray_naming_random_holdout PROPERTIES TIMEOUT 7200)
# A second holdout block whose seeds were first run after the T-081a gate fixes (the first block was looked at).
add_test(NAME ray_naming_random_holdout2 COMMAND ray_naming_oracle_tests "[random-holdout2]")
ray_test_runtime_path(ray_naming_random_holdout2)
set_tests_properties(ray_naming_random_holdout2 PROPERTIES TIMEOUT 7200)
# A third block, first run after the second block's findings were fixed.
add_test(NAME ray_naming_random_holdout3 COMMAND ray_naming_oracle_tests "[random-holdout3]")
ray_test_runtime_path(ray_naming_random_holdout3)
set_tests_properties(ray_naming_random_holdout3 PROPERTIES TIMEOUT 7200)
# A fourth block, first run (once, unseen) after the pick-time evidence and the oracle fixes of that task.
add_test(NAME ray_naming_random_holdout4 COMMAND ray_naming_oracle_tests "[random-holdout4]")
ray_test_runtime_path(ray_naming_random_holdout4)
set_tests_properties(ray_naming_random_holdout4 PROPERTIES TIMEOUT 7200)
# A fifth block, first run (once, unseen) after the fourth block's findings were fixed.
add_test(NAME ray_naming_random_holdout5 COMMAND ray_naming_oracle_tests "[random-holdout5]")
ray_test_runtime_path(ray_naming_random_holdout5)
set_tests_properties(ray_naming_random_holdout5 PROPERTIES TIMEOUT 7200)
# A sixth block, first run (once, unseen) after the fifth block's findings were fixed.
add_test(NAME ray_naming_random_holdout6 COMMAND ray_naming_oracle_tests "[random-holdout6]")
ray_test_runtime_path(ray_naming_random_holdout6)
set_tests_properties(ray_naming_random_holdout6 PROPERTIES TIMEOUT 7200)
# T-081b: references through the T-080 direct edits. The scripted scenarios (quick), the first 2 randomised sequences
# with direct edits of every part (quick), and the block of 1,000 such sequences (slow).
add_test(NAME ray_naming_directedit COMMAND ray_naming_oracle_tests "[directedit]")
ray_test_runtime_path(ray_naming_directedit)
# The scripted scenarios and a few key replays above (quick); every replay that pins a T-081b oracle rule or naming fix,
# each a sequence of a randomised block, here (slow).
add_test(NAME ray_naming_directedit_replays COMMAND ray_naming_oracle_tests "[directedit-replays]")
ray_test_runtime_path(ray_naming_directedit_replays)
set_tests_properties(ray_naming_directedit_replays PROPERTIES TIMEOUT 1200)
add_test(NAME ray_naming_random_directedit_smoke COMMAND ray_naming_oracle_tests "[random-directedit-smoke]")
ray_test_runtime_path(ray_naming_random_directedit_smoke)
set_tests_properties(ray_naming_random_directedit_smoke PROPERTIES TIMEOUT 1800)
add_test(NAME ray_naming_random_directedit COMMAND ray_naming_oracle_tests "[random-directedit]")
ray_test_runtime_path(ray_naming_random_directedit)
set_tests_properties(ray_naming_random_directedit PROPERTIES TIMEOUT 7200)
# Holdout blocks with direct edits (the generator that picks faces by geometry), each first run once, unseen: the block
# on the main block's seeds (first run, unseen, in T-081b round 1), then blocks on new seeds (from round 2).
add_test(NAME ray_naming_random_directedit_sorted COMMAND ray_naming_oracle_tests "[random-directedit-sorted]")
ray_test_runtime_path(ray_naming_random_directedit_sorted)
set_tests_properties(ray_naming_random_directedit_sorted PROPERTIES TIMEOUT 7200)
add_test(NAME ray_naming_random_directedit_holdout1 COMMAND ray_naming_oracle_tests "[random-directedit-holdout1]")
ray_test_runtime_path(ray_naming_random_directedit_holdout1)
set_tests_properties(ray_naming_random_directedit_holdout1 PROPERTIES TIMEOUT 7200)
add_test(NAME ray_naming_random_directedit_holdout2 COMMAND ray_naming_oracle_tests "[random-directedit-holdout2]")
ray_test_runtime_path(ray_naming_random_directedit_holdout2)
set_tests_properties(ray_naming_random_directedit_holdout2 PROPERTIES TIMEOUT 7200)
# T-081b round 4: the independent reviewer's block (seed 0x6B1D3F5A2C800000), kept as a tuned block.
add_test(NAME ray_naming_random_directedit_review1 COMMAND ray_naming_oracle_tests "[random-directedit-review1]")
ray_test_runtime_path(ray_naming_random_directedit_review1)
set_tests_properties(ray_naming_random_directedit_review1 PROPERTIES TIMEOUT 7200)
add_test(NAME ray_naming_random_directedit_review2 COMMAND ray_naming_oracle_tests "[random-directedit-review2]")
ray_test_runtime_path(ray_naming_random_directedit_review2)
set_tests_properties(ray_naming_random_directedit_review2 PROPERTIES TIMEOUT 7200)
add_test(NAME ray_naming_random_directedit_review3 COMMAND ray_naming_oracle_tests "[random-directedit-review3]")
ray_test_runtime_path(ray_naming_random_directedit_review3)
set_tests_properties(ray_naming_random_directedit_review3 PROPERTIES TIMEOUT 7200)
# The fifth block's seeds with every cos and sin of the inputs moved by a few ulp, as another platform's libm can give.
add_test(NAME ray_naming_random_perturbed COMMAND ray_naming_oracle_tests "[random-perturbed]")
ray_test_runtime_path(ray_naming_random_perturbed)
set_tests_properties(ray_naming_random_perturbed PROPERTIES TIMEOUT 7200)

# T-084: the rematch oracle (kernel and core only, like the oracle above: it cannot use the naming code it judges) and
# the revision pairs it judges (rematchImported: a part's STEP replaced by a new revision).
add_library(ray_rematch_oracle STATIC rematch_oracle.h rematch_oracle.cpp)
target_link_libraries(ray_rematch_oracle PUBLIC ray_kernel ray_core)
set_property(TARGET ray_rematch_oracle PROPERTY FOLDER "tests")
add_executable(ray_naming_rematch_tests rematch_pairs_test.cpp)
target_link_libraries(ray_naming_rematch_tests PRIVATE ray_rematch_oracle ray_naming ray_kernel ray_core
    Catch2::Catch2WithMain)
target_compile_definitions(ray_naming_rematch_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
set_property(TARGET ray_naming_rematch_tests PROPERTY FOLDER "tests")
# In three processes of about half a minute each in Release (together about 100 s): the pairs on parts built here, and
# the pairs on NIST parts in two halves.
add_test(NAME ray_naming_rematch_pairs COMMAND ray_naming_rematch_tests "[rematch-pairs]~[nist-a]~[nist-b]")
add_test(NAME ray_naming_rematch_pairs_nist_a COMMAND ray_naming_rematch_tests "[rematch-pairs][nist-a]")
add_test(NAME ray_naming_rematch_pairs_nist_b COMMAND ray_naming_rematch_tests "[rematch-pairs][nist-b]")
foreach(_test IN ITEMS ray_naming_rematch_pairs ray_naming_rematch_pairs_nist_a ray_naming_rematch_pairs_nist_b)
    ray_test_runtime_path(${_test})
endforeach()
