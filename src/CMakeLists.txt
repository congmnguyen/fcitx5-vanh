# Core sources are compiled once here and consumed by both the module and
# lotus_test_core; LOTUS_EXPORT keeps the test-facing API linkable.
add_library(lotus_core_objects OBJECT ${fcitx_lotus_core_sources})
set_target_properties(lotus_core_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(lotus_core_objects PRIVATE
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/common
)
target_compile_definitions(lotus_core_objects PRIVATE
    FCITX5_LOTUS_SETTINGS_PATH="${CMAKE_INSTALL_FULL_BINDIR}/fcitx5-vanh-settings"
    LOTUS_UINPUT_PROXY_USER="${LOTUS_UINPUT_PROXY_USER}"
)
target_link_libraries(lotus_core_objects PRIVATE
    Fcitx5::Core
    Fcitx5::Config
    Fcitx5::Module::Emoji
    Lotus::Engine
    PkgConfig::LIBNOTIFY
    PkgConfig::DBUS
    Pthread::Pthread
)

# Parse the shared fcitx5 headers once instead of once per TU.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
    set(LOTUS_PCH_HEADERS
        <fcitx/instance.h>
        <fcitx/addonmanager.h>
        <fcitx/addonfactory.h>
        <fcitx/inputmethodengine.h>
        <fcitx/action.h>
        <fcitx/inputcontext.h>
        <fcitx/candidatelist.h>
        <fcitx/inputpanel.h>
        <fcitx/text.h>
        <fcitx/menu.h>
        <fcitx/userinterface.h>
        <fcitx/userinterfacemanager.h>
        <fcitx-config/iniparser.h>
        <fcitx-utils/log.h>
        <fcitx-utils/event.h>
        <fcitx-utils/keysym.h>
        <fcitx-utils/utf8.h>
    )
    target_precompile_headers(lotus_core_objects PRIVATE ${LOTUS_PCH_HEADERS})
    # test/ reuses the same list for its own PCH host (sibling scope).
    set(LOTUS_PCH_HEADERS "${LOTUS_PCH_HEADERS}" PARENT_SCOPE)
endif()

add_library(lotus MODULE lotus.cpp
    $<TARGET_OBJECTS:lotus_core_objects>
)
set_target_properties(lotus PROPERTIES OUTPUT_NAME "vanh")
set_target_properties(lotus PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(lotus
        Fcitx5::Core
        Fcitx5::Config
        Fcitx5::Module::Emoji
        Lotus::Engine
        PkgConfig::LIBNOTIFY
        PkgConfig::DBUS
        Pthread::Pthread
)

target_include_directories(lotus PRIVATE
    ${PROJECT_BINARY_DIR}
    ${PROJECT_SOURCE_DIR}/common
)
# Same flags as lotus_core_objects so its prebuilt PCH is reused, not rejected.
target_compile_definitions(lotus PRIVATE
    FCITX5_LOTUS_SETTINGS_PATH="${CMAKE_INSTALL_FULL_BINDIR}/fcitx5-vanh-settings"
    LOTUS_UINPUT_PROXY_USER="${LOTUS_UINPUT_PROXY_USER}"
)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
    target_precompile_headers(lotus REUSE_FROM lotus_core_objects)
endif()
install(TARGETS lotus DESTINATION "${CMAKE_INSTALL_LIBDIR}/fcitx5")

fcitx5_translate_desktop_file(vanh.conf.in vanh.conf)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vanh.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod")
configure_file(vanh-addon.conf.in.in vanh-addon.conf.in)
fcitx5_translate_desktop_file("${CMAKE_CURRENT_BINARY_DIR}/vanh-addon.conf.in" vanh-addon.conf)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vanh-addon.conf" RENAME vanh.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")

# Eagerly loaded module that renames the former "lotus" input method in the user's fcitx5
# profile before fcitx5 reads it (see vanh-migrate.cpp).
add_library(vanh_migrate MODULE vanh-migrate.cpp vanh-migration.cpp)
set_target_properties(vanh_migrate PROPERTIES OUTPUT_NAME "vanh-migrate" POSITION_INDEPENDENT_CODE ON)
target_include_directories(vanh_migrate PRIVATE ${PROJECT_BINARY_DIR} ${PROJECT_SOURCE_DIR}/common)
target_link_libraries(vanh_migrate Fcitx5::Core Fcitx5::Utils)
install(TARGETS vanh_migrate DESTINATION "${CMAKE_INSTALL_LIBDIR}/fcitx5")
configure_file(vanh-migrate-addon.conf.in vanh-migrate-addon.conf.in @ONLY)
fcitx5_translate_desktop_file("${CMAKE_CURRENT_BINARY_DIR}/vanh-migrate-addon.conf.in" vanh-migrate-addon.conf)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vanh-migrate-addon.conf" RENAME vanh-migrate.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")
