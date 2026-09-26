// SPDX-License-Identifier: GPL-3.0-or-later
//
// MainWindow: getting images in and out, the Photoshop way.
//
//   Edit > Cut / Copy / Copy Merged (Ctrl+X / Ctrl+C / Shift+Ctrl+C): COPY of docs/math/60 §14.4,
//     cropped to its content box (§14.4.1), onto the system clipboard as image/png + QImage (other
//     applications paste it) + the copied pixels' canvas position (Paste in Place). Cut then records
//     one `clear` op.
//   Edit > Paste / Paste in Place (Ctrl+V / Shift+Ctrl+V): whatever image the clipboard holds
//     (Rasterloom's own, a browser's "Copy image", a screenshot tool's) becomes ONE place_image op
//     that carries the pixels, directly above the active layer: centred on the selection or on the
//     visible canvas, or at the copied position.
//   Edit > Clear (Delete): one `clear` op through the selection.
//   Layer > Layer via Copy / Layer via Cut (Ctrl+J with a selection / Shift+Ctrl+J): layer_via_copy.
//   File > Place... and dropping image files / image data on a document: place_image per image,
//     centred on the canvas, named after the file.
//   With the untouched start-up document (Photoshop's "no document open"), a paste or drop opens the
//   image as its own document instead (the first of several dropped files; the rest are placed).
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QSettings>

#include <algorithm>
#include <cmath>

#include "core/edit/place.hpp"
#include "core/edit/png_payload.hpp"
#include "core/select/selection_ops.hpp"
#include "gui/canvas.hpp"
#include "gui/image_import.hpp"
#include "gui/json_util.hpp"
#include "gui/main_window.hpp"
#include "gui/session.hpp"

