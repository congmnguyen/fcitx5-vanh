#!/bin/bash

xgettext \
--language=C++ \
--from-code=UTF-8 \
--keyword=_ \
--keyword=N_ \
-o /tmp/vanh-cpp.pot \
$(find . \( -name "*.cpp" -o -name "*.h" \))

xgettext \
--language=appdata \
--from-code=UTF-8 \
-o /tmp/vanh-xml.pot \
org.fcitx.Fcitx5.Addon.Vanh.metainfo.xml.in.in

xgettext \
--language=Python \
--from-code=UTF-8 \
--keyword=_ \
--keyword=N_ \
-o /tmp/vanh-python.pot \
$(find . -name "*.py")

xgettext \
--language=Desktop \
--from-code=UTF-8 \
--keyword=Name \
--keyword=Comment \
-o /tmp/vanh-desktop.pot \
settings-gui/org.fcitx.Fcitx5.Addon.Vanh.Settings.desktop.in

{
    echo 'msgid ""'
    echo 'msgstr ""'
    echo '"Content-Type: text/plain; charset=UTF-8\n"'
    echo ""
    grep -hE "^Name=" \
    src/vanh.conf.in \
    src/vanh-addon.conf.in.in \
    src/vanh-migrate-addon.conf.in \
    | sed 's/^Name=\(.*\)/msgid "\1"\nmsgstr ""\n/'
} > /tmp/vanh-conf.pot

msgcat \
--use-first \
--sort-output \
/tmp/vanh-cpp.pot \
/tmp/vanh-xml.pot \
/tmp/vanh-conf.pot \
/tmp/vanh-python.pot \
/tmp/vanh-desktop.pot \
-o po/fcitx5-vanh.pot

rm -f /tmp/vanh-*.pot