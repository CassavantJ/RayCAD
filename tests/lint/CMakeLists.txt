# The lint tools run on the source tree, and each proves with a self-test that it catches a planted violation.

add_test(NAME lint_includes COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_includes.py")
add_test(NAME lint_includes_selftest COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_includes.py" --self-test)

add_test(NAME lint_cmake_deps COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_cmake_deps.py")
add_test(NAME lint_cmake_deps_selftest COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_cmake_deps.py" --self-test)

add_test(NAME lint_dmath COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_dmath.py")
add_test(NAME lint_dmath_selftest COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_dmath.py" --self-test)

add_test(NAME lint_colours COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_colours.py")
add_test(NAME lint_colours_selftest COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_colours.py" --self-test)

add_test(NAME lint_licences COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_licences.py")
add_test(NAME lint_licences_selftest COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_licences.py" --self-test)

# The licence ledger is also checked against what vcpkg really installed for this build (transitive ports).
add_test(NAME lint_licences_installed
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/lint_licences.py" --installed "${VCPKG_INSTALLED_DIR}")

# The committed part of the test corpus matches its manifest (no network), and the corpus and golden tools work.
add_test(NAME corpus_check COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/corpus_check.py")
add_test(NAME corpus_check_selftest COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/corpus_check.py" --self-test)
add_test(NAME corpus_fetch_selftest COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/corpus_fetch.py" --self-test)
add_test(NAME update_golden_selftest COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/update_golden.py" --self-test)
