# SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later

from i18n import _
from qtpy.QtCore import Qt, QUrl
from qtpy.QtGui import QDesktopServices, QIcon
from qtpy.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from ui.components import ScrollContent

try:
    from version import __version__
except ImportError:
    __version__ = "dev version"  # Fallback for local development


ISSUES_URL = "https://github.com/congmnguyen/fcitx5-vanh/issues/new"


class AboutPage(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        root_layout = QVBoxLayout(self)
        root_layout.setContentsMargins(0, 0, 0, 0)

        # Scroll Area to handle overcrowding
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        scroll.setAttribute(Qt.WA_TranslucentBackground)

        content_widget = ScrollContent()
        content_widget.setObjectName("AboutContent")

        layout = QVBoxLayout(content_widget)
        layout.setContentsMargins(40, 30, 40, 40)
        layout.setSpacing(20)
        layout.setAlignment(Qt.AlignTop | Qt.AlignHCenter)

        try:
            pixmap = QIcon.fromTheme("fcitx-vanh").pixmap(80, 80)
        except Exception:
            pixmap = None
        if pixmap is None or pixmap.isNull():
            logo = QLabel("🪷")
            logo.setStyleSheet("font-size: 64px; margin-bottom: 5px;")
        else:
            logo = QLabel()
            logo.setPixmap(pixmap)
            logo.setStyleSheet("margin-bottom: 5px;")

        layout.addWidget(logo, alignment=Qt.AlignCenter)

        title = QLabel("Fcitx5 Vanh")
        title.setObjectName("AboutTitle")
        layout.addWidget(title, alignment=Qt.AlignCenter)

        version = QLabel(_("Version {}").format(__version__))
        version.setObjectName("VersionTag")
        version.setAlignment(Qt.AlignCenter)
        version.setStyleSheet("""
            QLabel#VersionTag {
                background-color: palette(highlight);
                color: palette(highlighted-text);
                border-radius: 10px;
                padding: 2px 10px;
                font-size: 11px;
                font-weight: bold;
            }
        """)
        layout.addWidget(version, alignment=Qt.AlignCenter)

        desc = QLabel(_("Modern, fast, and stable Vietnamese input method for Linux."))
        desc.setWordWrap(True)
        desc.setAlignment(Qt.AlignCenter)
        desc.setObjectName("AboutDescription")
        desc.setMinimumHeight(60)
        layout.addWidget(desc, alignment=Qt.AlignCenter)

        # GitHub Project Link
        github_link = QLabel(
            '<a href="https://github.com/congmnguyen/fcitx5-vanh" style="text-decoration: none;">https://github.com/congmnguyen/fcitx5-vanh</a>'
        )
        github_link.setOpenExternalLinks(True)
        layout.addWidget(github_link, alignment=Qt.AlignCenter)

        # Support Buttons Row
        support_layout = QHBoxLayout()
        support_layout.setSpacing(15)
        support_layout.setAlignment(Qt.AlignCenter)

        btn_bug = QPushButton(_("Report Bug"))
        btn_bug.setObjectName("BugReport")
        btn_bug.setFixedWidth(200)
        btn_bug.clicked.connect(lambda: QDesktopServices.openUrl(QUrl(ISSUES_URL)))

        btn_feature = QPushButton(_("Request Feature"))
        btn_feature.setObjectName("FeatureRequest")
        btn_feature.setFixedWidth(200)
        btn_feature.clicked.connect(lambda: QDesktopServices.openUrl(QUrl(ISSUES_URL)))

        support_layout.addWidget(btn_bug)
        support_layout.addWidget(btn_feature)
        layout.addLayout(support_layout)

        line = QFrame()
        line.setFrameShape(QFrame.HLine)
        line.setObjectName("AboutLine")
        layout.addWidget(line)

        # Credits Section
        credits_title = QLabel(_("Developed by"))
        credits_title.setObjectName("CreditsTitle")
        layout.addWidget(credits_title, alignment=Qt.AlignCenter)

        # Credit to the upstream project Vanh was forked from
        contributors_link = QLabel(
            '<a href="https://github.com/LotusInputMethod/fcitx5-lotus/graphs/contributors" style="text-decoration: none;">'
            + _("Based on fcitx5-lotus by its contributors")
            + "</a>"
        )
        contributors_link.setOpenExternalLinks(True)
        contributors_link.setCursor(Qt.PointingHandCursor)
        contributors_link.setObjectName("AuthorLink")
        layout.addWidget(contributors_link, alignment=Qt.AlignCenter)

        layout.addStretch()

        # Footer (Open Source Licenses)
        footer_line = QFrame()
        footer_line.setFrameShape(QFrame.HLine)
        footer_line.setObjectName("AboutLine")
        layout.addWidget(footer_line)

        license_title = QLabel(_("Open Source Licenses"))
        license_title.setObjectName("CreditsTitle")
        layout.addWidget(license_title, alignment=Qt.AlignCenter)

        # Main License
        main_license_info = QLabel(
            _(
                "This project is licensed under the <b>GNU General Public License v3.0 or later (GPL-3.0-or-later)</b>."
            )
        )
        main_license_info.setTextFormat(Qt.RichText)
        main_license_info.setWordWrap(True)
        main_license_info.setAlignment(Qt.AlignCenter)
        main_license_info.setObjectName("LicenseInfo")
        layout.addWidget(main_license_info)

        # Third-party & Upstream Info
        third_party_info = QLabel(
            _(
                "Based on upstream work originally licensed under <b>LGPL-2.1-or-later</b>.<br>"
                "Includes third-party components licensed under the <b>MIT License</b>."
            )
        )
        third_party_info.setTextFormat(Qt.RichText)
        third_party_info.setWordWrap(True)
        third_party_info.setAlignment(Qt.AlignCenter)
        third_party_info.setObjectName("LicenseInfo")
        third_party_info.setStyleSheet("opacity: 0.8; margin-top: 5px;")
        layout.addWidget(third_party_info)

        scroll.setWidget(content_widget)
        root_layout.addWidget(scroll)
