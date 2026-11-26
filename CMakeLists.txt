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
add_library(sevenzip STATIC ${SEVENZIP_BACKEND_SOURCES})
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
add_library(szlengine STATIC gtk/src/engine.cpp)
target_compile_options(szlengine PRIVATE -Wno-unused-parameter)
target_link_libraries(szlengine PUBLIC sevenzip)

add_executable(7zipl
  gtk/src/main.cpp
  gtk/src/app.cpp
  gtk/src/gtkutil.cpp
  gtk/src/style.cpp
  gtk/src/settings.cpp
  gtk/src/util.cpp
  gtk/src/job.cpp
  gtk/src/panel.cpp
  gtk/src/mainwindow.cpp
  gtk/src/compressdialog.cpp
  gtk/src/extractdialog.cpp
  gtk/src/dialogs.cpp
  gtk/src/selftest.cpp
)
target_include_directories(7zipl PRIVATE gtk/src)
target_compile_definitions(7zipl PRIVATE
  SZL_VERSION="${PROJECT_VERSION}"
  SZL_APP_ID="${SZL_APP_ID}"
  GDK_DISABLE_DEPRECATION_WARNINGS)
target_compile_options(7zipl PRIVATE ${SZL_WARN})
target_link_libraries(7zipl PRIVATE szlengine PkgConfig::GTK4)

install(TARGETS 7zipl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Desktop integration
configure_file(data/7zipl.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/${SZL_APP_ID}.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${SZL_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${SZL_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/icons/${SZL_APP_ID}.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES README.md DOC/License.txt DOC/unRarLicense.txt DESTINATION ${CMAKE_INSTALL_DOCDIR})

enable_testing()
add_test(NAME selftest COMMAND 7zipl --selftest)
set_tests_properties(selftest PROPERTIES SKIP_RETURN_CODE 77)
