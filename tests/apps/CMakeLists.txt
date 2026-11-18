# The two executables start and exit cleanly.
add_test(NAME raycad_cli_version COMMAND raycad-cli --version)
set_tests_properties(raycad_cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^raycad-cli ${PROJECT_VERSION}")

# Opens the Qt Quick window, waits for the first frame, exits by itself.
add_test(NAME raycad_smoke COMMAND raycad --smoke)
set_tests_properties(raycad_smoke PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "raycad: smoke ok")
ray_test_runtime_path(raycad_smoke QT)
ray_test_runtime_path(raycad_cli_version)

# raycad-cli end to end (T-017): import, save, reopen without the STEP file, export, actions, invoke, errors.
# Each test works in its own folder under the build tree; per-user files go there too (RAYCAD_USER_DATA_DIR).
function(ray_cli_test name)
    add_test(NAME ${name}
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/cli_roundtrip.py"
                     --cli "$<TARGET_FILE:raycad-cli>" --work "${CMAKE_CURRENT_BINARY_DIR}/${name}-$<CONFIG>" ${ARGN})
    set_tests_properties(${name} PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "cli_roundtrip: ok")
    ray_test_runtime_path(${name})
endfunction()
ray_cli_test(raycad_cli_roundtrip_small --step "${CMAKE_SOURCE_DIR}/tests/unit/kernel/data/box_hole.step")
ray_cli_test(raycad_cli_roundtrip_corpus
             --step "${CMAKE_SOURCE_DIR}/tests/corpus/files/nist-pmi/nist_ctc_04_asme1_rd_geometry.stp")
ray_cli_test(raycad_cli_errors --errors)
# Crash recovery: the api tests' helper is killed mid-session, then raycad-cli recover lists, replays or discards.
ray_cli_test(raycad_cli_recovery --recovery --helper "$<TARGET_FILE:ray_api_helper>"
             --step "${CMAKE_SOURCE_DIR}/tests/unit/kernel/data/box_hole.step")
