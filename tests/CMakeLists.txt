# SPDX-License-Identifier: GPL-3.0-or-later
find_package(GTest REQUIRED)
include(GoogleTest)

set(RL_CLI $<TARGET_FILE:rasterloom-cli>)
set(RL_SMOKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts/smoke")
set(RL_TEST_OUT "${CMAKE_CURRENT_BINARY_DIR}/out")
file(MAKE_DIRECTORY "${RL_TEST_OUT}")

# ---- unit tests (globbed: a lane adds tests/unit/<file>.cpp without editing this file) -----------
file(GLOB_RECURSE RL_UNIT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")
add_executable(rl_unit_tests ${RL_UNIT_SOURCES})
target_link_libraries(rl_unit_tests PRIVATE rasterloomcore GTest::gtest_main)
target_compile_definitions(rl_unit_tests PRIVATE
    RL_SMOKE_DIR="${RL_SMOKE_DIR}"
    RL_TEST_OUT="${RL_TEST_OUT}")
rl_warnings(rl_unit_tests)
gtest_discover_tests(rl_unit_tests DISCOVERY_MODE PRE_TEST)

# ---- core purity: zero Qt symbols, zero Qt includes in librasterloomcore.a -----------------------
add_test(NAME check_core_no_qt
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_core_no_qt.sh"
            $<TARGET_FILE:rasterloomcore> "${PROJECT_SOURCE_DIR}/src/core")

# ---- CLI smoke scripts: render twice, byte-compare (determinism) ---------------------------------
# err_*.json are invalid on purpose: the CLI must exit non-zero and write no PNG.
file(GLOB RL_SMOKE_SCRIPTS CONFIGURE_DEPENDS "${RL_SMOKE_DIR}/*.json")
foreach(script ${RL_SMOKE_SCRIPTS})
    get_filename_component(name "${script}" NAME_WE)
    set(a "${RL_TEST_OUT}/${name}.a.png")
    set(b "${RL_TEST_OUT}/${name}.b.png")
    if(name MATCHES "^err_")
        add_test(NAME cli_rejects_${name}
            COMMAND sh -c "rm -f '${a}'; if '${RL_CLI}' --render-script '${script}' --out '${a}'; then echo 'accepted an invalid script'; exit 1; fi; if [ -e '${a}' ] || [ -e '${a}.tmp' ]; then echo 'PNG written despite error'; exit 1; fi; exit 0")
    else()
        add_test(NAME determinism_${name}
            COMMAND sh -c "rm -f '${a}' '${b}' && '${RL_CLI}' --render-script '${script}' --out '${a}' && '${RL_CLI}' --render-script '${script}' --out '${b}' && cmp '${a}' '${b}'")
    endif()
endforeach()

# ---- tile statistics: sparse by default, dense under mutation 13 ---------------------------------
# sparse_rect.json: 1024x1024 canvas (256 tiles) with one 10x10 layer rect -> 1 allocated tile.
set(sparse "${RL_SMOKE_DIR}/sparse_rect.json")
add_test(NAME cli_stats_sparse
    COMMAND "${RL_CLI}" --render-script "${sparse}" --out "${RL_TEST_OUT}/stats_sparse.png" --stats)
set_tests_properties(cli_stats_sparse PROPERTIES
    PASS_REGULAR_EXPRESSION "stats: pixels 'dot' allocated=1 total=256")
add_test(NAME cli_stats_detects_mutation_13
    COMMAND "${RL_CLI}" --render-script "${sparse}" --out "${RL_TEST_OUT}/stats_dense.png" --stats --mutate=13)
set_tests_properties(cli_stats_detects_mutation_13 PROPERTIES
    PASS_REGULAR_EXPRESSION "stats: pixels 'dot' allocated=256 total=256")
add_test(NAME cli_env_mutation_13
    COMMAND "${CMAKE_COMMAND}" -E env RASTERLOOM_MUTATE=13
            "${RL_CLI}" --render-script "${sparse}" --out "${RL_TEST_OUT}/stats_env.png" --stats)
set_tests_properties(cli_env_mutation_13 PROPERTIES
    PASS_REGULAR_EXPRESSION "stats: pixels 'dot' allocated=256 total=256")
add_test(NAME cli_version COMMAND "${RL_CLI}" --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^rasterloom-cli ${PROJECT_VERSION}")
add_test(NAME cli_rejects_bad_mutation COMMAND "${RL_CLI}" --version --mutate=46)
set_tests_properties(cli_rejects_bad_mutation PROPERTIES WILL_FAIL TRUE)

# ---- GUI stub (offscreen only: never a visible window) --------------------------------------------
if(TARGET rasterloom)
    add_test(NAME gui_version COMMAND rasterloom --version)
    set_tests_properties(gui_version PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        PASS_REGULAR_EXPRESSION "^rasterloom ${PROJECT_VERSION} \\(Qt 6\\.")
    add_test(NAME gui_offscreen_smoke COMMAND rasterloom --smoke)
    set_tests_properties(gui_offscreen_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        PASS_REGULAR_EXPRESSION "platform=offscreen")
endif()

# ---- file I/O: PSD/PSB round-trip corpus (12 files from tests/psd/gen_corpus.py, an independent
# Python PSD writer) + OpenRaster (.orp/.ora) round trips, checked with psd-tools. Needs a Python
# with numpy, Pillow and psd-tools (the project's .venv); the test is skipped (exit 77) without it.
set(RL_PSD_PYTHON_DEFAULT "python3")
foreach(cand "${PROJECT_SOURCE_DIR}/.venv/bin/python" "${PROJECT_SOURCE_DIR}/../../.venv/bin/python")
    if(EXISTS "${cand}" AND RL_PSD_PYTHON_DEFAULT STREQUAL "python3")
        set(RL_PSD_PYTHON_DEFAULT "${cand}")
    endif()
endforeach()
set(RL_PSD_PYTHON "${RL_PSD_PYTHON_DEFAULT}" CACHE FILEPATH "Python with psd-tools for the PSD round-trip test")
add_test(NAME psd_ora_roundtrip_corpus
    COMMAND "${RL_PSD_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/psd/roundtrip.py"
            --cli ${RL_CLI} --work "${CMAKE_CURRENT_BINARY_DIR}/psd_rt"
            --scripts "${CMAKE_CURRENT_SOURCE_DIR}/scripts/compositing")
set_tests_properties(psd_ora_roundtrip_corpus PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# ---- golden corpus, determinism and the mutation gate (tests/tools/*.py) -------------------------
# goldens            every tests/scripts/<domain>/*.json: CLI twice (determinism) + NumPy reference,
#                    decoded-RGBA byte equality, equal_to pairs, tile-stat bounds. Writes
#                    tests/output/report.json, which tools/gen_parity.py validates evidence against.
# goldens_<domain>   the same per domain directory, so a red lane is visible at a glance.
# goldens_determinism CLI only, no reference: two renders of every golden must agree.
# mutation_gate      (label slow) ids 0-45 must each turn >= 1 golden red. PENDING ids (hook not
#                    implemented) fail the test unless RL_MUTATION_ALLOW_PENDING=1 is set.
# Fast local loop: ctest -LE slow. Harness self-test: harness_selftest.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(RL_TOOLS "${CMAKE_CURRENT_SOURCE_DIR}/tools")
    set(RL_GOLDEN_ARGS --cli ${RL_CLI} --python "${Python3_EXECUTABLE}"
        --scripts "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
    add_test(NAME harness_selftest
        COMMAND "${Python3_EXECUTABLE}" "${RL_TOOLS}/selftest_harness.py")
    add_test(NAME goldens
        COMMAND "${Python3_EXECUTABLE}" "${RL_TOOLS}/run_goldens.py" ${RL_GOLDEN_ARGS}
                --out "${CMAKE_CURRENT_SOURCE_DIR}/output")
    add_test(NAME goldens_determinism
        COMMAND "${Python3_EXECUTABLE}" "${RL_TOOLS}/run_goldens.py" ${RL_GOLDEN_ARGS}
                --determinism-only --out "${CMAKE_CURRENT_BINARY_DIR}/goldens-determinism")
    set_tests_properties(harness_selftest PROPERTIES LABELS "tooling" TIMEOUT 600)
    set_tests_properties(goldens goldens_determinism PROPERTIES LABELS "goldens" TIMEOUT 1800)

    file(GLOB RL_GOLDEN_DOMAINS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*")
    foreach(dir ${RL_GOLDEN_DOMAINS})
        get_filename_component(domain "${dir}" NAME)
        if(IS_DIRECTORY "${dir}" AND NOT domain MATCHES "^(smoke|gen)$" AND NOT domain MATCHES "^\\.")
            add_test(NAME goldens_${domain}
                COMMAND "${Python3_EXECUTABLE}" "${RL_TOOLS}/run_goldens.py" ${RL_GOLDEN_ARGS}
                        --single --filter "${domain}/*"
                        --out "${CMAKE_CURRENT_BINARY_DIR}/goldens-${domain}")
            set_tests_properties(goldens_${domain} PROPERTIES LABELS "goldens-domain" TIMEOUT 1800)
        endif()
    endforeach()

    add_test(NAME mutation_gate
        COMMAND sh -c "extra=''; if [ \"\${RL_MUTATION_ALLOW_PENDING:-0}\" = 1 ]; then extra=--allow-pending; echo 'RL_MUTATION_ALLOW_PENDING=1: pending mutation ids tolerated'; fi; exec \"$0\" \"$1\" --cli \"$2\" --python \"$0\" --scripts \"$3\" --out \"$4\" --ids 0-45 $extra"
                "${Python3_EXECUTABLE}" "${RL_TOOLS}/mutation_gate.py" ${RL_CLI}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts" "${CMAKE_CURRENT_BINARY_DIR}/mutation-gate")
    set_tests_properties(mutation_gate PROPERTIES LABELS "slow;goldens" TIMEOUT 3600)
else()
    message(WARNING "Python3 not found: goldens, goldens_determinism and mutation_gate are not registered")
endif()

# ---- infra lane: embedded self-test, mutation loop, memory tiers, scheduler (tests/unit/infra) ---
include("${CMAKE_CURRENT_SOURCE_DIR}/unit/infra/infra.cmake")

# ---- GUI lane: gui-smoke (offscreen, contact sheet) + GUI CLI flags ----------------------------------
if(TARGET rasterloomgui)
    add_subdirectory(gui)
endif()
