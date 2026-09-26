// SPDX-License-Identifier: GPL-3.0-or-later
//
// File open / save / export: a thin Qt adapter over the core's rl::io API (core/io/file_io.hpp).
// Formats: open PSD/PSB, ORA/.orp, PNG, JPEG, TIFF (the content decides); save .orp (native) and
// .ora (the same bytes, for GIMP); export PNG, JPEG (quality), TIFF, PSD/PSB, ORA. Saves are atomic
// in the core: a failed save leaves the existing file untouched and no temporary file behind.
// Non-fatal losses come back as warnings, shown together in one dialog.
#pragma once

#include <QString>
#include <QStringList>

#include <memory>

#include "core/doc/document.hpp"

namespace rl::gui {

struct LoadResult {
    std::unique_ptr<rl::Document> doc;
    QString error;        // empty on success
    QStringList warnings; // import losses (unsupported adjustments, converted 16-bit/CMYK, ...)
    QString format;       // format key of the content: "png", "jpeg", "tiff", "psd", "ora"
    double ms = 0.0;      // decode time
};

struct SaveResult {
    bool ok = false;
    QString error;
    QStringList warnings;
    double ms = 0.0;
};

struct SaveOptions {
    int jpeg_quality = 92;
    bool force_psb = false;
};

// Lower-case format key from a path's extension: "png", "jpeg", "tiff", "psd", "orp", "ora", "".
QString format_of(const QString& path);
// True for .orp / .ora (lossless, layered: what Save writes).
bool is_native(const QString& format);
QString open_filter();
// Paths the canvas accepts by drag-and-drop / Open (by extension).
bool can_open_path(const QString& path);

// Sides above this open, with a warning (editing ops are specified up to it; io reads PSB sizes).
constexpr int kLargeSide = 16384;

LoadResult load_document(const QString& path, size_t history_depth);
SaveResult save_document(const rl::DocState& state, const QString& path, const SaveOptions& opt = {});

}  // namespace rl::gui
