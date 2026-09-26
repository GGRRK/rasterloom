# SPDX-License-Identifier: GPL-3.0-or-later
#
# gui-smoke: drives the real MainWindow (ADS docking, raster canvas) through ~30 scripted
# interactions under QT_QPA_PLATFORM=offscreen, grabs the window after each and tiles the grabs into
# tests/output/gui-contact-sheet.png for human eyes (docs/research/qt-platform.md section 6).
find_package(Qt6 6.9 REQUIRED COMPONENTS Test)

add_executable(gui_smoke "${CMAKE_CURRENT_SOURCE_DIR}/gui_smoke.cpp")
set_target_properties(gui_smoke PROPERTIES AUTOMOC ON)
target_link_libraries(gui_smoke PRIVATE rasterloomgui Qt6::Test)
target_compile_definitions(gui_smoke PRIVATE
    RL_GUI_OUT="${PROJECT_SOURCE_DIR}/tests/output"
    RL_SMOKE_SCRIPTS="${PROJECT_SOURCE_DIR}/tests/scripts/smoke"
    RL_PSD_CORPUS_GEN="${PROJECT_SOURCE_DIR}/tests/psd/gen_corpus.py")
rl_warnings(gui_smoke)

add_test(NAME gui-smoke COMMAND gui_smoke)
set_tests_properties(gui-smoke PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;RASTERLOOM_CANVAS=raster;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config"
    ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:;RASTERLOOM_PLATFORM=unset:")

# Session / op runner / state diff / stroke adapter (no widgets).
add_executable(gui_session_test "${CMAKE_CURRENT_SOURCE_DIR}/gui_session_test.cpp")
set_target_properties(gui_session_test PROPERTIES AUTOMOC ON)
target_link_libraries(gui_session_test PRIVATE rasterloomgui Qt6::Test)
rl_warnings(gui_session_test)
add_test(NAME gui-session COMMAND gui_session_test)
set_tests_properties(gui-session PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# GL canvas vs raster canvas. Keeps DISPLAY (offscreen GLX draws into an unmapped window, nothing is
# shown); skips itself when no GL 3.3 core context exists.
add_executable(gui_gl_canvas "${CMAKE_CURRENT_SOURCE_DIR}/gui_gl_canvas.cpp")
set_target_properties(gui_gl_canvas PROPERTIES AUTOMOC ON)
target_link_libraries(gui_gl_canvas PRIVATE rasterloomgui Qt6::Test)
rl_warnings(gui_gl_canvas)
add_test(NAME gui-gl-canvas COMMAND gui_gl_canvas)
set_tests_properties(gui-gl-canvas PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config"
    ENVIRONMENT_MODIFICATION "WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:"
    SKIP_REGULAR_EXPRESSION "SKIP   :")

# The canvas as MainWindow hosts it (regression for 5c8e9df: ADS's QScrollArea turned
# autoFillBackground on and the GL overlay painter cleared every frame). gui-canvas-hosting is
# structural and needs no display or GL. gui-gl-widget grabs CanvasGLWidget (paintGL + overlay
# painter) through offscreen GLX like gui-gl-canvas (DISPLAY kept, nothing mapped) and skips without
# GL 3.3 core. gui-gl-widget-xcb is the same binary on the xcb platform, which opens a real window:
# it runs only with RL_TEST_XCB=1 (CI's Xvfb step, headless gamescope) and skips otherwise.
add_executable(gui_gl_widget "${CMAKE_CURRENT_SOURCE_DIR}/gui_gl_widget.cpp")
set_target_properties(gui_gl_widget PROPERTIES AUTOMOC ON)
target_link_libraries(gui_gl_widget PRIVATE rasterloomgui Qt6::Test)
rl_warnings(gui_gl_widget)
set(_rl_gl_widget_env_mod "WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:;RASTERLOOM_CANVAS=unset:;RASTERLOOM_PLATFORM=unset:")
add_test(NAME gui-canvas-hosting COMMAND gui_gl_widget canvas_hosting late_gl_failure_hosting)
set_tests_properties(gui-canvas-hosting PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-canvas-hosting"
    ENVIRONMENT_MODIFICATION "DISPLAY=unset:;${_rl_gl_widget_env_mod}")
add_test(NAME gui-gl-widget COMMAND gui_gl_widget gl_widget_frame)
set_tests_properties(gui-gl-widget PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-gl-widget"
    ENVIRONMENT_MODIFICATION "${_rl_gl_widget_env_mod}"
    SKIP_REGULAR_EXPRESSION "SKIP   :")
add_test(NAME gui-gl-widget-xcb COMMAND gui_gl_widget)
set_tests_properties(gui-gl-widget-xcb PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=xcb;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-gl-widget-xcb"
    ENVIRONMENT_MODIFICATION "${_rl_gl_widget_env_mod}"
    SKIP_REGULAR_EXPRESSION "SKIP   :")

# The GUI binary's CLI flags (no display needed).
add_test(NAME gui-version-verbose COMMAND rasterloom --version --verbose)
set_tests_properties(gui-version-verbose PROPERTIES
    PASS_REGULAR_EXPRESSION "glibc +[0-9].*Qt runtime +6\\..*Qt-ADS +5\\.1\\.1.*libpng.*zlib.*liblz4 +[0-9]+\\.[0-9]+\\.[0-9]+.*libjpeg.*libtiff +[0-9]")
# Release-hygiene files: RASTERLOOM_DOC_DIR (what CTest sets for any build dir) ...
add_test(NAME gui-license COMMAND rasterloom --license)
set_tests_properties(gui-license PROPERTIES PASS_REGULAR_EXPRESSION "GPL-3.0-or-later.*---- LICENSE ----.*GNU GENERAL PUBLIC LICENSE"
    ENVIRONMENT "RASTERLOOM_DOC_DIR=${PROJECT_SOURCE_DIR}")
# ... the checkout found upward from /proc/self/exe (no variable, no path compiled in) ...
add_test(NAME gui-license-dev-tree COMMAND rasterloom --license)
set_tests_properties(gui-license-dev-tree PROPERTIES PASS_REGULAR_EXPRESSION "---- LICENSE ----.*GNU GENERAL PUBLIC LICENSE"
    ENVIRONMENT_MODIFICATION "RASTERLOOM_DOC_DIR=unset:")
if(NOT PROJECT_BINARY_DIR MATCHES "^${PROJECT_SOURCE_DIR}/")
    set_tests_properties(gui-license-dev-tree PROPERTIES DISABLED TRUE)  # build tree outside the checkout
endif()
# ... and <bindir>/../share/doc/rasterloom of an installed layout (a copy of the binary in a fake
# prefix; LD_LIBRARY_PATH stands in for the install RUNPATH to the ADS library).
add_test(NAME gui-license-installed-layout COMMAND sh -c
    "set -e; p=\"$1\"; rm -rf \"$p\"; mkdir -p \"$p/bin\" \"$p/share/doc/rasterloom\"; cp \"$2\" \"$p/bin/\"; printf 'PREFIX-LICENSE-MARKER\\n' > \"$p/share/doc/rasterloom/LICENSE\"; printf 'PREFIX-NOTICES-MARKER\\n' > \"$p/share/doc/rasterloom/THIRD-PARTY-NOTICES.md\"; LD_LIBRARY_PATH=\"$3\" \"$p/bin/rasterloom\" --license; LD_LIBRARY_PATH=\"$3\" \"$p/bin/rasterloom\" --third-party-notices"
    sh "${CMAKE_CURRENT_BINARY_DIR}/fake-prefix" "$<TARGET_FILE:rasterloom>" "$<TARGET_FILE_DIR:ads::qtadvanceddocking-qt6>")
set_tests_properties(gui-license-installed-layout PROPERTIES
    PASS_REGULAR_EXPRESSION "---- LICENSE ----.PREFIX-LICENSE-MARKER.*liblz4 [0-9].*---- THIRD-PARTY-NOTICES.md ----.PREFIX-NOTICES-MARKER"
    ENVIRONMENT_MODIFICATION "RASTERLOOM_DOC_DIR=unset:")
add_test(NAME gui-third-party-notices COMMAND rasterloom --third-party-notices)
set_tests_properties(gui-third-party-notices PROPERTIES PASS_REGULAR_EXPRESSION "Qt-Advanced-Docking-System.*liblz4 [0-9]+\\.[0-9]+.*BSD-2-Clause.*libjpeg-turbo.*libtiff")
add_test(NAME gui-selftest COMMAND rasterloom --selftest "--selftest-dir=${PROJECT_SOURCE_DIR}/tests/scripts/smoke"
    "--junit-xml=${CMAKE_CURRENT_BINARY_DIR}/selftest.xml")
set_tests_properties(gui-selftest PROPERTIES PASS_REGULAR_EXPRESSION "selftest: [0-9]+ passed, 0 failed")

# Live strokes through rl::brush::StrokeSession: synthetic QTabletEvent strokes; the preview before
# pointer-up == the committed layer == a CLI replay of the recorded op (byte for byte).
add_executable(gui_stroke_live "${CMAKE_CURRENT_SOURCE_DIR}/gui_stroke_live.cpp")
set_target_properties(gui_stroke_live PROPERTIES AUTOMOC ON)
target_link_libraries(gui_stroke_live PRIVATE rasterloomgui Qt6::Test)
target_compile_definitions(gui_stroke_live PRIVATE RL_CLI="$<TARGET_FILE:rasterloom-cli>")
rl_warnings(gui_stroke_live)
add_test(NAME gui-stroke-live COMMAND gui_stroke_live)
set_tests_properties(gui-stroke-live PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;RASTERLOOM_CANVAS=raster"
    ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:")

# File I/O through the GUI: save/export/reopen, atomic failed save, import warnings dialog (PSD
# corpus generated by tests/psd/gen_corpus.py), > 16384 px PSB, drag-and-drop, recent files.
add_executable(gui_file_io "${CMAKE_CURRENT_SOURCE_DIR}/gui_file_io.cpp")
set_target_properties(gui_file_io PROPERTIES AUTOMOC ON)
target_link_libraries(gui_file_io PRIVATE rasterloomgui Qt6::Test)
target_compile_definitions(gui_file_io PRIVATE RL_PSD_CORPUS_GEN="${PROJECT_SOURCE_DIR}/tests/psd/gen_corpus.py")
rl_warnings(gui_file_io)
add_test(NAME gui-file-io COMMAND gui_file_io)
set_tests_properties(gui-file-io PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;RASTERLOOM_CANVAS=raster"
    ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:")

# Getting images in (docs/math/60 §14 place_image / layer_via_copy / clear): drag-and-drop, the
# system clipboard, Clear, Layer via Copy/Cut, and GIF/BMP/WebP through Qt's image plugins. One ctest
# per PARITY row, each running one QtTest function on a fresh window.
add_executable(gui_import "${CMAKE_CURRENT_SOURCE_DIR}/gui_import.cpp")
set_target_properties(gui_import PROPERTIES AUTOMOC ON)
target_link_libraries(gui_import PRIVATE rasterloomgui Qt6::Test)
target_compile_definitions(gui_import PRIVATE RL_CLI="$<TARGET_FILE:rasterloom-cli>"
                           RL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
rl_warnings(gui_import)
foreach(pair "drop-places-layer:drop_places_layer" "drop-opens-when-no-document:drop_opens_when_no_document"
             "drop-multiple-files:drop_multiple_files" "drop-image-data:drop_browser_image"
             "drop-unsupported-message:drop_unsupported_message" "copy-paste-clipboard:copy_paste_clipboard"
             "paste-from-other-application:paste_from_other_application"
             "paste-in-place-copy-merged:paste_in_place_and_copy_merged" "paste-new-document:paste_new_document"
             "cut-and-clear:cut_and_clear" "layer-via-copy-cut:layer_via_copy_and_cut" "open-gif-bmp-webp:open_gif_bmp_webp"
             "export-webp-bmp:export_webp_bmp")
    string(REPLACE ":" ";" pair "${pair}")
    list(GET pair 0 name)
    list(GET pair 1 fn)
    add_test(NAME gui-${name} COMMAND gui_import ${fn})
    set_tests_properties(gui-${name} PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;RASTERLOOM_CANVAS=raster;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-${name}"
        ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:")
endforeach()

# Layer editing through the doc-60 ops (rename, set_adjustment, duplicate, group mode, select_alpha, delete).
add_executable(gui_editing "${CMAKE_CURRENT_SOURCE_DIR}/gui_editing.cpp")
set_target_properties(gui_editing PROPERTIES AUTOMOC ON)
target_link_libraries(gui_editing PRIVATE rasterloomgui Qt6::Test)
rl_warnings(gui_editing)
add_test(NAME gui-editing COMMAND gui_editing)
set_tests_properties(gui-editing PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;RASTERLOOM_CANVAS=raster;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-editing"
    ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:")

# Free Transform: params + quad forms (distort / skew / perspective), live preview == commit == CLI replay.
add_executable(gui_transform "${CMAKE_CURRENT_SOURCE_DIR}/gui_transform.cpp")
set_target_properties(gui_transform PROPERTIES AUTOMOC ON)
target_link_libraries(gui_transform PRIVATE rasterloomgui Qt6::Test)
target_compile_definitions(gui_transform PRIVATE RL_CLI="$<TARGET_FILE:rasterloom-cli>")
rl_warnings(gui_transform)
add_test(NAME gui-transform COMMAND gui_transform)
set_tests_properties(gui-transform PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;RASTERLOOM_CANVAS=raster"
    ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:")

# Performance measurements, run by hand (not a ctest): gui_bench brush | gui_bench photo FILE.
add_executable(gui_bench "${CMAKE_CURRENT_SOURCE_DIR}/gui_bench.cpp")
target_link_libraries(gui_bench PRIVATE rasterloomgui Qt6::Test)
rl_warnings(gui_bench)

# Privacy: the shipped binaries embed no build-machine path (no source/build dir from __FILE__,
# assert() or debug info, no absolute RUNPATH); docs are found relative to the executable.
add_test(NAME binaries-no-build-paths COMMAND sh -c
    "rc=0; for b in \"$3\" \"$4\"; do n=$(grep -a -c -F -e \"$1\" -e \"$2\" -e /home/ \"$b\" || true); echo \"$(basename \"$b\"): $n lines with a build path\"; [ \"$n\" = 0 ] || rc=1; done; exit $rc"
    sh "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}" "$<TARGET_FILE:rasterloom>" "$<TARGET_FILE:rasterloom-cli>")

# View / workflow features, one ctest per PARITY row (each runs one QtTest function).
add_executable(gui_navigation "${CMAKE_CURRENT_SOURCE_DIR}/gui_navigation.cpp")
set_target_properties(gui_navigation PROPERTIES AUTOMOC ON)
target_link_libraries(gui_navigation PRIVATE rasterloomgui Qt6::Test)
rl_warnings(gui_navigation)
foreach(pair "hand-tool-pan:hand_tool_pan" "zoom-tool-click-drag:zoom_tool_click_drag" "open-recent:open_recent"
             "repeat-last-filter:repeat_last_filter" "default-and-swap-colors:default_and_swap_colors"
             "move-tool-arrow-nudge:move_tool_arrow_nudge" "brush-options-bar:brush_options_bar")
    string(REPLACE ":" ";" pair "${pair}")
    list(GET pair 0 name)
    list(GET pair 1 fn)
    add_test(NAME gui-${name} COMMAND gui_navigation ${fn})
    set_tests_properties(gui-${name} PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;RASTERLOOM_CANVAS=raster;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-${name}"
        ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:")
endforeach()
