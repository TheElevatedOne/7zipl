cmake_minimum_required(VERSION 3.16)
project(7zipl VERSION 0.1.0 LANGUAGES C CXX)

# 7zipl: 7-Zip with a native GTK 4 front-end for Linux. The 7-Zip engine (C/, CPP/) is upstream
# 7-Zip unchanged; the GUI lives in gtk/.

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

# ---- 7-Zip engine: the 7zz bundle (CPP/7zip/Bundles/Alone2) without its console front-end ----
include(cmake/sevenzip-sources.cmake)
# An OBJECT library: the codecs and archive handlers register themselves from static constructors that
# nothing references, so a static archive would let the linker drop them.
add_library(sevenzip OBJECT ${SEVENZIP_BACKEND_SOURCES})
target_compile_definitions(sevenzip PUBLIC _REENTRANT _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE
  $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
# Upstream builds with -Wall -Wextra -Werror; warnings from newer compilers must not break the build here.
target_compile_options(sevenzip PRIVATE -w)
set_target_properties(sevenzip PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17)
target_include_directories(sevenzip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sevenzip PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# ---- GUI ----
set(SZL_WARN -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# The glue between the GUI and the engine; the only code that includes 7-Zip's headers.
add_library(szlengine OBJECT gtk/src/engine.cpp)
target_compile_options(szlengine PRIVATE -Wno-unused-parameter)
target_link_libraries(szlengine PUBLIC sevenzip)

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
target_link_libraries(7zipl PRIVATE sevenzip szlengine PkgConfig::GTK4 PkgConfig::GIO_UNIX)

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
install(FILES README.md DOC/License.txt DOC/unRarLicense.txt DESTINATION ${CMAKE_INSTALL_DOCDIR})

enable_testing()
add_test(NAME selftest COMMAND 7zipl --selftest)
set_tests_properties(selftest PROPERTIES SKIP_RETURN_CODE 77)
