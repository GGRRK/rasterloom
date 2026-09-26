// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one place the GUI's colours live (docs/research/qt-platform.md section 4): a dark Fusion
// palette built role by role for all three colour groups, plus a small targeted style sheet for
// what the palette cannot express. The palette is set explicitly so an inherited platform theme
// (qt6ct, GTK) can never leak into the look or into offscreen tests.
#pragma once

#include <QColor>

class QApplication;

namespace rl::gui::theme {

// Colour tokens. Everything the GUI paints by hand reads these.
inline const QColor kWorkspace{0x1b, 0x1c, 0x1f};   // around the canvas
inline const QColor kWindow{0x2a, 0x2b, 0x2f};      // panels, menus
inline const QColor kWindowAlt{0x32, 0x34, 0x39};   // alternating rows, headers
inline const QColor kBase{0x1f, 0x20, 0x23};        // inputs, lists
inline const QColor kBorder{0x3c, 0x3f, 0x45};
inline const QColor kText{0xdc, 0xde, 0xe2};
inline const QColor kTextDim{0x8d, 0x91, 0x99};
inline const QColor kAccent{0x4a, 0x8c, 0xf7};
inline const QColor kAccentDim{0x2f, 0x5a, 0xa8};
inline const QColor kChecker1{0x9a, 0x9a, 0x9a};
inline const QColor kChecker2{0x6e, 0x6e, 0x6e};
inline const QColor kWarn{0xe8, 0xa3, 0x3d};

inline constexpr int kCheckerCell = 8;  // logical pixels, fixed in screen space

// Sets the Fusion style, the palette and the style sheet on `app`. Call once, before any widget.
void apply(QApplication& app);

}  // namespace rl::gui::theme
