// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/file_io.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <algorithm>
#include <exception>

#include "core/io/file_io.hpp"
#include "core/io/io_error.hpp"
#include "gui/image_import.hpp"

namespace rl::gui {

namespace {

QStringList to_qlist(const std::vector<std::string>& v) {
    QStringList l;
    for (const std::string& s : v) l << QString::fromStdString(s);
    return l;
}

QString key_of(rl::io::FileFormat f) {
    switch (f) {
        case rl::io::FileFormat::Png: return QStringLiteral("png");
        case rl::io::FileFormat::Jpeg: return QStringLiteral("jpeg");
        case rl::io::FileFormat::Tiff: return QStringLiteral("tiff");
        case rl::io::FileFormat::Ora: return QStringLiteral("ora");
        case rl::io::FileFormat::Psd: return QStringLiteral("psd");
        case rl::io::FileFormat::Unknown: break;
    }
    return {};
}

}  // namespace

QString format_of(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg")) return QStringLiteral("jpeg");
    if (ext == QLatin1String("tif") || ext == QLatin1String("tiff")) return QStringLiteral("tiff");
    if (ext == QLatin1String("psb")) return QStringLiteral("psd");
    return ext;
}

bool is_native(const QString& format) { return format == QLatin1String("orp") || format == QLatin1String("ora"); }

QString open_filter() {
    // Core formats (layered or exact) first, then every format Qt's image plugins read here.
    QStringList extra;
    for (const QString& e : qt_image_suffixes())
        if (!QStringList{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("tif"),
                         QStringLiteral("tiff"), QStringLiteral("psd"), QStringLiteral("psb"), QStringLiteral("ora")}
                 .contains(e))
            extra << QStringLiteral("*.") + e;
    QString f = QObject::tr("All supported images (%1);;").arg(image_name_filters()) +
                QObject::tr("Rasterloom (*.orp);;OpenRaster (*.ora);;PNG (*.png);;JPEG (*.jpg *.jpeg);;TIFF (*.tif *.tiff);;"
                            "Photoshop (*.psd *.psb)");
    if (!extra.isEmpty()) f += QObject::tr(";;Other images (%1)").arg(extra.join(QLatin1Char(' ')));
    return f + QObject::tr(";;All files (*)");
}

bool can_open_path(const QString& path) { return is_image_path(path); }

LoadResult load_document(const QString& path, size_t history_depth) {
    LoadResult r;
    QElapsedTimer t;
    t.start();
    try {
        // The content decides: a file no core codec recognises goes to Qt's image plugins.
        std::vector<uint8_t> head;
        {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray b = f.read(64);
                head.assign(b.begin(), b.end());
            }
        }
        std::unique_ptr<rl::Document> doc;
        if (!head.empty() && rl::io::sniff_format(head) == rl::io::FileFormat::Unknown) {
            ImportedImage q = read_with_qt(path);
            if (!q.ok()) {
                r.error = q.error.isEmpty() ? QObject::tr("Cannot open %1.").arg(QFileInfo(path).fileName()) : q.error;
                return r;
            }
            r.ms = static_cast<double>(t.nsecsElapsed()) / 1e6;
            r.format = q.format;
            r.warnings = q.warnings;
            rl::DocState s = rl::io::single_layer_document(q.img, "Background");
            doc = std::make_unique<rl::Document>(s.w, s.h, s.bg);
            doc->state() = std::move(s);
        } else {
            rl::io::OpenResult o = rl::io::open_document(path.toStdString());
            r.ms = static_cast<double>(t.nsecsElapsed()) / 1e6;
            if (!o.doc) {
                r.error = QObject::tr("Cannot open %1: the file holds no image.").arg(QFileInfo(path).fileName());
                return r;
            }
            r.format = key_of(o.format);
            r.warnings = to_qlist(o.warnings);
            doc = std::move(o.doc);
        }
        const int w = doc->width(), h = doc->height();
        if (w > kLargeSide || h > kLargeSide)
            r.warnings.prepend(QObject::tr("The image is %1 x %2 px, above the %3 px editing limit: it opens for viewing, "
                                           "painting and export, but operations that resize the canvas refuse sides over %3 px.")
                                   .arg(w)
                                   .arg(h)
                                   .arg(kLargeSide));
        doc->history().clear();
        doc->history().set_depth(history_depth);
        r.doc = std::move(doc);
    } catch (const std::exception& e) {
        r.error = QObject::tr("Cannot open %1: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(e.what()));
    }
    return r;
}

SaveResult save_document(const rl::DocState& state, const QString& path, const SaveOptions& opt) {
    SaveResult r;
    QElapsedTimer t;
    t.start();
    const QString fmt = format_of(path);
    if (fmt == QLatin1String("webp") || fmt == QLatin1String("bmp")) {
        // Flat formats only Qt's image plugins write (GUI-side, like their import).
        QString err;
        if (write_flat_with_qt(state, path, fmt, err)) {
            r.ok = true;
            if (fmt == QLatin1String("bmp")) r.warnings << QObject::tr("BMP has no transparency: the image was flattened onto white.");
        } else {
            r.error = QObject::tr("Could not save %1: %2\n\nThe existing file (if any) was left unchanged.").arg(QFileInfo(path).fileName(), err);
        }
        r.ms = static_cast<double>(t.nsecsElapsed()) / 1e6;
        return r;
    }
    try {
        rl::io::SaveOptions so;
        so.jpeg_quality = std::clamp(opt.jpeg_quality, 1, 100);
        so.force_psb = opt.force_psb;
        r.warnings = to_qlist(rl::io::save_document(state, path.toStdString(), so));
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = QObject::tr("Could not save %1: %2\n\nThe existing file (if any) was left unchanged.")
                      .arg(QFileInfo(path).fileName(), QString::fromUtf8(e.what()));
    }
    r.ms = static_cast<double>(t.nsecsElapsed()) / 1e6;
    return r;
}

}  // namespace rl::gui
