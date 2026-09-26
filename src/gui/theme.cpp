// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/theme.hpp"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace rl::gui::theme {

namespace {

const char* kStyleSheet = R"QSS(
QMainWindow::separator { background: #1b1c1f; width: 1px; height: 1px; }
QToolTip { color: #dcdee2; background: #32343a; border: 1px solid #3c3f45; padding: 3px 5px; }
QMenuBar { background: #242529; border-bottom: 1px solid #16171a; padding: 1px 2px; }
QMenuBar::item { padding: 4px 9px; background: transparent; border-radius: 3px; }
QMenuBar::item:selected { background: #3a3d44; }
QMenu { background: #2a2b2f; border: 1px solid #16171a; padding: 4px 0; }
QMenu::item { padding: 5px 28px 5px 26px; }
QMenu::item:selected { background: #2f5aa8; color: #ffffff; }
QMenu::item:disabled { color: #62666d; }
QMenu::separator { height: 1px; background: #3c3f45; margin: 4px 8px; }
QMenu::icon { padding-left: 6px; }
QToolBar { background: #242529; border: none; spacing: 2px; padding: 2px; }
QToolBar#ToolOptions { border-bottom: 1px solid #16171a; padding: 3px 6px; spacing: 6px; }
QToolBar#Tools { border-right: 1px solid #16171a; padding: 4px 3px; }
QToolBar#Tools QToolButton { padding: 4px; margin: 1px; border-radius: 4px; }
QToolBar QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 3px; }
QToolBar QToolButton:hover { background: #34363c; border-color: #3c3f45; }
QToolBar QToolButton:checked { background: #2f5aa8; border-color: #4a8cf7; }
QToolBar QLabel { color: #8d9199; padding: 0 2px; }
QStatusBar { background: #242529; border-top: 1px solid #16171a; color: #a4a8b0; }
QStatusBar QLabel { padding: 0 8px; color: #a4a8b0; }
QTreeView, QListView { background: #1f2023; border: none; outline: 0; }
QTreeView::item, QListView::item { padding: 2px 0; }
QTreeView::item:selected, QListView::item:selected { background: #2f5aa8; color: #ffffff; }
QHeaderView::section { background: #2a2b2f; border: none; border-bottom: 1px solid #16171a; padding: 3px; }
QGroupBox { border: 1px solid #3c3f45; border-radius: 4px; margin-top: 10px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; color: #a4a8b0; }
QDialog { background: #2a2b2f; }
QPushButton { padding: 4px 14px; }
#PanelHeaderRow { background: #26272b; border-bottom: 1px solid #16171a; }
#PanelFooterRow { background: #26272b; border-top: 1px solid #16171a; }
#PanelFooterRow QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 3px; }
#PanelFooterRow QToolButton:hover { background: #34363c; border-color: #3c3f45; }
#MessageBadge { color: #e8a33d; }
)QSS";

void set_all_groups(QPalette& p, QPalette::ColorRole role, const QColor& c) {
    p.setColor(QPalette::Active, role, c);
    p.setColor(QPalette::Inactive, role, c);
    p.setColor(QPalette::Disabled, role, c);
}

}  // namespace

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    set_all_groups(p, QPalette::Window, kWindow);
    set_all_groups(p, QPalette::WindowText, kText);
    set_all_groups(p, QPalette::Base, kBase);
    set_all_groups(p, QPalette::AlternateBase, kWindowAlt);
    set_all_groups(p, QPalette::Text, kText);
    set_all_groups(p, QPalette::Button, QColor(0x34, 0x36, 0x3b));
    set_all_groups(p, QPalette::ButtonText, kText);
    set_all_groups(p, QPalette::BrightText, QColor(0xff, 0x6b, 0x6b));
    set_all_groups(p, QPalette::Highlight, kAccentDim);
    set_all_groups(p, QPalette::HighlightedText, Qt::white);
    set_all_groups(p, QPalette::ToolTipBase, kWindowAlt);
    set_all_groups(p, QPalette::ToolTipText, kText);
    set_all_groups(p, QPalette::PlaceholderText, kTextDim);
    set_all_groups(p, QPalette::Link, kAccent);
    set_all_groups(p, QPalette::LinkVisited, kAccent.darker(120));
    set_all_groups(p, QPalette::Light, QColor(0x4a, 0x4d, 0x54));
    set_all_groups(p, QPalette::Midlight, QColor(0x3e, 0x41, 0x47));
    set_all_groups(p, QPalette::Mid, QColor(0x2e, 0x30, 0x34));
    set_all_groups(p, QPalette::Dark, QColor(0x1d, 0x1e, 0x21));
    set_all_groups(p, QPalette::Shadow, QColor(0x10, 0x10, 0x12));
    const QColor dim(0x62, 0x66, 0x6d);
    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x3a, 0x3d, 0x44));
    app.setPalette(p);
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);  // a hint only
    app.setStyleSheet(QString::fromLatin1(kStyleSheet));
}

}  // namespace rl::gui::theme
