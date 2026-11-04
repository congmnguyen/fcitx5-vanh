cmake_minimum_required(VERSION 3.16)

project(fcitx5-vanh VERSION 6.0.5)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(BUILD_TESTING "Build Vanh tests" OFF)

# The addon entry point is intentionally excluded: headless tests link the
# production implementation through the lotus_test_core target, never the
# module build itself.
set(fcitx_lotus_core_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-kb-sender.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-fcitx-text.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-mode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-mode-menu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-candidates.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/window_focus_tracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-plasma-theme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-key-buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lotus-notify.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emoji.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vanh-migration.cpp
)

set(REQUIRED_FCITX_VERSION 5.0.14)
find_package(ECM 1.0.0 REQUIRED)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
include(FeatureSummary)
include(GNUInstallDirs)
include(ECMUninstallTarget)

find_package(PkgConfig REQUIRED)
find_package(Fcitx5Core ${REQUIRED_FCITX_VERSION} REQUIRED)
find_package(Fcitx5ModuleEmoji REQUIRED)
find_package(Pthread REQUIRED)
find_package(Gettext REQUIRED)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(LIBNOTIFY REQUIRED IMPORTED_TARGET libnotify)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(-DFCITX_GETTEXT_DOMAIN=\"fcitx5-vanh\") 

if (Fcitx5Utils_VERSION VERSION_GREATER_EQUAL "5.1.13")
    add_definitions(-DLOTUS_USE_MODERN_FCITX_API)
    message(STATUS "Fcitx5 version ${Fcitx5Utils_VERSION} detected: Using modern API")
else()
    message(STATUS "Fcitx5 version ${Fcitx5Utils_VERSION} detected: Using legacy API")
endif()
fcitx5_add_i18n_definition()

# Absolute paths that every packaging has to override.
set(LOTUS_SETFACL_EXECUTABLE "/usr/bin/setfacl" CACHE FILEPATH
    "setfacl used by the udev rule and the service units")

# The uinput server runs as this user; the addon only talks to a server socket it owns.
set(LOTUS_UINPUT_PROXY_USER "uinput_proxy" CACHE STRING
    "System user of the uinput server: rw ACL on /dev/uinput, owner of /run/vanh (udev rules, sysusers.d, tmpfiles.d, service scripts, addon peer check)")
add_subdirectory(po)
add_subdirectory(engine-rs)
add_subdirectory(src)
add_subdirectory(data)
add_subdirectory(server-rs)
add_subdirectory(misc)
add_subdirectory(settings-gui)

if (BUILD_TESTING)
    include(CTest)
    add_subdirectory(test)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/org.fcitx.Fcitx5.Addon.Vanh.metainfo.xml.in.in"
    "${CMAKE_CURRENT_BINARY_DIR}/org.fcitx.Fcitx5.Addon.Vanh.metainfo.xml.in"
    @ONLY
)

fcitx5_translate_desktop_file(
    "${CMAKE_CURRENT_BINARY_DIR}/org.fcitx.Fcitx5.Addon.Vanh.metainfo.xml.in"
    org.fcitx.Fcitx5.Addon.Vanh.metainfo.xml
    XML
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.fcitx.Fcitx5.Addon.Vanh.metainfo.xml" DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

install(FILES 
        LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/${PROJECT_NAME}
)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
