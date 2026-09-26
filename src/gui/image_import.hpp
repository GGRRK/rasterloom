// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading images from outside the document: files (drag-and-drop, File > Place, Open) and the
// system clipboard. The core codecs (PNG, JPEG, TIFF, PSD/PSB, OpenRaster) decide by content first;
// every other format Qt's image plugins can read on this system (GIF first frame, BMP, WebP, ICO,
// TGA, ...) comes through QImageReader, GUI-side only (rasterloom-cli stays core-only).
//
// Everything here produces 8-bit straight-alpha RGBA (rl::io::RgbaBuffer, canonicalised), which the
// GUI hands to the core as a place_image op (docs/math/60-editing-ops.md §14) or, for Open, as a
// single-layer document.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QPoint>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

#include "core/doc/document.hpp"
#include "core/io/png.hpp"

class QMimeData;

namespace rl::gui {

struct ImportedImage {
    rl::io::RgbaBuffer img;
    QString name;          // layer name for a placed file: the file name without its extension
    QString format;        // "png", "jpeg", "gif", ...
    QStringList warnings;  // e.g. "only the first of 12 frames"
    QString error;         // empty on success
    bool ok() const { return error.isEmpty() && img.w > 0 && img.h > 0; }
};

// Lower-case file suffixes QImageReader can read here (plugins present at run time).
QStringList qt_image_suffixes();
// True when the extension is a core format or one of qt_image_suffixes().
bool is_image_path(const QString& path);
// True when the file can probably be decoded: an image extension, or content that a core codec or a
// Qt image reader recognises (extension-less files, wrong extensions).
bool can_read_image(const QString& path);
// "*.png *.jpg ..." for the Open / Place dialogs (core formats first).
QString image_name_filters();

// Decodes any image file: core codecs by content (a layered file gives its composite, as the file
// would be seen), else QImageReader. Multi-frame formats give their first frame (with a warning).
ImportedImage read_image_file(const QString& path);
// QImageReader only, for Open's fallback (a single-layer document).
ImportedImage read_with_qt(const QString& path);

// Export of the flattened document to a format only Qt writes: "webp" (lossless, with alpha) or
// "bmp" (24-bit, flattened onto white like JPEG). Atomic (QSaveFile); false + `err` on failure.
bool write_flat_with_qt(const rl::DocState& state, const QString& path, const QString& format, QString& err);

// QImage (any format) -> canonical straight RGBA8 buffer, and back (Format_RGBA8888, a deep copy).
rl::io::RgbaBuffer from_qimage(const QImage& img);
QImage to_qimage(const rl::io::RgbaBuffer& b);

// The clipboard formats Rasterloom writes besides image/png: where the copied pixels were
// (Paste in Place), as JSON {"x":..,"y":..,"w":..,"h":..}.
inline constexpr const char* kClipboardOriginMime = "application/x-rasterloom-origin";

struct ClipboardImage {
    rl::io::RgbaBuffer img;
    std::optional<QPoint> origin;  // only for pixels Rasterloom copied (and whose size matches)
    QStringList files;             // local image files on the clipboard (copied in a file manager)
};
// Reads the best image a QMimeData offers: image/png bytes (exact), then any other image data Qt
// can convert, then local image file URLs. Returns nullopt when there is nothing image-like.
std::optional<ClipboardImage> image_from_mime(const QMimeData* md);
// Builds the QMimeData for copied pixels at canvas position (x, y).
QMimeData* mime_for_pixels(const rl::io::RgbaBuffer& img, int x, int y);

}  // namespace rl::gui
