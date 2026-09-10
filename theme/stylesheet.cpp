// theme/stylesheet.cpp

#include "theme/stylesheet.h"

#include "theme/color.h"

namespace omega::theme {
namespace {

// Replaces every occurrence of one placeholder. The QSS below contains no
// literal "{bg}" or the like outside the placeholders, so a plain scan is
// enough and no escaping scheme is needed.
void substitute(std::string *s, const std::string &token,
                const std::string &value) {
    const std::string needle = "{" + token + "}";
    size_t at = 0;
    while ((at = s->find(needle, at)) != std::string::npos) {
        s->replace(at, needle.size(), value);
        at += value.size();
    }
}

// The template, exactly as stylesheet.py emits it.
constexpr const char *kTemplate = R"QSS(
    /* Main window and containers */
    QMainWindow, QDialog, QWidget {
        background-color: {bg};
        color: {fg};
    }

    /* Splitter */
    QSplitter::handle {
        background-color: {border};
    }
    QSplitter::handle:horizontal {
        width: 2px;
    }
    QSplitter::handle:vertical {
        height: 2px;
    }

    /* Menu bar */
    QMenuBar {
        background-color: {bg};
        color: {fg};
        border-bottom: 1px solid {border};
        padding: 2px;
    }
    QMenuBar::item {
        padding: 4px 8px;
        background-color: transparent;
    }
    QMenuBar::item:selected {
        background-color: {bg_lighter};
        border-radius: 4px;
    }

    /* Menus */
    QMenu {
        background-color: {bg};
        color: {fg};
        border: 1px solid {border};
        border-radius: 4px;
        padding: 4px;
    }
    QMenu::item {
        padding: 6px 24px 6px 8px;
        border-radius: 4px;
    }
    QMenu::item:selected {
        background-color: {accent};
        color: {bg};
    }
    QMenu::separator {
        height: 1px;
        background-color: {border};
        margin: 4px 8px;
    }

    /* Tab widget */
    QTabWidget::pane {
        border: 1px solid {border};
        border-radius: 4px;
        background-color: {bg};
    }
    QTabBar::tab {
        background-color: {bg_darker};
        color: {fg};
        border: 1px solid {border};
        border-bottom: none;
        padding: 6px 12px;
        margin-right: 2px;
        border-top-left-radius: 4px;
        border-top-right-radius: 4px;
    }
    QTabBar::tab:selected {
        background-color: {bg};
        border-bottom: 1px solid {bg};
    }
    QTabBar::tab:hover:!selected {
        background-color: {bg_lighter};
    }
    QTabBar::close-button {
        image: none;
        subcontrol-position: right;
    }
    QTabBar::close-button:hover {
        background-color: {red};
        border-radius: 2px;
    }

    /* Tree widget */
    QTreeWidget, QTreeView {
        background-color: {bg};
        color: {fg};
        border: 1px solid {border};
        border-radius: 4px;
        outline: none;
    }
    QTreeWidget::item, QTreeView::item {
        padding: 4px;
        border-radius: 4px;
    }
    QTreeWidget::item:hover, QTreeView::item:hover {
        background-color: {bg_lighter};
    }
    QTreeWidget::item:selected, QTreeView::item:selected {
        background-color: {accent};
        color: {bg};
    }
    QTreeWidget::branch {
        background-color: {bg};
    }

    /* Line edit */
    QLineEdit {
        background-color: {bg_darker};
        color: {fg};
        border: 1px solid {border};
        border-radius: 4px;
        padding: 6px 8px;
        selection-background-color: {accent};
        selection-color: {bg};
    }
    QLineEdit:focus {
        border-color: {accent};
    }
    QLineEdit:disabled {
        background-color: {bg};
        color: {border};
    }

    /* Spin box */
    QSpinBox {
        background-color: {bg_darker};
        color: {fg};
        border: 1px solid {border};
        border-radius: 4px;
        padding: 4px 8px;
    }
    QSpinBox:focus {
        border-color: {accent};
    }
    QSpinBox::up-button, QSpinBox::down-button {
        background-color: {bg_lighter};
        border: none;
        width: 16px;
    }
    QSpinBox::up-button:hover, QSpinBox::down-button:hover {
        background-color: {accent};
    }

