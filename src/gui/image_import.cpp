// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/image_import.hpp"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QObject>
#include <QPoint>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>
#include <exception>

#include "core/base/quant.hpp"
#include "core/composite/render.hpp"
#include "core/geometry/dense.hpp"
#include "core/io/file_io.hpp"
#include "core/io/image_util.hpp"
#include "core/io/layer_name.hpp"

namespace rl::gui {

namespace {

const QStringList& core_suffixes() {
    static const QStringList k = {QStringLiteral("orp"), QStringLiteral("ora"), QStringLiteral("png"), QStringLiteral("jpg"),
                                  QStringLiteral("jpeg"), QStringLiteral("jpe"), QStringLiteral("tif"), QStringLiteral("tiff"),
                                  QStringLiteral("psd"), QStringLiteral("psb")};
    return k;
}

void allow_large_images() {
    // Qt refuses to decode images above 256 MB by default; the editor's limit is 16384 x 16384
    // RGBA (1 GiB). Set once, before the first QImageReader.
    static const bool once = [] {
        QImageReader::setAllocationLimit(1100);
        return true;
    }();
    (void)once;
}

QString layer_name_for(const QString& path) {
    const std::string raw = QFileInfo(path).completeBaseName().toStdString();
    return QString::fromStdString(rl::io::sanitize_layer_name(raw).name);
}

rl::io::RgbaBuffer composite_of(const rl::DocState& s) {
    rl::io::RgbaBuffer b;
    b.w = s.w;
    b.h = s.h;
    b.px = rl::geom::to_dense(rl::composite::render_image(s, true));
    return b;
}

QByteArray read_bytes(const QString& path, QString& err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        err = f.errorString();
        return {};
    }
    return f.readAll();
}

}  // namespace

QStringList qt_image_suffixes() {
    static const QStringList list = [] {
        QStringList l;
        for (const QByteArray& f : QImageReader::supportedImageFormats()) l << QString::fromLatin1(f).toLower();
        l.removeDuplicates();
        l.sort();
        return l;
    }();
    return list;
}

bool is_image_path(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    return !ext.isEmpty() && (core_suffixes().contains(ext) || qt_image_suffixes().contains(ext));
}

bool can_read_image(const QString& path) {
    if (is_image_path(path)) return true;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(64);
    if (rl::io::sniff_format(std::vector<uint8_t>(head.begin(), head.end())) != rl::io::FileFormat::Unknown) return true;
    f.close();
    return !QImageReader::imageFormat(path).isEmpty();
}

QString image_name_filters() {
    QStringList pats;
    for (const QString& s : core_suffixes()) pats << QStringLiteral("*.") + s;
    for (const QString& s : qt_image_suffixes())
        if (!core_suffixes().contains(s)) pats << QStringLiteral("*.") + s;
    return pats.join(QLatin1Char(' '));
}

rl::io::RgbaBuffer from_qimage(const QImage& src) {
    rl::io::RgbaBuffer b;
    if (src.isNull()) return b;
    const QImage img = src.convertToFormat(QImage::Format_RGBA8888);  // straight (unpremultiplied) alpha
    b.w = img.width();
    b.h = img.height();
    b.px.resize(static_cast<size_t>(b.w) * static_cast<size_t>(b.h));
    for (int y = 0; y < b.h; ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < b.w; ++x) {
            const uchar* p = row + (static_cast<size_t>(x) * 4);
            b.at(x, y) = rl::canonicalize(rl::Rgba8{p[0], p[1], p[2], p[3]});
        }
    }
    return b;
}

QImage to_qimage(const rl::io::RgbaBuffer& b) {
    QImage img(b.w, b.h, QImage::Format_RGBA8888);
    for (int y = 0; y < b.h; ++y) {
        uchar* row = img.scanLine(y);
        for (int x = 0; x < b.w; ++x) {
            const rl::Rgba8 p = b.at(x, y);
            uchar* d = row + (static_cast<size_t>(x) * 4);
            d[0] = p.r;
            d[1] = p.g;
            d[2] = p.b;
            d[3] = p.a;
        }
    }
    return img;
}

ImportedImage read_with_qt(const QString& path) {
    allow_large_images();
    ImportedImage r;
    r.name = layer_name_for(path);
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);  // EXIF orientation, as viewers show the photo
    r.format = QString::fromLatin1(reader.format()).toLower();
    const int frames = reader.imageCount();
    QImage img;
    if (!reader.read(&img) || img.isNull()) {
        const QString ext = QFileInfo(path).suffix().toLower();
        r.error = reader.error() == QImageReader::UnsupportedFormatError
                      ? QObject::tr("%1 is not an image format Rasterloom can read (%2).")
                            .arg(QFileInfo(path).fileName(), ext.isEmpty() ? QObject::tr("no extension") : QStringLiteral(".") + ext)
                      : QObject::tr("Cannot read %1: %2").arg(QFileInfo(path).fileName(), reader.errorString());
        return r;
    }
    if (img.width() > 16384 || img.height() > 16384) {
        r.error = QObject::tr("%1 is %2 x %3 px: Rasterloom documents are at most 16384 px per side.")
                      .arg(QFileInfo(path).fileName())
                      .arg(img.width())
                      .arg(img.height());
        return r;
    }
    if (frames > 1)
        r.warnings << QObject::tr("%1 has %2 frames; only the first frame was opened (animation is not supported).")
                          .arg(QFileInfo(path).fileName())
                          .arg(frames);
    if (img.depth() > 32)
        r.warnings << QObject::tr("%1 has more than 8 bits per channel; it was converted to 8-bit.").arg(QFileInfo(path).fileName());
    r.img = from_qimage(img);
    return r;
}