namespace rl::gui {

namespace {

QSettings settings_for_import() { return QSettings(QStringLiteral("Rasterloom"), QStringLiteral("Rasterloom")); }

QString short_list(const QStringList& paths) {
    QStringList names;
    for (const QString& p : paths) names << QFileInfo(p).fileName();
    if (names.size() > 3) return names.mid(0, 3).join(QStringLiteral(", ")) + QObject::tr(" and %1 more").arg(names.size() - 3);
    return names.join(QStringLiteral(", "));
}

}  // namespace

Json MainWindow::placement_fields() const {
    Json j = Json::object();
    const rl::Node* n = session_->active_node();
    if (!n) return j;
    if (n->is_group()) j["parent"] = n->id;  // a selected group receives the new layer at its top
    else j["above"] = n->id;                  // Photoshop pastes directly above the active layer
    return j;
}

QPoint MainWindow::paste_position(int w, int h) const {
    const rl::Document& d = session_->doc();
    QPointF c(d.width() / 2.0, d.height() / 2.0);
    if (session_->state().selection.active()) {
        // Bounds of the selected (non-zero) pixels. Not content_bbox(): that measures against the
        // mask's background value, which is 255 after Select All.
        const std::vector<uint8_t> m = rl::select::mask_dense(session_->state().selection);
        const int W = d.width(), H = d.height();
        int x0 = W, y0 = H, x1 = -1, y1 = -1;
        for (int y = 0; y < H; ++y) {
            const uint8_t* row = m.data() + (static_cast<size_t>(y) * static_cast<size_t>(W));
            for (int x = 0; x < W; ++x)
                if (row[x] != 0) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
        }
        if (x1 >= 0) c = QPointF(x0 + ((x1 - x0 + 1) / 2.0), y0 + ((y1 - y0 + 1) / 2.0));
    } else {
        const QRectF v = canvas_->visible_canvas_rect();
        if (!v.isEmpty()) c = v.center();
    }
    const auto clampi = [](double v) { return static_cast<int>(std::clamp(std::floor(v), -32768.0, 32768.0)); };
    return QPoint(clampi(c.x() - (w / 2.0)), clampi(c.y() - (h / 2.0)));
}

bool MainWindow::place_pixels(const rl::io::RgbaBuffer& img, const QString& name, std::optional<QPoint> at, const QString& label) {
    if (img.w < 1 || img.h < 1) return false;
    if (img.w > rl::kMaxCanvasSide || img.h > rl::kMaxCanvasSide) {
        show_message(tr("%1: the image is %2 x %3 px; Rasterloom documents are at most %4 px per side.")
                         .arg(label)
                         .arg(img.w)
                         .arg(img.h)
                         .arg(rl::kMaxCanvasSide),
                     true);
        return false;
    }
    canvas_->commit_stroke_if_any();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::string payload = rl::edit::encode_payload(img);
    QApplication::restoreOverrideCursor();
    const std::string layer_name = to_std(name);

    if (!session_->has_document() || session_->is_placeholder()) {
        // No document yet: the image becomes one, sized to it (Photoshop: New from Clipboard + Paste).
        Json op = {{"op", "place_image"}, {"id", "Layer 1"}, {"png", payload}};
        if (!layer_name.empty()) op["name"] = layer_name;
        if (!maybe_save(label)) return false;
        if (!session_->new_document_from_op(img.w, img.h, op, "Layer 1", label).ok()) return false;
        update_title();
        update_actions();
        show_message(tr("%1: new document %2 x %3 px").arg(label).arg(img.w).arg(img.h), false);
        return true;
    }

    const std::string id = session_->unique_id("Layer");
    Json op = {{"op", "place_image"}, {"id", id}, {"png", payload}};
    if (at) {
        op["x"] = at->x();
        op["y"] = at->y();
    }
    if (!layer_name.empty()) op["name"] = layer_name;
    const Json where = placement_fields();
    if (where.contains("above")) op["above"] = where["above"];
    if (where.contains("parent")) op["parent"] = where["parent"];
    if (!session_->apply(op, label).ok()) return false;
    session_->set_active_layer(id);
    const rl::Document& d = session_->doc();
    const int x = at ? at->x() : static_cast<int>(rl::edit::floor_half(static_cast<int64_t>(d.width()) - img.w));
    const int y = at ? at->y() : static_cast<int>(rl::edit::floor_half(static_cast<int64_t>(d.height()) - img.h));
    if (x < 0 || y < 0 || x + img.w > d.width() || y + img.h > d.height())
        show_message(tr("%1: %2 x %3 px, partly outside the %4 x %5 canvas - the part outside was not kept.")
                         .arg(label)
                         .arg(img.w)
                         .arg(img.h)
                         .arg(d.width())
                         .arg(d.height()),
                     true);
    else
        show_message(tr("%1: %2 x %3 px as a new layer").arg(label).arg(img.w).arg(img.h), false);
    return true;
}

// ---- clipboard --------------------------------------------------------------------------------------------------

void MainWindow::copy_selection(bool merged, bool cut) {
    if (!require_doc()) return;
    canvas_->commit_stroke_if_any();
    const QString what = cut ? tr("Cut") : merged ? tr("Copy Merged") : tr("Copy");
    std::string layer;
    if (!merged) {
        if (!require_raster(what)) return;
        layer = session_->active_layer();
        if (cut && session_->active_node()->lock_alpha) {
            show_message(tr("Cut: the transparency of %1 is locked, so its pixels cannot be removed (Layer > Lock Transparency).")
                             .arg(layer_display_name(*session_->active_node())),
                         true);
            return;
        }
    }
    std::optional<rl::edit::Copied> c;
    try {
        c = rl::edit::copy_region(session_->state(), layer);
    } catch (const std::exception& e) {
        show_message(what + QStringLiteral(": ") + QString::fromUtf8(e.what()), true);
        return;
    }
    if (!c) {
        show_message(tr("Could not %1: the selected area is empty.").arg(what.toLower()), true);
        return;
    }
    QGuiApplication::clipboard()->setMimeData(mime_for_pixels(c->pixels, c->x, c->y));
    if (cut && !session_->apply({{"op", "clear"}, {"layer", layer}}, tr("Cut")).ok()) return;
    show_message(tr("%1: %2 x %3 px on the clipboard").arg(what).arg(c->pixels.w).arg(c->pixels.h), false);
}

void MainWindow::paste(bool in_place) {
    const QString label = in_place ? tr("Paste in Place") : tr("Paste");
    const std::optional<ClipboardImage> ci = image_from_mime(QGuiApplication::clipboard()->mimeData());
    if (!ci) {
        show_message(tr("Nothing to paste: the clipboard holds no image."), true);
        return;
    }
    if (ci->img.w == 0) {  // image files copied in a file manager: place them like a drop
        handle_drop(ci->files, {}, std::nullopt);
        return;
    }
    std::optional<QPoint> at;
    if (session_->has_document() && !session_->is_placeholder())
        at = (in_place && ci->origin) ? *ci->origin : paste_position(ci->img.w, ci->img.h);
    place_pixels(ci->img, QString(), at, label);
}

void MainWindow::clear_selected() {
    if (!require_doc() || !require_raster(tr("Clear"))) return;
    if (!session_->state().selection.active()) {
        show_message(tr("Clear deletes the selected pixels: make a selection first (Select > All for the whole layer)."), true);
        return;
    }
    if (session_->active_node()->lock_alpha) {
        show_message(tr("Clear: the transparency of %1 is locked (Layer > Lock Transparency).").arg(layer_display_name(*session_->active_node())),
                     true);
        return;
    }
    session_->apply({{"op", "clear"}, {"layer", session_->active_layer()}}, tr("Clear"));
}

void MainWindow::layer_via(bool cut) {
    if (!require_doc()) return;
    const QString what = cut ? tr("Layer via Cut") : tr("Layer via Copy");
    if (!cut && (!session_->state().selection.active() || !session_->active_node() || !session_->active_node()->is_raster())) {
        duplicate_layer();  // Ctrl+J without a selection duplicates the layer (Photoshop)
        return;
    }
    if (!require_raster(what)) return;
    const rl::Node& src = *session_->active_node();
    if (cut && src.lock_alpha) {
        show_message(tr("%1: the transparency of %2 is locked (Layer > Lock Transparency).").arg(what, layer_display_name(src)), true);
        return;
    }
    const std::string id = session_->unique_id("Layer");
    Json op = {{"op", "layer_via_copy"}, {"layer", src.id}, {"id", id}};
    if (cut) op["cut"] = true;
    if (session_->apply(op, what).ok()) session_->set_active_layer(id);
}

// ---- files ------------------------------------------------------------------------------------------------------

void MainWindow::place_dialog() {
    QSettings s = settings_for_import();
    const QString dir = s.value(QStringLiteral("file/last_dir")).toString();
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Place"), dir,
                                                            tr("Images (%1);;All files (*)").arg(image_name_filters()));
    if (paths.isEmpty()) return;
    s.setValue(QStringLiteral("file/last_dir"), QFileInfo(paths.front()).absolutePath());
    for (const QString& p : paths) place_file(p);
}

