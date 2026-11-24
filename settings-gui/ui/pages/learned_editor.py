# SPDX-FileCopyrightText: 2026 Cong Nguyen <congmnguyen@users.noreply.github.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Learned Words Editor Page. Edits vanh-learned.tsv (LearnFromReverts)."""

import os
import tempfile

from core.dbus_handler import VanhDBusHandler
from i18n import _
from qtpy.QtCore import Qt
from qtpy.QtGui import QIcon
from qtpy.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QComboBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from ui.helpers import bool_to_str, config_bool, local_learned_path, set_add_button_mode
from ui.pages.base_editor import BaseEditorPage
from ui.pages.dynamic_settings import CardWidget

LEARNED_SCORE_LIMIT = 8


def is_valid_learned_key(key: str) -> bool:
    """Matches engine-rs/src/learning.rs: 1..39 lowercase ASCII letters."""
    return 0 < len(key) < 40 and all("a" <= ch <= "z" for ch in key)


def parse_learned_text(text: str) -> dict[str, int]:
    """Parses vanh-learned.tsv into a {keys: score} mapping."""
    prefs: dict[str, int] = {}
    for line in text.splitlines():
        if "\t" not in line:
            continue
        keys, raw_score = line.split("\t", 1)
        if not is_valid_learned_key(keys):
            continue
        try:
            score = int(raw_score.strip())
        except ValueError:
            continue
        if score != 0:
            prefs[keys] = max(-LEARNED_SCORE_LIMIT, min(LEARNED_SCORE_LIMIT, score))
    return prefs


def format_learned_text(prefs: dict[str, int]) -> str:
    """Serializes a {keys: score} mapping to sorted vanh-learned.tsv text."""
    rows = sorted(
        (k, max(-LEARNED_SCORE_LIMIT, min(LEARNED_SCORE_LIMIT, v)))
        for k, v in prefs.items()
        if is_valid_learned_key(k) and v != 0
    )
    return "".join(f"{keys}\t{score}\n" for keys, score in rows)


