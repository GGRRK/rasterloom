// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/icons.hpp"

#include <QHash>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include <map>

#include "gui/theme.hpp"

namespace rl::gui {

namespace {

// Path data only; wrap() adds the <svg> element with the shared stroke style.
const std::map<QString, const char*>& glyphs() {
    static const std::map<QString, const char*> g = {
        // ---- tools ----
        {"move", R"(<path d="M12 3v18M3 12h18"/><path d="M9.5 5.5 12 3l2.5 2.5M9.5 18.5 12 21l2.5-2.5M5.5 9.5 3 12l2.5 2.5M18.5 9.5 21 12l-2.5 2.5"/>)"},
        {"brush", R"(<path d="M20.5 3.5 11 13"/><path d="M11 13c-1.6-1.6-4.3-.9-5 1.3-.5 1.7-.8 3.6-2.5 4.7 3.4.9 6.9-.2 8.3-2 1-1.3.9-2.8-.8-4Z" fill="#000" fill-opacity=".25"/>)"},
        {"eraser", R"(<path d="M4.5 15.5 13 7a2 2 0 0 1 2.8 0l3.2 3.2a2 2 0 0 1 0 2.8L12.5 19.5h-4Z"/><path d="M9 11l5.5 5.5M8.5 19.5H20"/>)"},
        {"bucket", R"(<path d="M5 10.5 11.5 4l7 7-6.5 6.5a1.5 1.5 0 0 1-2.1 0L5 12.6a1.5 1.5 0 0 1 0-2.1Z"/><path d="M5 11.5h13.5"/><path d="M19.5 15.5s-1.8 2.2-1.8 3.3a1.8 1.8 0 0 0 3.6 0c0-1.1-1.8-3.3-1.8-3.3Z" fill="#000"/>)"},
        {"gradient", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M8 4.5v15M12 4.5v15" stroke-opacity=".75"/><path d="M15.5 4.5v15" stroke-opacity=".45"/><path d="M18.5 4.5v15" stroke-opacity=".2"/>)"},
        {"clone", R"(<circle cx="12" cy="5.5" r="2.5"/><path d="M12 8v4M6 12.5h12a1.5 1.5 0 0 1 1.5 1.5v2.5h-15V14A1.5 1.5 0 0 1 6 12.5ZM6 19.5h12"/>)"},
        {"eyedropper", R"(<path d="M14 6.5 17.5 10"/><path d="M15.2 4.3a2.4 2.4 0 0 1 3.4 0l1.1 1.1a2.4 2.4 0 0 1 0 3.4l-2 2-4.5-4.5Z"/><path d="M13.5 8.8 5.5 16.8 4.5 20.5 8.2 19.5 16.2 11.5"/>)"},
        {"crop", R"(<path d="M7 3v14h14"/><path d="M3 7h14v14"/>)"},
        {"hand", R"(<path d="M8 13V6.5a1.5 1.5 0 0 1 3 0V12M11 11.5V5a1.5 1.5 0 0 1 3 0v6.5M14 11.5V6.5a1.5 1.5 0 0 1 3 0v7.5c0 4-2.5 6.5-6 6.5-2.6 0-4.1-1.2-5.4-3.2L3.8 14.2a1.4 1.4 0 0 1 2.3-1.6L8 15"/>)"},
        {"zoom", R"(<circle cx="10.5" cy="10.5" r="6"/><path d="M15 15l5.5 5.5M8 10.5h5M10.5 8v5"/>)"},
        {"marquee_rect", R"(<rect x="4" y="5" width="16" height="14" rx=".5" stroke-dasharray="2.6 2.2"/>)"},
        {"marquee_ellipse", R"(<ellipse cx="12" cy="12" rx="8.5" ry="7" stroke-dasharray="2.6 2.2"/>)"},
        {"lasso", R"(<path d="M6.5 16C3.5 13.5 3.8 7.8 9 6c5-1.7 11 .4 10.6 4.6-.4 4.6-7.5 6-11.4 5.2" stroke-dasharray="2.6 2.2"/><path d="M8.2 15.8c-.6 1.8.2 3.6 2.3 4.2"/>)"},
        {"polygon_lasso", R"(<path d="M5 17 8 5l11 4-3 10Z" stroke-dasharray="2.6 2.2"/><circle cx="5" cy="17" r="1.3" fill="#000"/><circle cx="8" cy="5" r="1.3" fill="#000"/><circle cx="19" cy="9" r="1.3" fill="#000"/><circle cx="16" cy="19" r="1.3" fill="#000"/>)"},
        {"wand", R"(<path d="M4 20 12.8 11.2"/><path d="M15 3.5v3M15 10.5v3M11.5 7h-3M21.5 7h-3M12.5 4.5l1.3 1.3M17.5 9.5l-1.3-1.3M17.5 4.5l-1.3 1.3"/>)"},
        {"transform", R"(<rect x="5" y="5" width="14" height="14" stroke-dasharray="2.6 2.2"/><rect x="3" y="3" width="4" height="4" fill="#fff"/><rect x="17" y="3" width="4" height="4" fill="#fff"/><rect x="3" y="17" width="4" height="4" fill="#fff"/><rect x="17" y="17" width="4" height="4" fill="#fff"/>)"},
        // ---- panels and actions ----
        {"eye", R"(<path d="M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12Z"/><circle cx="12" cy="12" r="3"/>)"},
        {"eye_off", R"(<path d="M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12Z" stroke-opacity=".35"/><path d="M4 20 20 4"/>)"},
        {"lock", R"(<rect x="5.5" y="10.5" width="13" height="9.5" rx="1.5"/><path d="M8.5 10.5V8a3.5 3.5 0 0 1 7 0v2.5"/>)"},
        {"plus", R"(<rect x="4" y="4" width="16" height="16" rx="2"/><path d="M12 8v8M8 12h8"/>)"},
        {"trash", R"(<path d="M4.5 6.5h15M9.5 6.5V4.5h5v2M6.5 6.5l1 13h9l1-13M10.5 10v6M13.5 10v6"/>)"},
        {"folder", R"(<path d="M3.5 7a1.5 1.5 0 0 1 1.5-1.5h4.2l2 2H19A1.5 1.5 0 0 1 20.5 9v8.5A1.5 1.5 0 0 1 19 19H5a1.5 1.5 0 0 1-1.5-1.5Z"/>)"},
        {"mask", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><circle cx="12" cy="12" r="4.5" fill="#000" fill-opacity=".35"/>)"},
        {"adjust", R"(<circle cx="12" cy="12" r="8"/><path d="M12 4a8 8 0 0 1 0 16Z" fill="#000"/>)"},
        {"merge", R"(<path d="M6 4.5v4a3.5 3.5 0 0 0 3.5 3.5h5A3.5 3.5 0 0 0 18 8.5v-4M12 12v7M9 16.5l3 3 3-3"/>)"},
        {"clip", R"(<path d="M7 4v8.5a3 3 0 0 0 3 3h9"/><path d="M16 12.5l3 3-3 3"/>)"},
        {"undo", R"(<path d="M9 7.5 4.5 12 9 16.5"/><path d="M4.5 12H15a4.5 4.5 0 0 1 0 9h-2"/>)"},
        {"redo", R"(<path d="M15 7.5 19.5 12 15 16.5"/><path d="M19.5 12H9a4.5 4.5 0 0 0 0 9h2"/>)"},
        {"swap", R"(<path d="M7 5h8.5A3.5 3.5 0 0 1 19 8.5V17"/><path d="M16 14l3 3 3-3M10 2l-3 3 3 3"/>)"},
        {"layer", R"(<path d="M12 4 3.5 8.5 12 13l8.5-4.5Z"/><path d="M3.5 12.5 12 17l8.5-4.5" stroke-opacity=".6"/><path d="M3.5 16.5 12 21l8.5-4.5" stroke-opacity=".35"/>)"},
        {"layers", R"(<path d="M12 4 3.5 8.5 12 13l8.5-4.5Z"/><path d="M3.5 12.5 12 17l8.5-4.5"/><path d="M3.5 16.5 12 21l8.5-4.5"/>)"},
        {"history", R"(<path d="M4.5 12a7.5 7.5 0 1 0 2.2-5.3L4.5 9"/><path d="M4.5 4.5V9H9M12 8v4.5l3 2"/>)"},
        {"color", R"(<circle cx="9" cy="9.5" r="5.5"/><circle cx="15" cy="9.5" r="5.5" stroke-opacity=".6"/><circle cx="12" cy="15" r="5.5" stroke-opacity=".35"/>)"},
        {"navigator", R"(<rect x="3.5" y="5" width="17" height="14" rx="1.5"/><rect x="8" y="8.5" width="7" height="5.5" fill="#000" fill-opacity=".3"/>)"},
        {"fit", R"(<path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/>)"},
        {"actual", R"(<path d="M7 17V7l-2 1.5M16 17V7l-2 1.5"/><circle cx="11.5" cy="10" r=".8" fill="#000"/><circle cx="11.5" cy="14" r=".8" fill="#000"/>)"},
        {"info", R"(<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5.5"/><circle cx="12" cy="7.8" r=".9" fill="#000"/>)"},
        {"check", R"(<path d="M5 12.5 10 17.5 19.5 7"/>)"},
        {"cancel", R"(<path d="M6 6l12 12M18 6 6 18"/>)"},
        {"new_doc", R"(<path d="M6.5 3.5h7l4 4v13h-11Z"/><path d="M13.5 3.5v4h4M12 11v6M9 14h6"/>)"},
        {"open", R"(<path d="M3.5 7a1.5 1.5 0 0 1 1.5-1.5h4.2l2 2H18A1.5 1.5 0 0 1 19.5 9v1.5"/><path d="M3.5 18.5 6 11h15.5L19 18.5Z"/>)"},
        {"save", R"(<path d="M5 3.5h11.5l3 3V19A1.5 1.5 0 0 1 18 20.5H6A1.5 1.5 0 0 1 4.5 19V5A1.5 1.5 0 0 1 6 3.5Z"/><path d="M8 3.5v5h7.5v-5M8 20.5v-6h8v6"/>)"},
    };
    return g;
}

QByteArray wrap(const char* body) {
    QByteArray s(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">)");
    s += body;
    s += "</svg>";
    return s;
}

// Renders `svg` at `size` device pixels and recolours every painted pixel to `c` (alpha kept).
QPixmap render_tinted(const QByteArray& svg, QSize size, qreal dpr, const QColor& c) {
    QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QSvgRenderer r(svg);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        r.render(&p);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), c);
    }
    img.setDevicePixelRatio(dpr);
    return QPixmap::fromImage(img);
}

class TintedSvgEngine : public QIconEngine {
public:
    explicit TintedSvgEngine(QByteArray svg) : svg_(std::move(svg)) {}
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        p->drawPixmap(rect, pixmap_dpr(rect.size(), mode, state, dpr));
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return pixmap_dpr(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        return pixmap_dpr(size, mode, state, scale);
    }
    QIconEngine* clone() const override { return new TintedSvgEngine(svg_); }
    QString key() const override { return QStringLiteral("rl-tinted-svg"); }

private:
    QPixmap pixmap_dpr(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal dpr) {
        QColor c = theme::kText;
        if (mode == QIcon::Disabled) c = QColor(0x5a, 0x5e, 0x65);
        else if (mode == QIcon::Selected || state == QIcon::On) c = Qt::white;
        const QString k = QStringLiteral("%1x%2@%3/%4").arg(size.width()).arg(size.height()).arg(dpr).arg(c.rgba());
        auto it = cache_.find(k);
        if (it != cache_.end()) return *it;
        QPixmap pm = render_tinted(svg_, size, dpr, c);
        cache_.insert(k, pm);
        return pm;
    }
    QByteArray svg_;
    QHash<QString, QPixmap> cache_;
};

}  // namespace

QIcon icon(const QString& name) {
    static QHash<QString, QIcon> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return *it;
    const auto& g = glyphs();
    auto gi = g.find(name);
    const char* body = gi != g.end() ? gi->second
                                     : R"(<rect x="4" y="4" width="16" height="16"/><path d="M4 4l16 16M20 4 4 20"/>)";
    QIcon ic(new TintedSvgEngine(wrap(body)));
    cache.insert(name, ic);
    return ic;
}

QStringList icon_names() {
    QStringList out;
    for (const auto& [k, v] : glyphs()) out << k;
    return out;
}

}  // namespace rl::gui
