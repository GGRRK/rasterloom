# SPDX-License-Identifier: GPL-3.0-or-later
#
# Correctness/perf infrastructure checks (included from tests/CMakeLists.txt). The unit tests in
# this directory are compiled into rl_unit_tests by the tests/unit glob.
#
# selftest                  rasterloom-cli --selftest: every frozen golden in-process (fast)
# selftest_deterministic    the same with --deterministic
# selftest_memtest          the same with RASTERLOOM_MEM_TEST=1 (every op boundary compresses AND
#                           spills every tile, so all reads fault back in from the scratch file)
# gui_selftest_embedded     the GUI binary's --selftest (same entry point)
# goldens_frozen_in_sync    tests/goldens matches tests/scripts (tools/freeze_goldens.py --check)
# goldens_memtest           run_goldens (CLI vs NumPy reference) with RASTERLOOM_MEM_TEST=1
# cli_memory_hard_limit     an allocation beyond the hard limit is a clean error (exit 5, no PNG)
# cli_deterministic_flag    --deterministic renders byte-identically
# selftest_mutation_loop    (slow) BUILD-SPEC's loop: --selftest --selftest-mutate=N must FAIL for
#                           every mutation id the binary knows (0..kCount-1)

target_link_libraries(rl_unit_tests PRIVATE rasterloomselftest)

set(RL_INFRA_OUT "${CMAKE_CURRENT_BINARY_DIR}/infra")
file(MAKE_DIRECTORY "${RL_INFRA_OUT}")

add_test(NAME selftest COMMAND rasterloom-cli --selftest --selftest-quiet
    "--junit-xml=${RL_INFRA_OUT}/selftest.xml")
set_tests_properties(selftest PROPERTIES LABELS "selftest"
    PASS_REGULAR_EXPRESSION "selftest: [0-9]+ passed, 0 failed")
add_test(NAME selftest_deterministic COMMAND rasterloom-cli --selftest --selftest-quiet --deterministic)
set_tests_properties(selftest_deterministic PROPERTIES LABELS "selftest"
    PASS_REGULAR_EXPRESSION "scheduler=deterministic \\(--deterministic\\).*selftest: [0-9]+ passed, 0 failed")
add_test(NAME selftest_memtest COMMAND rasterloom-cli --selftest --selftest-quiet)
set_tests_properties(selftest_memtest PROPERTIES LABELS "selftest"
    ENVIRONMENT "RASTERLOOM_MEM_TEST=1;RASTERLOOM_SCRATCH_DIR=${RL_INFRA_OUT}/scratch"
    PASS_REGULAR_EXPRESSION "compressed [1-9][0-9]*, spilled [1-9][0-9]*, faults [1-9][0-9]*.*selftest: [0-9]+ passed, 0 failed")
if(TARGET rasterloom)
    add_test(NAME gui_selftest_embedded COMMAND rasterloom --selftest --selftest-quiet
        "--junit-xml=${RL_INFRA_OUT}/gui-selftest.xml")
    set_tests_properties(gui_selftest_embedded PROPERTIES LABELS "selftest"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        PASS_REGULAR_EXPRESSION "selftest: [0-9]+ passed, 0 failed")
    set(RL_SELFTEST_BIN $<TARGET_FILE:rasterloom>)
else()
    set(RL_SELFTEST_BIN $<TARGET_FILE:rasterloom-cli>)
endif()

# BUILD-SPEC <verification> mutation gate, verbatim loop shape; the id range comes from the binary.
add_test(NAME selftest_mutation_loop
    COMMAND sh -c "n=$(\"$1\" --list-mutations | wc -l); [ \"$n\" -ge 40 ] || { echo \"only $n mutation ids\"; exit 1; }; \
caught=0; for m in $(seq 0 $((n - 1))); do \
  if \"$0\" --selftest --selftest-quiet --selftest-mutate=$m > /dev/null 2>&1; then \
    echo \"MUTATION $m UNDETECTED - SUITE IS VACUOUS\"; exit 1; fi; caught=$((caught + 1)); done; \
echo \"selftest_mutation_loop: all $caught mutation ids 0..$((n - 1)) detected\""
        ${RL_SELFTEST_BIN} $<TARGET_FILE:rasterloom-cli>)
set_tests_properties(selftest_mutation_loop PROPERTIES LABELS "slow;selftest" TIMEOUT 3600
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    PASS_REGULAR_EXPRESSION "all [0-9]+ mutation ids 0\\.\\.[0-9]+ detected")

if(Python3_Interpreter_FOUND)
    add_test(NAME goldens_frozen_in_sync
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/freeze_goldens.py" --check
                --scripts "${PROJECT_SOURCE_DIR}/tests/scripts" --out "${PROJECT_SOURCE_DIR}/tests/goldens")
    set_tests_properties(goldens_frozen_in_sync PROPERTIES LABELS "selftest;tooling")
    add_test(NAME goldens_memtest
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_goldens.py"
                --cli $<TARGET_FILE:rasterloom-cli> --python "${Python3_EXECUTABLE}"
                --scripts "${CMAKE_CURRENT_SOURCE_DIR}/scripts" --single
                --out "${RL_INFRA_OUT}/goldens-memtest")
    set_tests_properties(goldens_memtest PROPERTIES LABELS "goldens" TIMEOUT 1800
        ENVIRONMENT "RASTERLOOM_MEM_TEST=1;RASTERLOOM_SCRATCH_DIR=${RL_INFRA_OUT}/scratch")
endif()

# 2048x2048 filled layer = 1024 tiles = 16 MiB > the 8 MiB hard limit: exit 5, message, no PNG.
set(RL_BIG_SCRIPT "${RL_INFRA_OUT}/big_fill.json")
file(WRITE "${RL_BIG_SCRIPT}" "{\"canvas\":{\"w\":2048,\"h\":2048},\"ops\":[{\"op\":\"add_layer\",\"id\":\"a\",\"fill\":\"solid\",\"color\":\"#336699ff\"}],\"out\":\"png8\"}\n")
add_test(NAME cli_memory_hard_limit
    COMMAND sh -c "rm -f \"$1\"; RASTERLOOM_MEM_HARD_MB=8 \"$0\" --render-script \"$2\" --out \"$1\"; rc=$?; \
[ $rc -eq 5 ] || { echo \"exit $rc, want 5\"; exit 1; }; [ ! -e \"$1\" ] || { echo 'PNG written'; exit 1; }; echo 'clean out-of-memory error'"
        $<TARGET_FILE:rasterloom-cli> "${RL_INFRA_OUT}/big_fill.png" "${RL_BIG_SCRIPT}")
set_tests_properties(cli_memory_hard_limit PROPERTIES LABELS "memory"
    PASS_REGULAR_EXPRESSION "out of memory: .*hard limit 8 MiB.*clean out-of-memory error")

add_test(NAME cli_deterministic_flag
    COMMAND sh -c "\"$0\" --render-script \"$1\" --out \"$2.a.png\" && \"$0\" --deterministic --render-script \"$1\" --out \"$2.b.png\" && cmp \"$2.a.png\" \"$2.b.png\""
        $<TARGET_FILE:rasterloom-cli> "${CMAKE_CURRENT_SOURCE_DIR}/scripts/smoke/groups_clip_masks.json" "${RL_INFRA_OUT}/det")
set_tests_properties(cli_deterministic_flag PROPERTIES LABELS "selftest")
