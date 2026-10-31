option(INSTALL_OPENRC "Install OpenRC script" OFF)
option(INSTALL_RUNIT "Install Runit script" OFF)

configure_file(99-vanh.rules.in ${CMAKE_CURRENT_BINARY_DIR}/99-vanh.rules @ONLY)
configure_file(fcitx5-vanh-server@.service.in ${CMAKE_CURRENT_BINARY_DIR}/fcitx5-vanh-server@.service @ONLY)
configure_file(user-vanh.conf.in ${CMAKE_CURRENT_BINARY_DIR}/user-vanh.conf @ONLY)
configure_file(tmpfiles-vanh.conf.in ${CMAKE_CURRENT_BINARY_DIR}/tmpfiles-vanh.conf @ONLY)

if(INSTALL_OPENRC)
    configure_file(fcitx5-vanh.openrc.in ${CMAKE_CURRENT_BINARY_DIR}/fcitx5-vanh.openrc @ONLY)
endif(INSTALL_OPENRC)

if(INSTALL_RUNIT)
    configure_file(fcitx5-vanh.runit.in ${CMAKE_CURRENT_BINARY_DIR}/fcitx5-vanh.runit @ONLY)
endif(INSTALL_RUNIT)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-vanh-server@.service"
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/99-vanh.rules"
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/udev/rules.d)

install(FILES module-fcitx5-vanh.conf
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/modules-load.d
    RENAME fcitx5-vanh.conf)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/user-vanh.conf"
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/sysusers.d
    RENAME vanh.conf)

# /run/vanh, the socket directory, owned by the uinput proxy user.
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tmpfiles-vanh.conf"
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/tmpfiles.d
    RENAME vanh.conf)

if(INSTALL_OPENRC)
    install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-vanh.openrc"
        DESTINATION /etc/init.d
        RENAME fcitx5-vanh)
endif(INSTALL_OPENRC)

if(INSTALL_RUNIT)
    set(RUNIT_SV_DIR "/etc/sv" CACHE PATH
        "Path to runit service definitions directory (e.g. /etc/sv on Void, /etc/runit/sv on Artix)")
    install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-vanh.runit"
        DESTINATION "${RUNIT_SV_DIR}/fcitx5-vanh"
        RENAME run)
endif(INSTALL_RUNIT)
