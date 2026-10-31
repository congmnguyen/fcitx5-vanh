install(DIRECTORY icons/ DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/"
    PATTERN .* EXCLUDE
    PATTERN *~ EXCLUDE)

# ── Status/tray icon coverage: hicolor status/ ────────────────────────────
# The engine returns plain icon names (see lotus-engine.cpp subModeIconImpl);
# the SNI host resolves them through standard XDG icon theme lookup. Install
# copies of every variant into hicolor status dirs in addition to the
# breeze/breeze-dark theme dirs installed above.
set(LOTUS_STATUS_ICONS
    fcitx-vanh fcitx-vanh-off fcitx-vanh-emoji
    fcitx-vanh-default fcitx-vanh-default-black
    fcitx-vanh-off-default fcitx-vanh-off-default-black
    fcitx-vanh-emoji-default fcitx-vanh-emoji-default-black
    fcitx-vanh-default-symbolic
    fcitx-vanh-off-default-symbolic
    fcitx-vanh-emoji-default-symbolic)

foreach(ICON ${LOTUS_STATUS_ICONS})
    # status/ — system tray (SVG, renderable by most compositors and SNI hosts)
    install(FILES icons/hicolor/scalable/apps/${ICON}.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/status"
        RENAME "${ICON}.svg")
endforeach()

install(FILES dictionaries/vietnamese.cm.dict DESTINATION "${FCITX_INSTALL_PKGDATADIR}/vanh")