class LearnedEditorPage(BaseEditorPage):
    """UI for viewing and editing words learned from Revert key presses."""

    def __init__(self, dbus_handler: VanhDBusHandler, parent=None):
        super().__init__(parent)
        self.dbus = dbus_handler
        self.prefs: dict[str, int] = {}
        self.initial_state: dict = {}
        self._is_loaded = False
        self._load_failed = False

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(30, 20, 30, 20)
        main_layout.setSpacing(15)

        title = QLabel(_("Learned Words"))
        title.setObjectName("CategoryTitle")
        main_layout.addWidget(title)

        explanation = QLabel(
            _(
                "Words remembered locally when you press the Revert key. You can remove accidental entries or pin words to English or Vietnamese."
            )
        )
        explanation.setWordWrap(True)
        explanation.setStyleSheet("color: gray; font-size: 13px;")
        main_layout.addWidget(explanation)

        toggles_card = CardWidget("")
        toggles_layout = QHBoxLayout()
        self.cb_enable = QCheckBox(_("Learn from reverts"))
        self.cb_enable.toggled.connect(self._on_item_changed)
        toggles_layout.addWidget(self.cb_enable)
        toggles_layout.addStretch()

        self.search_input = QLineEdit()
        self.search_input.setPlaceholderText(_("Search words..."))
        self.search_input.setClearButtonEnabled(True)
        self.search_input.setFixedWidth(200)
        self.search_input.textChanged.connect(self.on_search_changed)
        toggles_layout.addWidget(QLabel(_("Search:")))
        toggles_layout.addWidget(self.search_input)
        toggles_card.content_layout.addLayout(toggles_layout)
        main_layout.addWidget(toggles_card)

        editor_card = CardWidget("")
        content_layout = QVBoxLayout()
        editor_card.content_layout.addLayout(content_layout)
        main_layout.addWidget(editor_card)

        input_layout = QHBoxLayout()
        self.input_key = QLineEdit()
        self.input_key.setPlaceholderText(_("Keys (e.g. host, conf)"))
        self.input_key.setClearButtonEnabled(True)
        self.input_key.returnPressed.connect(self.on_add)
        self.input_key.textChanged.connect(self._update_add_button)

        self.combo_pref = QComboBox()
        self.combo_pref.addItem(_("Keep English (keys)"), 1)
        self.combo_pref.addItem(_("Prefer Vietnamese"), -1)

        self.btn_add = QPushButton(QIcon.fromTheme("list-add"), _("Add"))
        self.btn_add.clicked.connect(self.on_add)

        input_layout.addWidget(QLabel(_("Keys:")))
        input_layout.addWidget(self.input_key, 1)
        input_layout.addWidget(self.combo_pref)
        input_layout.addWidget(self.btn_add)
        content_layout.addLayout(input_layout)

        self.table = QTableWidget(0, 2)
        self.table.setHorizontalHeaderLabels([_("Keys"), _("Preference")])
        self.table.verticalHeader().setVisible(False)
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.table.setAlternatingRowColors(True)
        self.apply_table_style()
        self.table.cellClicked.connect(self.on_cell_clicked)
        content_layout.addWidget(self.table)

        toolbar_layout = QHBoxLayout()
        toolbar_layout.setContentsMargins(0, 5, 0, 0)

        self.btn_flip = QPushButton(QIcon.fromTheme("object-flip-horizontal"), _("Flip"))
        self.btn_flip.clicked.connect(self.on_flip)

        self.btn_remove = QPushButton(QIcon.fromTheme("list-remove"), _("Remove"))
        self.btn_remove.clicked.connect(self.on_remove)

        self.btn_clear = QPushButton(QIcon.fromTheme("edit-clear"), _("Clear All"))
        self.btn_clear.clicked.connect(self.on_clear_all)

        toolbar_layout.addWidget(self.btn_flip)
        toolbar_layout.addWidget(self.btn_remove)
        toolbar_layout.addStretch()
        toolbar_layout.addWidget(self.btn_clear)
        content_layout.addLayout(toolbar_layout)

        self._update_add_button()
        self.update_button_states()

    def showEvent(self, event):
        super().showEvent(event)
        if not self._is_loaded:
            self.load_data()
            self._is_loaded = True

    def load_data(self):
        self.blockSignals(True)
        self._load_failed = False
        try:
            config_data = self.dbus.get_config()
            if config_data:
                values = config_data.get("values", {})
                self.cb_enable.setChecked(config_bool(values, "LearnFromReverts", "False"))

            self.prefs = {}
            path = local_learned_path()
            if os.path.exists(path):
                try:
                    with open(path, "r", encoding="utf-8") as f:
                        self.prefs = parse_learned_text(f.read())
                except OSError as exc:
                    self._load_failed = True
                    QMessageBox.warning(
                        self,
                        _("Warning"),
                        _("Failed to load learned words: {}").format(exc),
                    )
            self._rebuild_table()
            self.initial_state = self._get_current_state()
        finally:
            self.blockSignals(False)
            self.on_search_changed()
            self.update_button_states()

    def _format_pref(self, score: int) -> str:
        if score > 0:
            return _("English (+{})").format(score)
        return _("Vietnamese ({})").format(score)

    def _rebuild_table(self, filtered_keys: list[str] | None = None):
        keys = filtered_keys if filtered_keys is not None else sorted(self.prefs)
        self.table.setRowCount(len(keys))
        for row, key in enumerate(keys):
            score = self.prefs[key]
            key_item = QTableWidgetItem(key)
            pref_item = QTableWidgetItem(self._format_pref(score))
            pref_item.setData(Qt.UserRole, score)
            self.table.setItem(row, 0, key_item)
            self.table.setItem(row, 1, pref_item)

    def _get_current_state(self) -> dict:
        return {
            "prefs": dict(sorted(self.prefs.items())),
            "LearnFromReverts": self.cb_enable.isChecked(),
        }

    def is_modified(self) -> bool:
        return self._get_current_state() != self.initial_state

    def is_modified_from_default(self) -> bool:
        return bool(self.prefs) or self.cb_enable.isChecked()

    def restore_defaults(self):
        self.blockSignals(True)
        try:
            self.cb_enable.setChecked(False)
            self.prefs.clear()
            self._rebuild_table()
            self._on_item_changed()
        finally:
            self.blockSignals(False)
            self.update_button_states()

    def save_data(self) -> bool:
        if self._load_failed:
            return False

        path = local_learned_path()
        tmp_name = None
        try:
            target_dir = os.path.dirname(path)
            os.makedirs(target_dir, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                "w", dir=target_dir, encoding="utf-8", delete=False
            ) as tf:
                tf.write(format_learned_text(self.prefs))
                tmp_name = tf.name
            os.chmod(tmp_name, 0o600)
            os.replace(tmp_name, path)
        except OSError as exc:
            if tmp_name and os.path.exists(tmp_name):
                try:
                    os.remove(tmp_name)
                except OSError:
                    pass
            QMessageBox.warning(self, _("Error"), _("Failed to save learned words: {}").format(exc))
            return False

        # SetConfig triggers LotusEngine::loadLearned() so changes apply immediately.
        if not self.dbus.save_values({"LearnFromReverts": bool_to_str(self.cb_enable.isChecked())}):
            return False

        self.initial_state = self._get_current_state()
        return True

    def on_search_changed(self):
        query = self.search_input.text().strip().lower()
        if not query:
            self._rebuild_table()
            return
        self._rebuild_table([k for k in sorted(self.prefs) if query in k])

    def _update_add_button(self):
        key = self.input_key.text().strip().lower()
        valid = is_valid_learned_key(key)
        if key and not valid:
            self.input_key.setStyleSheet("color: red;")
            self.input_key.setToolTip(_("Use 1 to 39 lowercase ASCII letters only."))
        else:
            self.input_key.setStyleSheet("")
            self.input_key.setToolTip("")
        self.btn_add.setEnabled(valid)
        set_add_button_mode(self.btn_add, key in self.prefs, _("Update"), _("Add"))

    def update_button_states(self):
        super().update_button_states()
        has_sel = bool(self.table.selectedItems())
        if hasattr(self, "btn_flip"):
            self.btn_flip.setEnabled(has_sel)
        if hasattr(self, "btn_clear"):
            self.btn_clear.setEnabled(bool(self.prefs))

    def on_cell_clicked(self, row, _column):
        key_item = self.table.item(row, 0)
        if key_item and key_item.text() in self.prefs:
            key = key_item.text()
            self.input_key.setText(key)
            idx = 0 if self.prefs[key] > 0 else 1
            self.combo_pref.setCurrentIndex(idx)
        self.update_button_states()

    def on_add(self):
        key = self.input_key.text().strip().lower()
        if not is_valid_learned_key(key):
            return
        direction = int(self.combo_pref.currentData())
        self.prefs[key] = direction
        self.input_key.clear()
        self.input_key.setFocus()
        self.on_search_changed()
        self.update_button_states()
        self._on_item_changed()

    def on_flip(self):
        rows = sorted({item.row() for item in self.table.selectedItems()})
        if not rows:
            return
        for row in rows:
            key_item = self.table.item(row, 0)
            if key_item and key_item.text() in self.prefs:
                key = key_item.text()
                self.prefs[key] = -1 if self.prefs[key] > 0 else 1
        self.on_search_changed()
        self.update_button_states()
        self._on_item_changed()

    def on_remove(self):
        rows = sorted({item.row() for item in self.table.selectedItems()})
        if not rows:
            return
        for row in rows:
            key_item = self.table.item(row, 0)
            if key_item:
                self.prefs.pop(key_item.text(), None)
        self.on_search_changed()
        self.update_button_states()
        self._update_add_button()
        self._on_item_changed()

    def on_clear_all(self):
        if not self.prefs:
            return
        reply = QMessageBox.question(
            self,
            _("Clear Learned Words"),
            _("Remove all learned words?"),
            QMessageBox.Yes | QMessageBox.No,
        )
        if reply == QMessageBox.Yes:
            self.prefs.clear()
            self.on_search_changed()
            self.update_button_states()
            self._update_add_button()
            self._on_item_changed()
