# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: LGPL-3.0-or-later



target_sources(verzeta-studio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources.qrc
)

include(GNUInstallDirs)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/verzeta.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/icons/verzeta-studio.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/verzeta-studio.notifyrc
        DESTINATION ${CMAKE_INSTALL_DATADIR}/knotifications6)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/icons/verzeta-studio.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps)
