// SPDX-License-Identifier: GPL-3.0-or-later
//
// Original single-colour glyphs (24-px grid, 1.5-px strokes, round joins), drawn for Rasterloom;
// none is traced from or modelled on another editor's icon set. Each glyph is authored in black
// and tinted from the palette at render time (normal / active / disabled states).
#pragma once

#include <QIcon>
#include <QString>

namespace rl::gui {

// Returns the icon named `name` ("brush", "eye", ...). Unknown names give a visible placeholder
// glyph (a crossed box) rather than a blank icon.
QIcon icon(const QString& name);

// Every glyph name, for tests.
QStringList icon_names();

}  // namespace rl::gui
