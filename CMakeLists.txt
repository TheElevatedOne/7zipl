cmake_minimum_required(VERSION 3.16)
project(7zipl VERSION 0.1.0 LANGUAGES C CXX)

# 7zipl: a native GTK 4 front-end for 7-Zip on Linux. It runs the system's 7-Zip (7zz or 7z from the
# distribution's 7zip package); the GUI lives in gtk/.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.14)

set(SZL_APP_ID "org.sevenzipl.SevenZipL")

# ---- GUI ----
set(SZL_WARN -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# The app icon, also built into the binary (window icon before installing, and the per-user desktop entry).
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(SZL_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/resources.c)
add_custom_command(
  OUTPUT ${SZL_RESOURCES}
  COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data/icons
          --target=${SZL_RESOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/data/7zipl.gresource.xml
  DEPENDS data/7zipl.gresource.xml data/icons/${SZL_APP_ID}.svg)

add_executable(7zipl
  ${SZL_RESOURCES}
  gtk/src/main.cpp
  gtk/src/engine.cpp
  gtk/src/app.cpp
  gtk/src/gtkutil.cpp
  gtk/src/style.cpp
  gtk/src/settings.cpp
  gtk/src/util.cpp
  gtk/src/job.cpp
  gtk/src/panel.cpp
  gtk/src/mainwindow.cpp
  gtk/src/commands.cpp
  gtk/src/compressdialog.cpp
  gtk/src/extractdialog.cpp
  gtk/src/dialogs.cpp
  gtk/src/integration.cpp
  gtk/src/optionsdialog.cpp
  gtk/src/selftest.cpp
)
target_include_directories(7zipl PRIVATE gtk/src)
target_compile_definitions(7zipl PRIVATE
  SZL_VERSION="${PROJECT_VERSION}"
  SZL_APP_ID="${SZL_APP_ID}"
  SZL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  GDK_DISABLE_DEPRECATION_WARNINGS)
target_compile_options(7zipl PRIVATE ${SZL_WARN})
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
target_link_libraries(7zipl PRIVATE Threads::Threads PkgConfig::GTK4 PkgConfig::GIO_UNIX)

install(TARGETS 7zipl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Desktop integration
configure_file(data/7zipl.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/${SZL_APP_ID}.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${SZL_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${SZL_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/icons/${SZL_APP_ID}.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
# Refresh the desktop and icon caches on a plain "cmake --install" (packages do this with their own hooks).
install(CODE [[
  if(NOT DEFINED ENV{DESTDIR} OR "$ENV{DESTDIR}" STREQUAL "")
    find_program(UDD update-desktop-database)
    if(UDD)
      execute_process(COMMAND ${UDD} -q "${CMAKE_INSTALL_PREFIX}/share/applications")
    endif()
    find_program(GUIC gtk-update-icon-cache)
    if(GUIC)
      execute_process(COMMAND ${GUIC} -q -t -f "${CMAKE_INSTALL_PREFIX}/share/icons/hicolor")
    endif()
  endif()
]])
install(FILES README.md COPYING DESTINATION ${CMAKE_INSTALL_DOCDIR})

enable_testing()
add_test(NAME selftest COMMAND 7zipl --selftest)
set_tests_properties(selftest PROPERTIES SKIP_RETURN_CODE 77)
