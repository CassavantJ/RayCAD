# The two executables start and exit cleanly.
add_test(NAME raycad_cli_version COMMAND raycad-cli --version)
set_tests_properties(raycad_cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^raycad-cli ${PROJECT_VERSION}")

# Opens the Qt Quick window, waits for the first frame, exits by itself.
add_test(NAME raycad_smoke COMMAND raycad --smoke)
set_tests_properties(raycad_smoke PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "raycad: smoke ok")
ray_test_runtime_path(raycad_smoke QT)
ray_test_runtime_path(raycad_cli_version)