bool MainWindow::place_file(const QString& path) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const ImportedImage im = read_image_file(path);
    QApplication::restoreOverrideCursor();
    if (!im.ok()) {
        show_message(im.error, true);
        return false;
    }
    if (!place_pixels(im.img, im.name, std::nullopt, tr("Place %1").arg(QFileInfo(path).fileName()))) return false;
    if (!im.warnings.isEmpty()) show_message(im.warnings.join(QLatin1Char(' ')), true);
    return true;
}

void MainWindow::handle_drop(const QStringList& files, const QStringList& remote, const std::optional<rl::io::RgbaBuffer>& data) {
    QStringList ok, bad;
    for (const QString& f : files) (can_read_image(f) ? ok : bad) << f;
    if (!ok.isEmpty()) {
        int placed = 0;
        int first = 0;
        if (!session_->has_document() || session_->is_placeholder()) {
            // The first file that opens becomes the document; the others are placed into it.
            while (first < ok.size() && !open_file(ok[first])) ++first;
            if (first == ok.size()) return;
            ++first;
        }
        for (int i = first; i < ok.size(); ++i) placed += place_file(ok[i]) ? 1 : 0;
        if (!bad.isEmpty())
            show_message(tr("Skipped %1: not an image format Rasterloom reads.").arg(short_list(bad)), true);
        else if (placed > 1)
            show_message(tr("Placed %1 images as new layers.").arg(placed), false);
        return;
    }
    if (data) {
        place_pixels(*data, QString(), std::nullopt, tr("Drop Image"));
        return;
    }
    if (!remote.isEmpty()) {
        show_message(tr("%1 is a web address and Rasterloom does not download files: save the image, then drop or open the saved file.")
                         .arg(remote.front().left(80)),
                     true);
        return;
    }
    if (!bad.isEmpty()) {
        show_message(tr("Cannot open %1: not an image format Rasterloom reads (PNG, JPEG, TIFF, PSD/PSB, OpenRaster, %2).")
                         .arg(short_list(bad), qt_image_suffixes().join(QStringLiteral(", ")).toUpper()),
                     true);
        return;
    }
    show_message(tr("Nothing to open in this drop: drop image files or an image."), true);
}

}  // namespace rl::gui