ImportedImage read_image_file(const QString& path) {
    ImportedImage r;
    r.name = layer_name_for(path);
    QString err;
    const QByteArray bytes = read_bytes(path, err);
    if (!err.isEmpty()) {
        r.error = QObject::tr("Cannot read %1: %2").arg(QFileInfo(path).fileName(), err);
        return r;
    }
    const std::vector<uint8_t> v(bytes.begin(), bytes.end());
    if (rl::io::sniff_format(v) == rl::io::FileFormat::Unknown) {
        ImportedImage q = read_with_qt(path);
        q.name = r.name;
        return q;
    }
    try {
        rl::io::FileFormat f = rl::io::FileFormat::Unknown;
        std::vector<std::string> w;
        const rl::DocState s = rl::io::decode_document(v, f, w);
        r.format = QString::fromLatin1(rl::io::format_name(f)).toLower();
        for (const std::string& x : w) r.warnings << QString::fromStdString(x);
        if (s.w > 16384 || s.h > 16384) {
            r.error = QObject::tr("%1 is %2 x %3 px: placed images are at most 16384 px per side.")
                          .arg(QFileInfo(path).fileName())
                          .arg(s.w)
                          .arg(s.h);
            return r;
        }
        r.img = composite_of(s);
    } catch (const std::exception& e) {
        r.error = QObject::tr("Cannot read %1: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(e.what()));
    }
    return r;
}

bool write_flat_with_qt(const rl::DocState& state, const QString& path, const QString& format, QString& err) {
    rl::io::RgbaBuffer b = composite_of(state);
    const bool bmp = format == QLatin1String("bmp");
    if (bmp)
        for (rl::Rgba8& p : b.px) p = rl::io::matte_white(p);
    QImage img = to_qimage(b);
    if (bmp) img = img.convertToFormat(QImage::Format_RGB888);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        err = f.errorString();
        return false;
    }
    QImageWriter w(&f, format.toLatin1());
    if (format == QLatin1String("webp")) w.setQuality(100);  // Qt's WebP writer: 100 = lossless
    if (!w.write(img)) {
        err = w.errorString();
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        err = f.errorString();
        return false;
    }
    return true;
}

std::optional<ClipboardImage> image_from_mime(const QMimeData* md) {
    if (!md) return std::nullopt;
    ClipboardImage c;
    bool have = false;
    if (md->hasFormat(QStringLiteral("image/png"))) {
        const QByteArray png = md->data(QStringLiteral("image/png"));
        try {
            c.img = rl::io::decode_png(std::vector<uint8_t>(png.begin(), png.end()));
            for (rl::Rgba8& p : c.img.px) p = rl::canonicalize(p);
            have = c.img.w > 0;
        } catch (const std::exception&) {
            have = false;
        }
    }
    if (!have && md->hasImage()) {
        c.img = from_qimage(qvariant_cast<QImage>(md->imageData()));
        have = c.img.w > 0;
    }
    if (!have) {
        // Other image/* payloads (image/jpeg, image/webp, image/bmp ...) through Qt's readers.
        for (const QString& f : md->formats()) {
            if (!f.startsWith(QLatin1String("image/"))) continue;
            QByteArray data = md->data(f);
            QBuffer buf(&data);
            buf.open(QIODevice::ReadOnly);
            allow_large_images();
            QImageReader reader(&buf);
            QImage img;
            if (reader.read(&img) && !img.isNull()) {
                c.img = from_qimage(img);
                have = true;
                break;
            }
        }
    }
    if (have) {
        if (md->hasFormat(QLatin1String(kClipboardOriginMime))) {
            const QJsonObject o = QJsonDocument::fromJson(md->data(QLatin1String(kClipboardOriginMime))).object();
            if (o.value(QStringLiteral("w")).toInt() == c.img.w && o.value(QStringLiteral("h")).toInt() == c.img.h)
                c.origin = QPoint(o.value(QStringLiteral("x")).toInt(), o.value(QStringLiteral("y")).toInt());
        }
        return c;
    }
    if (md->hasUrls()) {
        for (const QUrl& u : md->urls())
            if (u.isLocalFile() && is_image_path(u.toLocalFile())) c.files << u.toLocalFile();
        if (!c.files.isEmpty()) return c;
    }
    return std::nullopt;
}

QMimeData* mime_for_pixels(const rl::io::RgbaBuffer& img, int x, int y) {
    auto* md = new QMimeData;
    const std::vector<uint8_t> png = rl::io::encode_png(img);
    md->setData(QStringLiteral("image/png"), QByteArray(reinterpret_cast<const char*>(png.data()), static_cast<qsizetype>(png.size())));
    md->setImageData(to_qimage(img));
    QJsonObject o;
    o.insert(QStringLiteral("x"), x);
    o.insert(QStringLiteral("y"), y);
    o.insert(QStringLiteral("w"), img.w);
    o.insert(QStringLiteral("h"), img.h);
    md->setData(QLatin1String(kClipboardOriginMime), QJsonDocument(o).toJson(QJsonDocument::Compact));
    return md;
}

}  // namespace rl::gui
