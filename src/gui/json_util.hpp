// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small helpers for building op JSON from GUI values.
#pragma once

#include <QColor>
#include <QString>

#include <cstdio>
#include <string>

#include "core/base/types.hpp"
#include "core/doc/node.hpp"
#include "core/script/fields.hpp"

namespace rl::gui {

inline std::string hex_rgba(int r, int g, int b, int a) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", r & 255, g & 255, b & 255, a & 255);
    return buf;
}
inline std::string hex_rgb(int r, int g, int b) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", r & 255, g & 255, b & 255);
    return buf;
}
inline std::string hex(const QColor& c) { return hex_rgba(c.red(), c.green(), c.blue(), c.alpha()); }
inline std::string hex_opaque(const QColor& c) { return hex_rgb(c.red(), c.green(), c.blue()); }
inline std::string hex(rl::Rgba8 p) { return hex_rgba(p.r, p.g, p.b, p.a); }

inline QColor to_qcolor(rl::Rgba8 p) { return QColor(p.r, p.g, p.b, p.a); }
inline rl::Rgba8 to_rgba8(const QColor& c) {
    return rl::Rgba8{static_cast<uint8_t>(c.red()), static_cast<uint8_t>(c.green()), static_cast<uint8_t>(c.blue()),
                     static_cast<uint8_t>(c.alpha())};
}

inline std::string to_std(const QString& s) { return s.toStdString(); }
inline QString to_q(const std::string& s) { return QString::fromStdString(s); }

// doc 60 §1.1: what the UI shows for a node is its name, or its id while it has none. Ids stay the
// op addresses (immutable, §1.2); names are display only.
inline QString layer_display_name(const rl::Node& n) { return to_q(n.name.empty() ? n.id : n.name); }

}  // namespace rl::gui