    /* Combo box */
    QComboBox {
        background-color: {bg_darker};
        color: {fg};
        border: 1px solid {border};
        border-radius: 4px;
        padding: 6px 8px;
        min-width: 100px;
    }
    QComboBox:focus {
        border-color: {accent};
    }
    QComboBox::drop-down {
        border: none;
        width: 20px;
    }
    QComboBox::down-arrow {
        width: 12px;
        height: 12px;
    }
    QComboBox QAbstractItemView {
        background-color: {bg};
        color: {fg};
        border: 1px solid {border};
        selection-background-color: {accent};
        selection-color: {bg};
    }

    /* Push button */
    QPushButton {
        background-color: {bg_lighter};
        color: {fg};
        border: 1px solid {border};
        border-radius: 4px;
        padding: 6px 16px;
        min-width: 60px;
    }
    QPushButton:hover {
        background-color: {accent};
        color: {bg};
        border-color: {accent};
    }
    QPushButton:pressed {
        background-color: {accent_pressed};
    }
    QPushButton:disabled {
        background-color: {bg_darker};
        color: {border};
        border-color: {bg_darker};
    }
    QPushButton:default {
        border-color: {accent};
    }

    /* Group box */
    QGroupBox {
        color: {fg};
        border: 1px solid {border};
        border-radius: 4px;
        margin-top: 8px;
        padding-top: 8px;
    }
    QGroupBox::title {
        subcontrol-origin: margin;
        subcontrol-position: top left;
        padding: 0 4px;
        color: {fg};
    }

    /* Labels */
    QLabel {
        color: {fg};
        background-color: transparent;
    }

    /* Scroll bars */
    QScrollBar:vertical {
        background-color: {bg};
        width: 12px;
        border-radius: 6px;
    }
    QScrollBar::handle:vertical {
        background-color: {border};
        border-radius: 6px;
        min-height: 20px;
    }
    QScrollBar::handle:vertical:hover {
        background-color: {accent};
    }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
        height: 0;
    }
    QScrollBar:horizontal {
        background-color: {bg};
        height: 12px;
        border-radius: 6px;
    }
    QScrollBar::handle:horizontal {
        background-color: {border};
        border-radius: 6px;
        min-width: 20px;
    }
    QScrollBar::handle:horizontal:hover {
        background-color: {accent};
    }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
        width: 0;
    }

    /* Check box */
    QCheckBox {
        color: {fg};
        spacing: 8px;
    }
    QCheckBox::indicator {
        width: 16px;
        height: 16px;
        border: 1px solid {border};
        border-radius: 4px;
        background-color: {bg_darker};
    }
    QCheckBox::indicator:checked {
        background-color: {accent};
        border-color: {accent};
    }
    QCheckBox::indicator:hover {
        border-color: {accent};
    }

    /* Tool tip */
    QToolTip {
        background-color: {bg_lighter};
        color: {fg};
        border: 1px solid {border};
        border-radius: 4px;
        padding: 4px 8px;
    }

    /* Message box */
    QMessageBox {
        background-color: {bg};
    }
    QMessageBox QLabel {
        color: {fg};
    }

    /* Dialog button box */
    QDialogButtonBox {
        button-layout: 3;
    }

    /* Frame */
    QFrame {
        background-color: transparent;
    }

    /* Header view (for tree/table) */
    QHeaderView::section {
        background-color: {bg_darker};
        color: {fg};
        border: none;
        border-right: 1px solid {border};
        padding: 4px 8px;
    }
    )QSS";

}  // namespace

std::string generateStylesheet(const Theme &theme) {
    const std::string bg = theme.background_color;
    const std::string fg = theme.foreground_color;
    const std::string border = theme.border_color;
    const std::string accent = theme.accent_color;

    const std::string bg_lighter = lighten(bg, 0.1);
    const std::string bg_darker = darken(bg, 0.1);
    const std::string accent_pressed = darken(accent, 0.1);

    // The Python also derives accent_hover and pulls green and yellow out of
    // the terminal colours. None of the three appears in the template, so they
    // are not computed here. Red does -- it is the tab close button.
    const std::string red = theme.color("red", "#f38ba8");

    std::string out = kTemplate;
    substitute(&out, "bg_lighter", bg_lighter);
    substitute(&out, "bg_darker", bg_darker);
    substitute(&out, "accent_pressed", accent_pressed);
    substitute(&out, "bg", bg);
    substitute(&out, "fg", fg);
    substitute(&out, "border", border);
    substitute(&out, "accent", accent);
    substitute(&out, "red", red);
    return out;
}

}  // namespace omega::theme
