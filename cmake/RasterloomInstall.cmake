# SPDX-License-Identifier: GPL-3.0-or-later
#
# install() rules (packaging lane). Everything Rasterloom itself ships is in the install component
# "rasterloom", so the AppImage build installs exactly that set:
#
#   cmake --install build --prefix AppDir/usr --component rasterloom
#
# A plain `cmake --install build` additionally runs the Qt-Advanced-Docking-System's own rules
# (its headers and CMake package files, component "Unspecified"); packagers who want only the
# application use the component form above too.
#
# Layout (relative to the prefix; the GUI looks for its release-hygiene files at
# <dir of the rasterloom binary>/../share/doc/rasterloom, see src/gui/app_info.cpp find_doc_file,
# and for its self-test scripts at <dir of the binary>/../share/rasterloom/selftest):
#   bin/rasterloom, bin/rasterloom-cli
#   lib/libqtadvanceddocking-qt6.so.*            (GUI builds; shared, LGPL-2.1-or-later)
#   share/applications/rasterloom.desktop
#   share/metainfo/io.github.ggrrk.rasterloom.metainfo.xml
#   share/icons/hicolor/{scalable,16x16..512x512}/apps/rasterloom.{svg,png}
#   share/rasterloom/selftest/*.json               (tests/scripts/smoke, for `rasterloom --selftest`)
#   share/doc/rasterloom/{LICENSE,COPYING.LESSER,NOTICE,AUTHORS}
#   share/doc/rasterloom/{THIRD-PARTY-NOTICES.md,WRITTEN-OFFER.txt}
#       These two describe the libraries a *bundle* ships, so they are generated per bundle by
#       packaging/build-appimage.sh after linuxdeploy has populated the AppDir. For another
#       package format, generate them and pass -DRL_RELEASE_DOCS_DIR=<dir containing both>; the
#       install then fails loudly if either is missing.
#
# The doc directory is spelled out ("share/doc/rasterloom") rather than taken from
# CMAKE_INSTALL_DOCDIR, whose default is share/doc/${PROJECT_NAME} = share/doc/Rasterloom, which
# is not where the GUI looks.

include(GNUInstallDirs)

set(RL_INSTALL_COMPONENT rasterloom)
set(RL_INSTALL_DOCDIR "${CMAKE_INSTALL_DATADIR}/doc/rasterloom")
set(RL_RELEASE_DOCS_DIR "" CACHE PATH
    "Directory holding generated THIRD-PARTY-NOTICES.md and WRITTEN-OFFER.txt to install (optional)")

install(TARGETS rasterloom-cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${RL_INSTALL_COMPONENT})

if(TARGET rasterloom)
    # Find the shared ADS library next to the binary after installation (linuxdeploy rewrites
    # this to its own $ORIGIN/../lib anyway; a plain prefix install needs it too).
    set_target_properties(rasterloom PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    install(TARGETS rasterloom
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${RL_INSTALL_COMPONENT})
    if(TARGET qtadvanceddocking-qt6)
        install(TARGETS qtadvanceddocking-qt6
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${RL_INSTALL_COMPONENT}
            NAMELINK_SKIP)
    endif()
endif()

set(RL_SHARE "${PROJECT_SOURCE_DIR}/share")
install(FILES "${RL_SHARE}/rasterloom.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT ${RL_INSTALL_COMPONENT})
install(FILES "${RL_SHARE}/io.github.ggrrk.rasterloom.metainfo.xml"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo" COMPONENT ${RL_INSTALL_COMPONENT})
install(FILES "${RL_SHARE}/icons/rasterloom.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" COMPONENT ${RL_INSTALL_COMPONENT})
foreach(size 16 24 32 48 64 128 256 512)
    install(FILES "${RL_SHARE}/icons/hicolor/${size}x${size}/apps/rasterloom.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
        COMPONENT ${RL_INSTALL_COMPONENT})
endforeach()

install(DIRECTORY "${PROJECT_SOURCE_DIR}/tests/scripts/smoke/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rasterloom/selftest" COMPONENT ${RL_INSTALL_COMPONENT}
    FILES_MATCHING PATTERN "*.json")

install(FILES
        "${PROJECT_SOURCE_DIR}/LICENSE"
        "${PROJECT_SOURCE_DIR}/COPYING.LESSER"
        "${PROJECT_SOURCE_DIR}/NOTICE"
        "${PROJECT_SOURCE_DIR}/AUTHORS"
    DESTINATION "${RL_INSTALL_DOCDIR}" COMPONENT ${RL_INSTALL_COMPONENT})

if(RL_RELEASE_DOCS_DIR)
    foreach(f THIRD-PARTY-NOTICES.md WRITTEN-OFFER.txt)
        if(NOT EXISTS "${RL_RELEASE_DOCS_DIR}/${f}")
            message(FATAL_ERROR "RL_RELEASE_DOCS_DIR=${RL_RELEASE_DOCS_DIR} has no ${f}")
        endif()
    endforeach()
    install(FILES "${RL_RELEASE_DOCS_DIR}/THIRD-PARTY-NOTICES.md" "${RL_RELEASE_DOCS_DIR}/WRITTEN-OFFER.txt"
        DESTINATION "${RL_INSTALL_DOCDIR}" COMPONENT ${RL_INSTALL_COMPONENT})
endif()
