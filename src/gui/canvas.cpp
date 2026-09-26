// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/canvas.hpp"

#include <QCursor>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTransform>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/composite/render.hpp"
#include "core/geometry/gui_api.hpp"
#include "gui/diagnostics.hpp"
#include "gui/json_util.hpp"
#include "gui/session.hpp"
#include "gui/theme.hpp"

namespace rl::gui {

namespace {

constexpr double kZoomSteps[] = {0.02, 0.03, 0.05, 1.0 / 12, 0.125, 1.0 / 6, 0.25, 1.0 / 3, 0.5, 2.0 / 3,
                                 1.0,  1.5,  2.0,  3.0,      4.0,   5.0,     6.0,  8.0,     12.0, 16.0,
                                 24.0, 32.0, 48.0, 64.0};
constexpr double kMinZoom = 0.02;
constexpr double kMaxZoom = 64.0;
constexpr double kFrameBudgetMs = 16.0;  // tile rendering per painted frame (keeps frames near 60 Hz)

double rhu(double v) { return std::floor(v + 0.5); }  // doc 40 round-half-up

const QPixmap& checker_pixmap() {
    static QPixmap pm = [] {
        const int c = theme::kCheckerCell;
        QPixmap p(2 * c, 2 * c);
        QPainter g(&p);
        g.fillRect(0, 0, 2 * c, 2 * c, theme::kChecker1);
        g.fillRect(0, 0, c, c, theme::kChecker2);
        g.fillRect(c, c, c, c, theme::kChecker2);
        return p;
    }();
    return pm;
}

QString tool_label(Tool t) { return QString::fromLatin1(tool_info(t).name); }

// A small readable caption on a dark pill, above `anchor` (clamped into the widget).
void draw_label(QPainter& p, const QPointF& anchor, const QString& text, const QRect& bounds) {
    const QFontMetrics fm(p.font());
    QRectF r(0, 0, fm.horizontalAdvance(text) + 14, fm.height() + 6);
    r.moveBottomLeft(anchor + QPointF(0, -6));
    if (r.top() < bounds.top() + 4) r.moveTop(bounds.top() + 4);
    if (r.left() < bounds.left() + 4) r.moveLeft(bounds.left() + 4);
    if (r.right() > bounds.right() - 4) r.moveRight(bounds.right() - 4);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 21, 24, 215));
    p.drawRoundedRect(r, 4, 4);
    p.setPen(QColor(0xdc, 0xde, 0xe2));
    p.drawText(r, Qt::AlignCenter, text);
    p.restore();
}

}  // namespace

CanvasController::CanvasController(EditorSession* session, ToolState* tools, QObject* parent)
    : QObject(parent), session_(session), tools_(tools) {
    connect(session_, &EditorSession::document_reset, this, &CanvasController::on_document_reset);
    connect(session_, &EditorSession::tiles_dirty, this, &CanvasController::on_tiles_dirty);
    connect(session_, &EditorSession::selection_changed, this, &CanvasController::on_selection_changed);
    connect(tools_, &ToolState::tool_changed, this, &CanvasController::on_tool_changed);
    connect(tools_, &ToolState::settings_changed, this, [this] { request_update(); });
    ants_timer_.setInterval(140);
    connect(&ants_timer_, &QTimer::timeout, this, [this] {
        ants_phase_ = (ants_phase_ + 1) % 8;
        request_update();
    });
    airbrush_timer_.setInterval(16);  // doc 40 §5: a synthetic sample every 16 ms of wall time
    connect(&airbrush_timer_, &QTimer::timeout, this, &CanvasController::on_airbrush_tick);
    // Nothing may restructure the document under a live StrokeSession: any other edit commits the
    // stroke first.
    session_->set_edit_guard([this] {
        finish_stroke();
        if (xf_.active) cancel_pending();  // an edit (undo, a menu op) abandons an unapplied transform
    });
    xf_timer_.setSingleShot(true);
    xf_timer_.setInterval(0);
    connect(&xf_timer_, &QTimer::timeout, this, [this] { update_xform_preview(!xf_dragging_); });
}

CanvasController::~CanvasController() = default;

void CanvasController::set_widget(QWidget* w) {
    widget_ = w;
    if (widget_) {
        update_cursor();
        request_update();
    }
}

void CanvasController::request_update() {
    if (widget_) widget_->update();
}

void CanvasController::request_update_canvas_rect(const QRectF& r) {
    if (!widget_) return;
    const QRectF w(to_widget(r.topLeft()), to_widget(r.bottomRight()));
    widget_->update(w.toAlignedRect().adjusted(-3, -3, 3, 3));
}

// ---- view -------------------------------------------------------------------------------------------

void CanvasController::set_zoom(double z, const QPointF& anchor) {
    z = std::clamp(z, kMinZoom, kMaxZoom);
    const QPointF c = to_canvas(anchor);
    zoom_ = z;
    off_ = anchor - c * zoom_;
    off_ = QPointF(std::round(off_.x()), std::round(off_.y()));  // integral: tiles and checker align
    fitted_ = false;
    clamp_offset();
    emit view_changed();
    request_update();
}

void CanvasController::zoom_in() {
    const QPointF anchor = widget_ ? QPointF(widget_->width() / 2.0, widget_->height() / 2.0) : QPointF();
    for (double s : kZoomSteps)
        if (s > zoom_ * 1.0001) return set_zoom(s, anchor);
}

void CanvasController::zoom_out() {
    const QPointF anchor = widget_ ? QPointF(widget_->width() / 2.0, widget_->height() / 2.0) : QPointF();
    for (auto it = std::rbegin(kZoomSteps); it != std::rend(kZoomSteps); ++it)
        if (*it < zoom_ * 0.9999) return set_zoom(*it, anchor);
}

void CanvasController::zoom_to_widget_rect(const QRectF& r) {
    if (!widget_ || r.isEmpty()) return;
    // The dragged area fills the view (the smaller of the two ratios, so all of it stays visible),
    // centred; the zoom is exact, not snapped to a preset step, as Photoshop's marquee zoom.
    const QPointF centre_c = to_canvas(r.center());
    const double z = std::clamp(zoom_ * std::min(widget_->width() / std::max(1.0, r.width()), widget_->height() / std::max(1.0, r.height())),
                                kMinZoom, kMaxZoom);
    zoom_ = z;
    fitted_ = false;
    center_on(centre_c);
}

void CanvasController::fit_to_view() {
    if (!widget_ || !session_->has_document()) return;
    const double W = session_->doc().width(), H = session_->doc().height();
    const double vw = std::max(1, widget_->width() - 48), vh = std::max(1, widget_->height() - 48);
    zoom_ = std::clamp(std::min(vw / W, vh / H), kMinZoom, kMaxZoom);
    if (zoom_ > 1.0) zoom_ = std::floor(zoom_);  // integral magnification when enlarging
    off_ = QPointF(std::round((widget_->width() - W * zoom_) / 2.0), std::round((widget_->height() - H * zoom_) / 2.0));
    fitted_ = true;
    emit view_changed();
    request_update();
}

void CanvasController::actual_pixels() {
    if (!widget_ || !session_->has_document()) return;
    const QPointF centre = to_canvas(QPointF(widget_->width() / 2.0, widget_->height() / 2.0));
    zoom_ = 1.0;
    fitted_ = false;
    center_on(centre);
}

void CanvasController::center_on(const QPointF& c) {
    if (!widget_) return;
    off_ = QPointF(widget_->width() / 2.0, widget_->height() / 2.0) - c * zoom_;
    off_ = QPointF(std::round(off_.x()), std::round(off_.y()));
    clamp_offset();
    emit view_changed();
    request_update();
}

void CanvasController::pan_by(const QPointF& d) {
    off_ += d;
    off_ = QPointF(std::round(off_.x()), std::round(off_.y()));
    fitted_ = false;
    clamp_offset();
    emit view_changed();
    request_update();
}

void CanvasController::clamp_offset() {
    if (!widget_ || !session_->has_document()) return;
    // Keep at least 32 px of the canvas on screen.
    const double cw = session_->doc().width() * zoom_, ch = session_->doc().height() * zoom_;
    const double m = 32.0;
    off_.setX(std::clamp(off_.x(), m - cw, widget_->width() - m));
    off_.setY(std::clamp(off_.y(), m - ch, widget_->height() - m));
}

QRectF CanvasController::canvas_rect_widget() const {
    if (!session_->has_document()) return {};
    return QRectF(off_, QSizeF(session_->doc().width() * zoom_, session_->doc().height() * zoom_));
}

QRectF CanvasController::visible_canvas_rect() const {
    if (!widget_ || !session_->has_document()) return {};
    const QRectF v(to_canvas(QPointF(0, 0)), to_canvas(QPointF(widget_->width(), widget_->height())));
    return v.intersected(QRectF(0, 0, session_->doc().width(), session_->doc().height()));
}

void CanvasController::viewport_resized() {
    if (fitted_) fit_to_view();
    else {
        clamp_offset();
        emit view_changed();
    }
}

// ---- tiles --------------------------------------------------------------------------------------------

namespace {

constexpr int kMaxLevel = 6;  // 1/64: below the 2 % minimum zoom

// Finds a node by id in a state's tree (the display override is a DocState, not a Document).
rl::Node* find_node(rl::Node& container, const std::string& id) {
    for (rl::Node& n : container.children) {
        if (n.id == id) return &n;
        if (n.is_group())
            if (rl::Node* f = find_node(n, id)) return f;
    }
    return nullptr;
}

}  // namespace

int CanvasController::level_for_zoom(double zoom) {
    if (zoom >= 1.0) return 0;
    // The finest level whose scale is still >= zoom: at most 2x minification, which bilinear
    // filtering shows cleanly.
    const int l = static_cast<int>(std::floor(std::log2(1.0 / zoom) + 1e-9));
    return std::clamp(l, 0, kMaxLevel);
}

void CanvasController::build_levels() {
    levels_.clear();
    if (!session_->has_document()) return;
    int w = session_->doc().width(), h = session_->doc().height();
    int offset = 0;
    for (int l = 0; l <= kMaxLevel; ++l) {
        Level lv;
        lv.w = w;
        lv.h = h;
        lv.tx = (w + rl::kTileSize - 1) / rl::kTileSize;
        lv.ty = (h + rl::kTileSize - 1) / rl::kTileSize;
        lv.offset = offset;
        lv.tiles.assign(static_cast<size_t>(lv.tx) * static_cast<size_t>(lv.ty), CachedTile{});
        offset += lv.tx * lv.ty;
        levels_.push_back(std::move(lv));
        if (w <= rl::kTileSize && h <= rl::kTileSize) break;  // one tile: coarser levels add nothing
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
}

int CanvasController::total_level_tiles() const {
    if (levels_.empty()) return 0;
    const Level& l = levels_.back();
    return l.offset + l.tx * l.ty;
}

void CanvasController::on_document_reset() {
    finish_stroke();
    clear_override();
    airbrush_timer_.stop();
    drag_ = Drag::None;
    poly_active_ = false;
    poly_.clear();
    crop_ = QRectF();
    xf_ = Xform();
    if (session_->has_document()) {
        tiles_x_ = (session_->doc().width() + rl::kTileSize - 1) / rl::kTileSize;
        tiles_y_ = (session_->doc().height() + rl::kTileSize - 1) / rl::kTileSize;
    } else {
        tiles_x_ = tiles_y_ = 0;
    }
    build_levels();
    ++layout_gen_;
    rebuild_selection_edges();
    fit_to_view();
    emit pending_changed();
    request_update();
}

void CanvasController::invalidate_tile(int tx, int ty) {
    for (size_t l = 0; l < levels_.size(); ++l) {
        Level& lv = levels_[l];
        const int x = tx >> l, y = ty >> l;
        if (x >= lv.tx || y >= lv.ty) return;
        lv.tiles[static_cast<size_t>(y * lv.tx + x)].valid = false;
    }
}

void CanvasController::invalidate_all() {
    for (Level& lv : levels_)
        for (CachedTile& ct : lv.tiles) ct.valid = false;
}

void CanvasController::on_tiles_dirty(const StateDiff& d) {
    if (d.tiles_x != tiles_x_ || d.tiles_y != tiles_y_) return on_document_reset();
    for (int ty = 0; ty < tiles_y_; ++ty)
        for (int tx = 0; tx < tiles_x_; ++tx)
            if (d.tile_dirty(tx, ty)) invalidate_tile(tx, ty);
    request_update();
}

const rl::DocState& CanvasController::display_state() const { return ov_ ? *ov_ : session_->state(); }

void CanvasController::clear_override() {
    if (!ov_) return;
    invalidate_override_tiles();
    ov_tiles_.clear();
    ov_.reset();
    ov_node_ = nullptr;
    xf_src_ = rl::RgbaImage();
    request_update();
}

bool CanvasController::over_budget() const { return deadline_ns_ >= 0 && diag().now_ns() > deadline_ns_; }

void CanvasController::set_render_budget_ms(double ms) {
    budget_ms_ = ms;
    deadline_ns_ = ms > 0.0 ? diag().now_ns() + static_cast<qint64>(ms * 1e6) : -1;
    if (ms > 0.0) complete_ = true;
}

void CanvasController::render_level0(int tx, int ty, CachedTile& ct) {
    rl::composite::render_tile(display_state(), tx, ty, /*onto_bg=*/true, scratch_);
    if (ct.img.isNull()) ct.img = QImage(rl::kTileSize, rl::kTileSize, QImage::Format_ARGB32_Premultiplied);
    // Straight RGBA8 -> premultiplied ARGB32 (display only).
    for (int y = 0; y < rl::kTileSize; ++y) {
        auto* dst = reinterpret_cast<QRgb*>(ct.img.scanLine(y));
        const rl::Rgba8* src = &scratch_.px[static_cast<size_t>(y * rl::kTileSize)];
        for (int x = 0; x < rl::kTileSize; ++x) {
            const rl::Rgba8 p = src[x];
            if (p.a == 255) {
                dst[x] = qRgba(p.r, p.g, p.b, 255);
            } else if (p.a == 0) {
                dst[x] = 0;
            } else {
                const unsigned a = p.a;
                dst[x] = qRgba((p.r * a + 127) / 255, (p.g * a + 127) / 255, (p.b * a + 127) / 255, static_cast<int>(a));
            }
        }
    }
    ct.valid = true;
    ct.gen = ++gen_counter_;
    ++frame_tiles_;
    ++diag().tiles_rendered_total;
    ++diag().cache_misses;
}

void CanvasController::downsample_into(int level, int tx, int ty, CachedTile& ct, bool& all_valid) {
    if (ct.img.isNull()) ct.img = QImage(rl::kTileSize, rl::kTileSize, QImage::Format_ARGB32_Premultiplied);
    const Level& child = levels_[static_cast<size_t>(level - 1)];
    all_valid = true;
    constexpr int H = rl::kTileSize / 2;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int cx = 2 * tx + i, cy = 2 * ty + j;
            const QImage* src = nullptr;
            int cw = 0, ch = 0;
            if (cx < child.tx && cy < child.ty) {
                src = level_tile(level - 1, cx, cy);
                if (!child.tiles[static_cast<size_t>(cy * child.tx + cx)].valid) all_valid = false;
                cw = std::min(rl::kTileSize, child.w - cx * rl::kTileSize);
                ch = std::min(rl::kTileSize, child.h - cy * rl::kTileSize);
            }
            for (int y = 0; y < H; ++y) {
                auto* dst = reinterpret_cast<QRgb*>(ct.img.scanLine(j * H + y)) + i * H;
                if (!src || 2 * y >= ch) {
                    std::fill(dst, dst + H, QRgb(0));
                    continue;
                }
                const int y0 = 2 * y, y1 = std::min(2 * y + 1, ch - 1);
                const auto* r0 = reinterpret_cast<const QRgb*>(src->constScanLine(y0));
                const auto* r1 = reinterpret_cast<const QRgb*>(src->constScanLine(y1));
                for (int x = 0; x < H; ++x) {
                    const int x0 = 2 * x;
                    if (x0 >= cw) {
                        dst[x] = 0;
                        continue;
                    }
                    const int x1 = std::min(x0 + 1, cw - 1);
                    const QRgb a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
                    // Box 2x2 of premultiplied values; byte-wise, rounded.
                    const auto avg = [&](int sh) {
                        return ((((a >> sh) & 0xff) + ((b >> sh) & 0xff) + ((c >> sh) & 0xff) + ((d >> sh) & 0xff) + 2) >> 2) << sh;
                    };
                    dst[x] = avg(24) | avg(16) | avg(8) | avg(0);
                }
            }
        }
    }
    ct.gen = ++gen_counter_;
}

const QImage* CanvasController::level_tile(int level, int tx, int ty) {
    Level& lv = levels_[static_cast<size_t>(level)];
    CachedTile& ct = lv.tiles[static_cast<size_t>(ty * lv.tx + tx)];
    if (ct.valid) {
        ++diag().cache_hits;
        return &ct.img;
    }
    if (level == 0) {
        if (over_budget()) {
            complete_ = false;
            return ct.img.isNull() ? nullptr : &ct.img;  // stale is better than a hole
        }
        render_level0(tx, ty, ct);
        return &ct.img;
    }
    // Coarser levels are cheap 2x2 means of the level below (whose own renders are budgeted).
    
    bool all = true;
    downsample_into(level, tx, ty, ct, all);
    ct.valid = all;
    if (!all) complete_ = false;
    return &ct.img;
}

const QImage& CanvasController::display_tile(int tx, int ty) {
    const qint64 saved = deadline_ns_;
    deadline_ns_ = -1;
    const QImage* img = level_tile(0, tx, ty);
    deadline_ns_ = saved;
    return *img;
}

quint64 CanvasController::tile_generation(int level, int tx, int ty) const {
    const Level& lv = levels_[static_cast<size_t>(level)];
    return lv.tiles[static_cast<size_t>(ty * lv.tx + tx)].gen;
}

void CanvasController::finish_display() {
    if (levels_.empty()) return;
    const qint64 saved = deadline_ns_;
    deadline_ns_ = -1;
    const int l = display_level();
    const QRect r = visible_tile_range(l);
    for (int ty = r.top(); ty <= r.bottom() && !r.isEmpty(); ++ty)
        for (int tx = r.left(); tx <= r.right(); ++tx) level_tile(l, tx, ty);
    deadline_ns_ = saved;
    complete_ = true;
    request_update();
}

QSize CanvasController::doc_size() const {
    return session_->has_document() ? QSize(session_->doc().width(), session_->doc().height()) : QSize();
}

std::optional<QColor> CanvasController::composite_pixel(int x, int y) {
    if (!session_->has_document() || x < 0 || y < 0 || x >= session_->doc().width() || y >= session_->doc().height())
        return std::nullopt;
    rl::composite::render_tile(session_->state(), x / rl::kTileSize, y / rl::kTileSize, true, scratch_);
    const rl::Rgba8 p = scratch_.at(x % rl::kTileSize, y % rl::kTileSize);
    return QColor(p.r, p.g, p.b, p.a);
}

QRectF CanvasController::level_tile_canvas_rect(int level, int tx, int ty) const {
    const double s = static_cast<double>(1 << level) * rl::kTileSize;
    const QRectF r(tx * s, ty * s, s, s);
    return r.intersected(QRectF(0, 0, session_->doc().width(), session_->doc().height()));
}

QRect CanvasController::visible_tile_range(int level) const {
    const QRectF v = visible_canvas_rect();
    if (v.isEmpty() || levels_.empty()) return {};
    const Level& lv = levels_[static_cast<size_t>(level)];
    const double s = static_cast<double>(1 << level) * rl::kTileSize;
    const int x0 = std::max(0, static_cast<int>(std::floor(v.left() / s)));
    const int y0 = std::max(0, static_cast<int>(std::floor(v.top() / s)));
    const int x1 = std::min(lv.tx, static_cast<int>(std::ceil(v.right() / s)));
    const int y1 = std::min(lv.ty, static_cast<int>(std::ceil(v.bottom() / s)));
    if (x1 <= x0 || y1 <= y0) return {};
    return QRect(QPoint(x0, y0), QPoint(x1 - 1, y1 - 1));
}

void CanvasController::frame_begin() {
    frame_tiles_ = 0;
    frame_start_ns_ = diag().now_ns();
    complete_ = true;
    // Tile rendering per frame is budgeted so a huge document stays interactive; what does not fit
    // is finished by follow-up frames (stale tiles or the checkerboard show meanwhile).
    deadline_ns_ = frame_start_ns_ + static_cast<qint64>(kFrameBudgetMs * 1e6);
}

void CanvasController::frame_end() {
    Diagnostics& d = diag();
    const qint64 now = d.now_ns();
    deadline_ns_ = -1;
    d.tiles_last_frame = frame_tiles_;
    d.last_frame_ms = static_cast<double>(now - frame_start_ns_) / 1e6;
    d.note_frame(d.last_frame_ms);
    if (d.pending_sample_ns >= 0) {
        d.dab_to_pixel_ms = static_cast<double>(now - d.pending_sample_ns) / 1e6;
        d.note_dab_latency(d.dab_to_pixel_ms);
        d.pending_sample_ns = -1;
    }
    if (!complete_) QTimer::singleShot(0, this, [this] { request_update(); });
    if (frame_tiles_ > 0) emit display_progress();
}

void CanvasController::paint_raster(QPainter& p, const QRect& update_rect) {
    p.fillRect(update_rect, theme::kWorkspace);
    if (!session_->has_document() || levels_.empty()) return;
    const QRectF cr = canvas_rect_widget();
    // Same rounding as the tile rects below, so no checker row peeks out at a fractional edge.
    const QRect cri(QPoint(static_cast<int>(std::round(cr.left())), static_cast<int>(std::round(cr.top()))),
                    QPoint(static_cast<int>(std::round(cr.right())) - 1, static_cast<int>(std::round(cr.bottom())) - 1));
    // Drop shadow + checkerboard (fixed in screen space: brush origin at the widget origin).
    p.fillRect(cri.adjusted(2, 2, 3, 3), QColor(0, 0, 0, 90));
    p.setBrushOrigin(0, 0);
    p.fillRect(cri.intersected(update_rect), QBrush(checker_pixmap()));
    const int l = display_level();
    const double s = static_cast<double>(1 << l);
    const bool exact = std::abs(zoom_ * s - 1.0) < 1e-9;  // level pixels map 1:1 to screen pixels
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0 && !exact);
    const QRect tr = visible_tile_range(l);
    for (int ty = tr.top(); ty <= tr.bottom() && !tr.isEmpty(); ++ty) {
        for (int tx = tr.left(); tx <= tr.right(); ++tx) {
            const QRectF c = level_tile_canvas_rect(l, tx, ty);
            const QPointF a = to_widget(c.topLeft());
            const QPointF b = to_widget(c.bottomRight());
            const QRect target(QPoint(static_cast<int>(std::round(a.x())), static_cast<int>(std::round(a.y()))),
                               QPoint(static_cast<int>(std::round(b.x())) - 1, static_cast<int>(std::round(b.y())) - 1));
            if (!target.intersects(update_rect)) continue;
            const QImage* img = level_tile(l, tx, ty);
            if (!img) continue;
            p.drawImage(target, *img, QRectF(0, 0, c.width() / s, c.height() / s));
        }
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
}

// ---- selection edges ------------------------------------------------------------------------------------

void CanvasController::on_selection_changed() {
    rebuild_selection_edges();
    request_update();
}

void CanvasController::rebuild_selection_edges() {
    sel_edges_.clear();
    ants_timer_.stop();
    if (!session_->has_document()) return;
    const rl::Selection& sel = session_->state().selection;
    if (!sel.active()) return;
    const int W = session_->doc().width(), H = session_->doc().height();
    // Display threshold at 50 % coverage (a feathered selection shows its half-way contour).
    std::vector<uint8_t> in(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    const bool bg_in = sel.mask.background() >= 128;
    for (int ty = 0; ty < tiles_y_; ++ty) {
        for (int tx = 0; tx < tiles_x_; ++tx) {
            if (sel.mask.is_absent(tx, ty) && !bg_in) continue;
            const rl::GrayTile& t = sel.mask.tile(tx, ty);
            const int x0 = tx * rl::kTileSize, y0 = ty * rl::kTileSize;
            const int nx = std::min(rl::kTileSize, W - x0), ny = std::min(rl::kTileSize, H - y0);
            for (int ly = 0; ly < ny; ++ly)
                for (int lx = 0; lx < nx; ++lx)
                    in[static_cast<size_t>(y0 + ly) * static_cast<size_t>(W) + static_cast<size_t>(x0 + lx)] =
                        t.at(lx, ly) >= 128 ? 1 : 0;
        }
    }
    const auto at = [&](int x, int y) -> uint8_t {
        if (x < 0 || y < 0 || x >= W || y >= H) return 0;
        return in[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)];
    };
    for (int y = 0; y <= H; ++y) {  // horizontal boundaries between rows y-1 and y
        int run = -1;
        for (int x = 0; x <= W; ++x) {
            const bool edge = x < W && at(x, y - 1) != at(x, y);
            if (edge && run < 0) run = x;
            if (!edge && run >= 0) {
                sel_edges_.push_back(QLineF(run, y, x, y));
                run = -1;
            }
        }
    }
    for (int x = 0; x <= W; ++x) {  // vertical boundaries between columns x-1 and x
        int run = -1;
        for (int y = 0; y <= H; ++y) {
            const bool edge = y < H && at(x - 1, y) != at(x, y);
            if (edge && run < 0) run = y;
            if (!edge && run >= 0) {
                sel_edges_.push_back(QLineF(x, run, x, y));
                run = -1;
            }
        }
    }
    if (!sel_edges_.isEmpty()) ants_timer_.start();
}

// ---- overlays -----------------------------------------------------------------------------------------------

void CanvasController::paint_overlays(QPainter& p) {
    if (!session_->has_document()) return;
    const QRectF cr = canvas_rect_widget();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(QColor(0, 0, 0, 160), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(cr.adjusted(-0.5, -0.5, 0.5, 0.5));

    const QTransform view(zoom_, 0, 0, zoom_, off_.x(), off_.y());  // canvas -> widget

    // Selection "marching ants".
    if (!sel_edges_.isEmpty()) {
        p.save();
        p.setTransform(view);
        QPen black(Qt::black, 0);
        p.setPen(black);
        p.drawLines(sel_edges_);
        QPen white(Qt::white, 0);
        white.setDashPattern({4, 4});
        white.setDashOffset(ants_phase_);
        p.setPen(white);
        p.drawLines(sel_edges_);
        p.restore();
    }

    const QPen guide_dark(QColor(0, 0, 0, 200), 1);
    QPen guide_light(QColor(255, 255, 255, 230), 1, Qt::DashLine);
    const auto dual = [&](const std::function<void()>& draw) {
        p.setPen(guide_dark);
        draw();
        p.setPen(guide_light);
        draw();
    };

    switch (drag_) {
        case Drag::ZoomDrag: {
            const QRectF r = QRectF(press_w_, last_w_).normalized();
            if (r.width() >= kZoomDragMin || r.height() >= kZoomDragMin) {
                p.setBrush(QColor(255, 255, 255, 28));
                dual([&] { p.drawRect(r); });
            }
            break;
        }
        case Drag::Marquee: {
            const QRectF r = QRectF(to_widget(press_c_), to_widget(cur_c_)).normalized();
            p.setBrush(Qt::NoBrush);
            if (effective_tool() == Tool::MarqueeEllipse) {
                p.setRenderHint(QPainter::Antialiasing, true);
                dual([&] { p.drawEllipse(r); });
            } else {
                dual([&] { p.drawRect(r); });
            }
            break;
        }
        case Drag::Lasso: {
            QPolygonF w;
            for (const QPointF& c : lasso_) w << to_widget(c);
            p.setRenderHint(QPainter::Antialiasing, true);
            dual([&] { p.drawPolyline(w); });
            break;
        }
        case Drag::Gradient: {
            p.setRenderHint(QPainter::Antialiasing, true);
            const QPointF a = to_widget(press_c_), b = to_widget(cur_c_);
            dual([&] { p.drawLine(a, b); });
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(tools_->fg());
            p.drawEllipse(a, 4.5, 4.5);
            p.setBrush(tools_->bg());
            p.drawEllipse(b, 4.5, 4.5);
            break;
        }
        case Drag::Move: {
            if (move_bounds_) {
                const QPointF d(std::round(cur_c_.x() - press_c_.x()), std::round(cur_c_.y() - press_c_.y()));
                const QRectF r(to_widget(QPointF(move_bounds_->topLeft()) + d),
                               to_widget(QPointF(move_bounds_->topLeft() + QPoint(move_bounds_->width(), move_bounds_->height())) + d));
                p.setBrush(QColor(74, 140, 247, 40));
                dual([&] { p.drawRect(r); });
                draw_label(p, r.topLeft(), QStringLiteral("Δ %1, %2").arg(d.x()).arg(d.y()), widget_->rect());
            }
            break;
        }
        default: break;
    }

    // Polygon lasso in progress.
    if (poly_active_ && !poly_.isEmpty()) {
        QPolygonF w;
        for (const QPointF& c : poly_) w << to_widget(c);
        w << to_widget(hover_c_);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        dual([&] { p.drawPolyline(w); });
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::white);
        for (int i = 0; i < poly_.size(); ++i) p.drawRect(QRectF(to_widget(poly_[i]) - QPointF(2.5, 2.5), QSizeF(5, 5)));
    }

    // Pending crop: dim the outside, frame + thirds.
    if (!crop_.isEmpty()) {
        const QRectF r(to_widget(crop_.topLeft()), to_widget(crop_.bottomRight()));
        QPainterPath outside;
        outside.addRect(cr.united(r).adjusted(-2000, -2000, 2000, 2000));
        QPainterPath inside;
        inside.addRect(r);
        p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 120));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 90), 1));
        for (int i = 1; i < 3; ++i) {
            p.drawLine(QPointF(r.left() + r.width() * i / 3.0, r.top()), QPointF(r.left() + r.width() * i / 3.0, r.bottom()));
            p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 3.0), QPointF(r.right(), r.top() + r.height() * i / 3.0));
        }
        dual([&] { p.drawRect(r); });
        draw_label(p, r.topLeft(),
                   QStringLiteral("%1 × %2   Enter crops, Esc cancels").arg(crop_rect().width()).arg(crop_rect().height()),
                   widget_->rect());
    }

    // Free transform box (handles mapped through the same matrix the op will use).
    if (xf_.active) {
        const QPolygonF poly = xform_polygon_widget();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        dual([&] { p.drawPolygon(poly); });
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::white);
        const auto hs = xform_handles_canvas();
        for (size_t i = 0; i < hs.size(); ++i) {
            const QPointF w = to_widget(hs[i]);
            if (std::isnan(w.x())) continue;
            if (xf_.quad_mode && i % 2 == 0) p.drawEllipse(w, 4.5, 4.5);  // distortable corners
            else p.drawRect(QRectF(w - QPointF(3.5, 3.5), QSizeF(7, 7)));
        }
        if (!xf_.quad_mode) p.drawEllipse(to_widget(xf_.pivot + QPointF(xf_.tx, xf_.ty)), 4, 4);
        const QString text = xf_.quad_mode ? tr("Distort   Ctrl: corner  Ctrl+Shift: skew  Ctrl+Alt+Shift: perspective   Enter applies, Esc cancels")
                                           : tr("W %1 %   H %2 %   %3°   Ctrl+corner distorts   Enter applies, Esc cancels")
                                                 .arg(xf_.sx * 100, 0, 'f', 1)
                                                 .arg(xf_.sy * 100, 0, 'f', 1)
                                                 .arg(xf_.rot, 0, 'f', 1);
        draw_label(p, poly.boundingRect().topLeft() + QPointF(0, -8), text, widget_->rect());
    }

    // Clone source marker.
    const Tool et = effective_tool();
    if (et == Tool::Clone && clone_src_) {
        QPointF c = *clone_src_;
        if (stroke_.active() && clone_off_ && !stroke_.samples().empty())
            c = QPointF(stroke_.samples().back().x + clone_off_->x(), stroke_.samples().back().y + clone_off_->y());
        const QPointF w = to_widget(c);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        dual([&] {
            p.drawLine(w - QPointF(8, 0), w + QPointF(8, 0));
            p.drawLine(w - QPointF(0, 8), w + QPointF(0, 8));
            p.drawEllipse(w, 5, 5);
        });
    }

    // Brush cursor outline.
    if (hover_inside_ && (et == Tool::Brush || et == Tool::Eraser || et == Tool::Clone) && !temp_hand_) {
        const BrushSettings* b = tools_->brush_settings_for(et);
        const double r = std::max(1.0, b->size * zoom_ * 0.5);
        const QPointF w = to_widget(hover_c_);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.save();
        p.translate(w);
        p.rotate(-b->angle);
        p.setPen(QPen(QColor(0, 0, 0, 170), 1.5));
        p.drawEllipse(QPointF(0, 0), r, r * b->roundness);
        p.setPen(QPen(QColor(255, 255, 255, 220), 0.8));
        p.drawEllipse(QPointF(0, 0), r, r * b->roundness);
        p.restore();
    }
    p.restore();
}

// ---- helpers ---------------------------------------------------------------------------------------------------

Tool CanvasController::effective_tool() const {
    if (temp_hand_) return Tool::Hand;
    if (xf_.active) return Tool::Transform;
    return tools_->tool();
}

void CanvasController::update_cursor() {
    if (!widget_) return;
    switch (effective_tool()) {
        case Tool::Hand: widget_->setCursor(drag_ == Drag::Pan ? Qt::ClosedHandCursor : Qt::OpenHandCursor); break;
        case Tool::Move: widget_->setCursor(Qt::SizeAllCursor); break;
        case Tool::Zoom: widget_->setCursor(Qt::CrossCursor); break;
        case Tool::Brush:
        case Tool::Eraser:
        case Tool::Clone: widget_->setCursor(Qt::CrossCursor); break;
        case Tool::Eyedropper: widget_->setCursor(Qt::PointingHandCursor); break;
        default: widget_->setCursor(Qt::CrossCursor); break;
    }
}

void CanvasController::on_tool_changed() {
    // doc 40 §5: a tool switch ends the stroke (commit).
    finish_stroke();
    if (poly_active_) {
        poly_active_ = false;
        poly_.clear();
    }
    if (tools_->tool() != Tool::Crop) crop_ = QRectF();
    if (xf_.active && tools_->tool() != Tool::Transform) {
        xf_ = Xform();
        xf_timer_.stop();
        clear_override();
    }
    drag_ = Drag::None;
    update_cursor();
    emit pending_changed();
    request_update();
}

void CanvasController::apply_op(const Json& op, const QString& label) { session_->apply(op, label); }

bool CanvasController::require_raster_layer(const QString& what) {
    const rl::Node* n = session_->active_node();
    if (n && n->is_raster()) return true;
    emit message(tr("%1 needs a raster layer: select one in the Layers panel.").arg(what), true);
    return false;
}

std::optional<QRect> CanvasController::layer_bounds(const std::string& id) const {
    if (!session_->has_document()) return std::nullopt;
    const rl::Node* n = nullptr;
    // active_node() finds by id; reuse via the session when it is the active layer.
    if (session_->active_layer() == id) n = session_->active_node();
    if (!n || !n->is_raster()) return std::nullopt;
    const rl::RgbaImage& im = n->pixels;
    const int W = im.width(), H = im.height();
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int ty = 0; ty < im.tiles_y(); ++ty) {
        for (int tx = 0; tx < im.tiles_x(); ++tx) {
            if (im.is_absent(tx, ty) && im.background().a == 0) continue;
            const rl::RgbaTile& t = im.tile(tx, ty);
            const int bx = tx * rl::kTileSize, by = ty * rl::kTileSize;
            const int nx = std::min(rl::kTileSize, W - bx), ny = std::min(rl::kTileSize, H - by);
            for (int ly = 0; ly < ny; ++ly)
                for (int lx = 0; lx < nx; ++lx)
                    if (t.at(lx, ly).a != 0) {
                        x0 = std::min(x0, bx + lx);
                        y0 = std::min(y0, by + ly);
                        x1 = std::max(x1, bx + lx);
                        y1 = std::max(y1, by + ly);
                    }
        }
    }
    if (x1 < 0) return std::nullopt;
    return QRect(QPoint(x0, y0), QPoint(x1, y1));
}

StrokeSample CanvasController::sample_from(const PointerInput& in) const {
    const QPointF c = to_canvas(in.pos);
    StrokeSample s;
    s.x = c.x();
    s.y = c.y();
    s.pressure = in.tablet ? in.pressure : 1.0;  // doc 40 §5: mouse records pressure 1.0, tilt 0
    s.tilt_x = in.tablet ? in.tilt_x : 0.0;
    s.tilt_y = in.tablet ? in.tilt_y : 0.0;
    s.t_ms = std::max(0.0, in.t_ms - stroke_t0_);
    return s;
}

void CanvasController::set_temp_hand(bool on) {
    if (on == temp_hand_) return;
    temp_hand_ = on;
    update_cursor();
    request_update();
}

// ---- strokes --------------------------------------------------------------------------------------------------------

void CanvasController::start_stroke(const PointerInput& in, Tool t) {
    const rl::Node* n = session_->active_node();
    const bool mask = t == Tool::Brush && session_->edit_mask();
    if (!n || !n->is_raster()) {
        emit message(tr("%1 needs a raster layer: select one in the Layers panel.").arg(tool_label(t)), true);
        return;
    }
    StrokeSetup st;
    st.kind = t == Tool::Brush ? StrokeKind::Brush : t == Tool::Eraser ? StrokeKind::Eraser : StrokeKind::Clone;
    st.brush = *tools_->brush_settings_for(t);
    st.layer = n->id;
    st.target_mask = mask;
    st.color = tools_->fg();
    st.view_zoom = zoom_;
    stroke_t0_ = in.t_ms;
    const StrokeSample first = sample_from(in);
    if (st.kind == StrokeKind::Clone) {
        if (!clone_src_) {
            emit message(tr("Clone Stamp: Alt-click to set the source point first."), true);
            return;
        }
        st.clone_aligned = tools_->settings().clone_aligned;
        // Every op carries its own "source", so each clone_stroke op is self-contained (replayable
        // on its own) and still follows doc 40 §4: aligned reuses the offset fixed by the first
        // stroke after the source was set; non-aligned restarts at the source point each stroke.
        if (st.clone_aligned && clone_off_) {
            st.clone_source = QPointF(first.x + clone_off_->x(), first.y + clone_off_->y());
        } else {
            st.clone_source = *clone_src_;
            clone_off_ = QPoint(static_cast<int>(rhu(clone_src_->x() - first.x)),
                                static_cast<int>(rhu(clone_src_->y() - first.y)));
        }
    }
    stroke_before_ = session_->state();
    StrokeUpdate u;
    const std::string err = stroke_.begin(session_->doc(), st, first, &u);
    if (!err.empty()) {
        emit message(tool_label(t) + QStringLiteral(": ") + QString::fromStdString(err), true);
        return;
    }
    // The canvas shows the document with the target replaced by the engine's live preview.
    ov_ = session_->state();
    ov_node_ = find_node(ov_->root, st.layer);
    Diagnostics& d = diag();
    d.reset_stroke_stats();
    d.pending_sample_ns = d.now_ns();
    stroke_wall0_ns_ = d.pending_sample_ns;
    apply_stroke_update(u);
    drag_ = Drag::Stroke;
    if (st.brush.dabs_per_second > 0.0) {
        airbrush_wall0_ = diag().now_ms();
        airbrush_timer_.start();
    }
}

void CanvasController::apply_stroke_update(const StrokeUpdate& u) {
    Diagnostics& d = diag();
    d.stroke_dabs += static_cast<qint64>(u.dabs);
    d.stroke_engine_ms += u.engine_ms;
    ++d.stroke_events;
    d.stroke_wall_ms = static_cast<double>(d.now_ns() - stroke_wall0_ns_) / 1e6;
    if (u.tiles.empty() || !ov_node_) return;
    // Share the changed preview tiles into the display state (copy-on-write: no pixel copies) and
    // re-render only those tiles.
    if (stroke_.targets_mask()) {
        const rl::GrayImage* m = stroke_.preview_mask();
        if (m && ov_node_->mask) {
            for (const QPoint& t : u.tiles) {
                if (m->is_absent(t.x(), t.y())) ov_node_->mask->plane.erase_tile(t.x(), t.y());
                else ov_node_->mask->plane.put_tile(t.x(), t.y(), m->tile_ptr(t.x(), t.y()));
            }
        }
    } else if (const rl::RgbaImage* px = stroke_.preview_pixels()) {
        for (const QPoint& t : u.tiles) {
            if (px->is_absent(t.x(), t.y())) ov_node_->pixels.erase_tile(t.x(), t.y());
            else ov_node_->pixels.put_tile(t.x(), t.y(), px->tile_ptr(t.x(), t.y()));
        }
    }
    for (const QPoint& t : u.tiles) invalidate_tile(t.x(), t.y());
    ov_tiles_.insert(ov_tiles_.end(), u.tiles.begin(), u.tiles.end());
    if (d.pending_sample_ns < 0) d.pending_sample_ns = d.now_ns();
    request_update_canvas_rect(u.rect);
}

void CanvasController::finish_stroke() {
    airbrush_timer_.stop();
    if (!stroke_.active()) return;
    const StrokeKind k = stroke_.setup().kind;
    const Json op = stroke_.finish();  // the engine writes the preview into the layer
    clear_override();
    if (drag_ == Drag::Stroke) drag_ = Drag::None;
    const rl::DocState before = std::move(stroke_before_);
    stroke_before_ = rl::DocState();
    session_->record_external(before, op,
                              k == StrokeKind::Brush ? tr("Brush Stroke") : k == StrokeKind::Eraser ? tr("Eraser Stroke") : tr("Clone Stamp"));
}

void CanvasController::on_airbrush_tick() {
    if (!stroke_.active()) {
        airbrush_timer_.stop();
        return;
    }
    StrokeSample s = stroke_.samples().back();
    s.t_ms = static_cast<double>(diag().now_ms() - airbrush_wall0_);
    if (s.t_ms <= stroke_.samples().back().t_ms) return;
    apply_stroke_update(stroke_.add_sample(s));
}

// ---- pointer input ------------------------------------------------------------------------------------------------------

void CanvasController::pointer_press(const PointerInput& in) {
    if (!session_->has_document()) return;
    press_w_ = last_w_ = in.pos;
    press_c_ = cur_c_ = to_canvas(in.pos);
    press_mods_ = in.mods;

    if (in.button == Qt::MiddleButton) {
        drag_ = Drag::Pan;
        update_cursor();
        return;
    }
    if (in.button != Qt::LeftButton) return;

    Tool t = effective_tool();
    if (in.eraser_end && (t == Tool::Brush || t == Tool::Clone)) t = Tool::Eraser;  // doc 40 §5

    switch (t) {
        case Tool::Hand:
            drag_ = Drag::Pan;
            update_cursor();
            return;
        case Tool::Zoom:
            // Click = one step (in, Alt: out) at the pointer, decided on release; a drag draws a
            // rectangle that is zoomed to fill the view.
            drag_ = Drag::ZoomDrag;
            zoom_alt_ = (in.mods & Qt::AltModifier) != 0;
            return;
        case Tool::Eyedropper: {
            const QPointF c = press_c_;
            if (auto col = composite_pixel(static_cast<int>(std::floor(c.x())), static_cast<int>(std::floor(c.y()))))
                emit color_sampled(*col, (in.mods & Qt::AltModifier) != 0);
            return;
        }
        case Tool::Brush:
        case Tool::Eraser:
        case Tool::Clone:
            if (t == Tool::Clone && (in.mods & Qt::AltModifier)) {
                clone_src_ = press_c_;
                clone_off_.reset();
                emit message(tr("Clone source set at %1, %2").arg(press_c_.x(), 0, 'f', 1).arg(press_c_.y(), 0, 'f', 1), false);
                request_update();
                return;
            }
            if (t == Tool::Brush && (in.mods & Qt::AltModifier)) {  // Alt: temporary eyedropper
                if (auto col = composite_pixel(static_cast<int>(std::floor(press_c_.x())), static_cast<int>(std::floor(press_c_.y()))))
                    emit color_sampled(*col, false);
                return;
            }
            start_stroke(in, t);
            return;
        case Tool::MarqueeRect:
        case Tool::MarqueeEllipse:
        case Tool::Lasso: {
            SelMode m = tools_->settings().sel_mode;
            const bool shift = in.mods & Qt::ShiftModifier, alt = in.mods & Qt::AltModifier;
            if (shift && alt) m = SelMode::Intersect;
            else if (shift) m = SelMode::Add;
            else if (alt) m = SelMode::Subtract;
            gesture_sel_mode_ = m;
            if (t == Tool::Lasso) {
                lasso_.clear();
                lasso_ << press_c_;
                drag_ = Drag::Lasso;
            } else {
                drag_ = Drag::Marquee;
            }
            return;
        }
        case Tool::PolygonLasso: {
            if (!poly_active_) {
                SelMode m = tools_->settings().sel_mode;
                const bool shift = in.mods & Qt::ShiftModifier, alt = in.mods & Qt::AltModifier;
                if (shift && alt) m = SelMode::Intersect;
                else if (shift) m = SelMode::Add;
                else if (alt) m = SelMode::Subtract;
                gesture_sel_mode_ = m;
                poly_active_ = true;
                poly_.clear();
                poly_ << press_c_;
                hover_c_ = press_c_;
                emit pending_changed();
            } else {
                const QPointF first = to_widget(poly_.front());
                if (in.double_click || (poly_.size() >= 3 && QLineF(first, in.pos).length() < 7.0)) {
                    commit_pending();
                    return;
                }
                poly_ << press_c_;
            }
            request_update();
            return;
        }
        case Tool::Wand: {
            if (!require_raster_layer(tr("Magic Wand"))) return;
            const int x = static_cast<int>(std::floor(press_c_.x())), y = static_cast<int>(std::floor(press_c_.y()));
            if (x < 0 || y < 0 || x >= session_->doc().width() || y >= session_->doc().height()) return;
            SelMode m = tools_->settings().sel_mode;
            const bool shift = in.mods & Qt::ShiftModifier, alt = in.mods & Qt::AltModifier;
            if (shift && alt) m = SelMode::Intersect;
            else if (shift) m = SelMode::Add;
            else if (alt) m = SelMode::Subtract;
            const ToolSettings& s = tools_->settings();
            apply_op({{"op", "select_wand"}, {"layer", session_->active_layer()}, {"x", x}, {"y", y},
                      {"tolerance", s.wand_tolerance}, {"contiguous", s.wand_contiguous},
                      {"antialias", s.wand_antialias}, {"mode", sel_mode_json(m)}},
                     tr("Magic Wand"));
            return;
        }
        case Tool::Bucket: {
            if (!require_raster_layer(tr("Paint Bucket"))) return;
            const int x = static_cast<int>(std::floor(press_c_.x())), y = static_cast<int>(std::floor(press_c_.y()));
            if (x < 0 || y < 0 || x >= session_->doc().width() || y >= session_->doc().height()) return;
            const ToolSettings& s = tools_->settings();
            apply_op({{"op", "bucket_fill"}, {"layer", session_->active_layer()}, {"x", x}, {"y", y},
                      {"color", hex(tools_->fg())}, {"opacity", s.bucket_opacity}, {"tolerance", s.bucket_tolerance},
                      {"contiguous", s.bucket_contiguous}, {"antialias", s.bucket_antialias}},
                     tr("Paint Bucket"));
            return;
        }
        case Tool::Gradient:
            if (!require_raster_layer(tr("Gradient"))) return;
            drag_ = Drag::Gradient;
            return;
        case Tool::Move:
            if (!require_raster_layer(tr("Move"))) return;
            move_bounds_ = layer_bounds(session_->active_layer());
            if (!move_bounds_) move_bounds_ = QRect(0, 0, session_->doc().width(), session_->doc().height());
            drag_ = Drag::Move;
            return;
        case Tool::Crop:
            if (!crop_.isEmpty() && in.double_click) {
                commit_pending();
                return;
            }
            if (!crop_.isEmpty() && crop_.contains(press_c_)) {
                drag_ = Drag::CropMove;
                return;
            }
            crop_ = QRectF();
            drag_ = Drag::Crop;
            return;
        case Tool::Transform: {
            if (in.double_click) {
                commit_pending();
                return;
            }
            const int hit = xform_hit(in.pos);
            xf_.press = press_c_;
            xf_.p_tx = xf_.tx;
            xf_.p_ty = xf_.ty;
            xf_.p_sx = xf_.sx;
            xf_.p_sy = xf_.sy;
            xf_.p_rot = xf_.rot;
            xf_.handle = hit;
            xf_.mods = in.mods;
            xf_dragging_ = true;
            if (hit >= 0 && hit < 8 && ((in.mods & Qt::ControlModifier) || xf_.quad_mode)) {
                to_quad_mode();  // Ctrl+corner: distort (doc 30 §12.2 quad form from here on)
                drag_ = Drag::XQuad;
            } else {
                drag_ = hit >= 0 && hit < 8 ? Drag::XScale : hit == 8 ? Drag::XMove : Drag::XRotate;
            }
            xf_.p_quad = xf_.quad;
            return;
        }
    }
}

void CanvasController::pointer_move(const PointerInput& in) {
    const QPointF prev_w = last_w_;
    last_w_ = in.pos;
    cur_c_ = to_canvas(in.pos);
    hover_c_ = cur_c_;
    emit cursor_moved(cur_c_, session_->has_document() && QRectF(0, 0, session_->doc().width(), session_->doc().height()).contains(cur_c_));
    switch (drag_) {
        case Drag::Pan: pan_by(in.pos - prev_w); return;
        case Drag::Stroke: {
            if (diag().pending_sample_ns < 0) diag().pending_sample_ns = diag().now_ns();
            apply_stroke_update(stroke_.add_sample(sample_from(in)));
            // The cursor outline follows too.
            request_update_canvas_rect(QRectF(cur_c_.x() - 2, cur_c_.y() - 2, 4, 4));
            return;
        }
        case Drag::Lasso:
            if (lasso_.isEmpty() || QLineF(to_widget(lasso_.back()), in.pos).length() >= 1.0) lasso_ << cur_c_;
            request_update();
            return;
        case Drag::ZoomDrag: request_update(); return;
        case Drag::Crop:
            crop_ = QRectF(QPointF(std::round(press_c_.x()), std::round(press_c_.y())),
                           QPointF(std::round(cur_c_.x()), std::round(cur_c_.y())))
                        .normalized();
            request_update();
            return;
        case Drag::CropMove: {
            const QPointF d = to_canvas(in.pos) - to_canvas(prev_w);
            crop_.translate(d);
            request_update();
            return;
        }
        case Drag::XMove: {
            QPointF d = cur_c_ - xf_.press;
            if (in.mods & Qt::ShiftModifier) d = QPointF(std::round(d.x()), std::round(d.y()));
            if (xf_.quad_mode) {
                for (size_t i = 0; i < 4; ++i) xf_.quad[i] = xf_.p_quad[i] + d;
            } else {
                xf_.tx = xf_.p_tx + d.x();
                xf_.ty = xf_.p_ty + d.y();
            }
            xform_changed();
            return;
        }
        case Drag::XQuad: {
            const bool ctrl = xf_.mods & Qt::ControlModifier, shift = xf_.mods & Qt::ShiftModifier, alt = xf_.mods & Qt::AltModifier;
            drag_quad(cur_c_, ctrl && shift && alt ? QuadDrag::Perspective : ctrl && shift ? QuadDrag::Skew : QuadDrag::Distort);
            xform_changed();
            return;
        }
        case Drag::XRotate: {
            const QPointF c = xf_.quad_mode ? (xf_.p_quad[0] + xf_.p_quad[1] + xf_.p_quad[2] + xf_.p_quad[3]) / 4.0
                                            : QPointF(xf_.pivot.x() + xf_.tx, xf_.pivot.y() + xf_.ty);
            const double a0 = std::atan2(xf_.press.y() - c.y(), xf_.press.x() - c.x());
            const double a1 = std::atan2(cur_c_.y() - c.y(), cur_c_.x() - c.x());
            double deg = xf_.p_rot + (a1 - a0) * 180.0 / M_PI;
            while (deg > 180.0) deg -= 360.0;
            while (deg < -180.0) deg += 360.0;
            if (in.mods & Qt::ShiftModifier) deg = std::round(deg / 15.0) * 15.0;
            if (xf_.quad_mode) {
                // Rotate the distorted quad about its centre.
                const QPointF c2 = (xf_.p_quad[0] + xf_.p_quad[1] + xf_.p_quad[2] + xf_.p_quad[3]) / 4.0;
                const double a = (deg - xf_.p_rot) * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
                for (size_t i = 0; i < 4; ++i) {
                    const QPointF v = xf_.p_quad[i] - c2;
                    xf_.quad[i] = c2 + QPointF(v.x() * ca - v.y() * sa, v.x() * sa + v.y() * ca);
                }
            } else {
                xf_.rot = deg;
            }
            xform_changed();
            return;
        }
        case Drag::XScale: {
            // Pointer in the un-rotated, un-translated frame of the box.
            QTransform inv;
            inv.translate(xf_.pivot.x(), xf_.pivot.y());
            inv.rotate(-xf_.p_rot);
            inv.translate(-xf_.pivot.x() - xf_.p_tx, -xf_.pivot.y() - xf_.p_ty);
            const QPointF l = inv.map(cur_c_);
            const QRectF b = xf_.box;
            const QPointF hs[8] = {b.topLeft(),     QPointF(b.center().x(), b.top()),    b.topRight(),
                                   QPointF(b.right(), b.center().y()), b.bottomRight(), QPointF(b.center().x(), b.bottom()),
                                   b.bottomLeft(),  QPointF(b.left(), b.center().y())};
            const QPointF h = hs[xf_.handle];
            const bool ax = xf_.handle != 1 && xf_.handle != 5;
            const bool ay = xf_.handle != 3 && xf_.handle != 7;
            const auto fix = [](double v) {
                const double m = std::clamp(std::abs(v), 0.01, 1000.0);
                return v < 0 ? -m : m;
            };
            double sx = xf_.p_sx, sy = xf_.p_sy;
            if (ax && std::abs(h.x() - xf_.pivot.x()) > 1e-9) sx = fix((l.x() - xf_.pivot.x()) / (h.x() - xf_.pivot.x()));
            if (ay && std::abs(h.y() - xf_.pivot.y()) > 1e-9) sy = fix((l.y() - xf_.pivot.y()) / (h.y() - xf_.pivot.y()));
            if ((in.mods & Qt::ShiftModifier) && ax && ay) {
                const double s = std::max(std::abs(sx), std::abs(sy));
                sx = sx < 0 ? -s : s;
                sy = sy < 0 ? -s : s;
            }
            xf_.sx = sx;
            xf_.sy = sy;
            xform_changed();
            return;
        }
        case Drag::None:
            hover(in);
            return;
        default:
            request_update();
            return;
    }
}

void CanvasController::pointer_release(const PointerInput& in) {
    cur_c_ = to_canvas(in.pos);
    const Drag d = drag_;
    drag_ = Drag::None;
    switch (d) {
        case Drag::Pan: update_cursor(); break;
        case Drag::ZoomDrag: {
            const QRectF r = QRectF(press_w_, in.pos).normalized();
            if (r.width() < kZoomDragMin && r.height() < kZoomDragMin) {
                if (zoom_alt_) {
                    for (auto it = std::rbegin(kZoomSteps); it != std::rend(kZoomSteps); ++it)
                        if (*it < zoom_ * 0.9999) return set_zoom(*it, press_w_);
                } else {
                    for (double s : kZoomSteps)
                        if (s > zoom_ * 1.0001) return set_zoom(s, press_w_);
                }
                request_update();
                break;
            }
            zoom_to_widget_rect(r);
            break;
        }
        case Drag::Stroke:
            airbrush_timer_.stop();
            apply_stroke_update(stroke_.add_release_sample(sample_from(in)));
            finish_stroke();
            break;
        case Drag::Marquee: {
            const QRectF r = QRectF(press_c_, cur_c_).normalized();
            if (r.width() < 1.0 / zoom_ && r.height() < 1.0 / zoom_) {
                // A click without a drag: deselect (New mode only).
                if (gesture_sel_mode_ == SelMode::New && session_->state().selection.active())
                    apply_op({{"op", "deselect"}}, tr("Deselect"));
                break;
            }
            // Pixel-grid snapped when not anti-aliased, so a rect selects whole pixels.
            const bool ell = tools_->tool() == Tool::MarqueeEllipse;
            const bool aa = ell ? tools_->settings().ellipse_antialias : tools_->settings().marquee_antialias;
            QRectF s = r;
            if (!aa) s = QRectF(QPointF(std::round(r.left()), std::round(r.top())), QPointF(std::round(r.right()), std::round(r.bottom())));
            if (s.width() <= 0 || s.height() <= 0) break;
            apply_op({{"op", ell ? "select_ellipse" : "select_rect"}, {"x", s.x()}, {"y", s.y()}, {"w", s.width()},
                      {"h", s.height()}, {"antialias", aa}, {"mode", sel_mode_json(gesture_sel_mode_)}},
                     ell ? tr("Elliptical Marquee") : tr("Rectangular Marquee"));
            break;
        }
        case Drag::Lasso: {
            lasso_ << cur_c_;
            if (lasso_.size() < 3) break;
            // doc 30 §19: 3..4096 points; decimate evenly when a long drag produced more.
            QPolygonF pts = lasso_;
            if (pts.size() > 4096) {
                QPolygonF d2;
                const double step = static_cast<double>(pts.size()) / 4096.0;
                for (int i = 0; i < 4096; ++i) d2 << pts[static_cast<int>(i * step)];
                pts = d2;
            }
            Json arr = Json::array();
            for (const QPointF& p : pts) arr.push_back(Json::array({p.x(), p.y()}));
            apply_op({{"op", "select_polygon"}, {"points", arr}, {"antialias", tools_->settings().lasso_antialias},
                      {"mode", sel_mode_json(gesture_sel_mode_)}},
                     tr("Lasso"));
            lasso_.clear();
            break;
        }
        case Drag::Gradient: {
            if (QLineF(press_c_, cur_c_).length() <= 0.0) break;
            const ToolSettings& s = tools_->settings();
            apply_op({{"op", "gradient"}, {"layer", session_->active_layer()}, {"type", s.gradient_radial ? "radial" : "linear"},
                      {"p0", Json::array({press_c_.x(), press_c_.y()})}, {"p1", Json::array({cur_c_.x(), cur_c_.y()})},
                      {"c0", hex(tools_->fg())}, {"c1", hex(tools_->bg())}, {"opacity", s.gradient_opacity},
                      {"reverse", s.gradient_reverse}},
                     tr("Gradient"));
            break;
        }
        case Drag::Move: {
            const double dx = std::round(cur_c_.x() - press_c_.x()), dy = std::round(cur_c_.y() - press_c_.y());
            move_bounds_.reset();
            if (dx == 0.0 && dy == 0.0) break;
            apply_op({{"op", "transform"}, {"layer", session_->active_layer()}, {"interp", "nearest"},
                      {"translate", Json::array({dx, dy})}},
                     tr("Move"));
            break;
        }
        case Drag::Crop:
            if (crop_.width() < 1 || crop_.height() < 1) crop_ = QRectF();
            emit pending_changed();
            break;
        case Drag::XMove:
        case Drag::XScale:
        case Drag::XRotate:
        case Drag::XQuad:
            xf_dragging_ = false;
            xf_timer_.stop();
            update_xform_preview(true);
            break;
        default: break;
    }
    request_update();
}

void CanvasController::hover(const PointerInput& in) {
    hover_c_ = to_canvas(in.pos);
    const bool was = hover_inside_;
    hover_inside_ = true;
    emit cursor_moved(hover_c_, session_->has_document() &&
                                    QRectF(0, 0, session_->doc().width(), session_->doc().height()).contains(hover_c_));
    const Tool t = effective_tool();
    if (t == Tool::Brush || t == Tool::Eraser || t == Tool::Clone || poly_active_ || !was) request_update();
}

void CanvasController::pointer_left() {
    hover_inside_ = false;
    request_update();
}

void CanvasController::focus_lost() {
    // doc 40 §5: focus loss ends the stroke (commit).
    finish_stroke();
    set_temp_hand(false);
}

void CanvasController::wheel(QWheelEvent* e) {
    if (!session_->has_document()) return;
    const QPoint a = e->angleDelta();
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
        // Zoom at the cursor. Some platforms turn Alt+wheel into a horizontal wheel: use either axis.
        const int delta = a.y() != 0 ? a.y() : a.x();
        const double f = std::pow(1.0015, delta);
        set_zoom(zoom_ * f, e->position());
    } else if (e->modifiers() & Qt::ShiftModifier) {
        pan_by(QPointF(a.y() / 2.0, 0));
    } else {
        const QPoint px = e->pixelDelta();
        pan_by(px.isNull() ? QPointF(a.x() / 2.0, a.y() / 2.0) : QPointF(px));
    }
    e->accept();
}

bool CanvasController::key_press(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat() && !stroke_.active()) {
        set_temp_hand(true);
        return true;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (has_pending()) {
            commit_pending();
            return true;
        }
    }
    if (e->key() == Qt::Key_Escape && has_pending()) {
        cancel_pending();
        return true;
    }
    if (e->key() == Qt::Key_Backspace && poly_active_ && poly_.size() > 1) {
        poly_.removeLast();
        request_update();
        return true;
    }
    if (tools_->tool() == Tool::Move && !xf_.active) {
        int dx = 0, dy = 0;
        const int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
        switch (e->key()) {
            case Qt::Key_Left: dx = -step; break;
            case Qt::Key_Right: dx = step; break;
            case Qt::Key_Up: dy = -step; break;
            case Qt::Key_Down: dy = step; break;
            default: break;
        }
        if (dx || dy) {
            if (require_raster_layer(tr("Move")))
                apply_op({{"op", "transform"}, {"layer", session_->active_layer()}, {"interp", "nearest"},
                          {"translate", Json::array({dx, dy})}},
                         tr("Nudge"));
            return true;
        }
    }
    return false;
}

bool CanvasController::key_release(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        set_temp_hand(false);
        return true;
    }
    return false;
}

// ---- pending states ----------------------------------------------------------------------------------------------

bool CanvasController::has_pending() const { return poly_active_ || !crop_.isEmpty() || xf_.active; }

QRect CanvasController::crop_rect() const {
    if (crop_.isEmpty()) return {};
    return QRect(static_cast<int>(std::round(crop_.x())), static_cast<int>(std::round(crop_.y())),
                 std::max(1, static_cast<int>(std::round(crop_.width()))), std::max(1, static_cast<int>(std::round(crop_.height()))));
}

void CanvasController::commit_pending() {
    if (poly_active_) {
        poly_active_ = false;
        if (poly_.size() >= 3) {
            Json arr = Json::array();
            for (const QPointF& p : poly_) arr.push_back(Json::array({p.x(), p.y()}));
            apply_op({{"op", "select_polygon"}, {"points", arr}, {"antialias", tools_->settings().lasso_antialias},
                      {"mode", sel_mode_json(gesture_sel_mode_)}},
                     tr("Polygonal Lasso"));
        }
        poly_.clear();
    } else if (!crop_.isEmpty()) {
        const QRect r = crop_rect();
        crop_ = QRectF();
        apply_op({{"op", "crop"}, {"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()}}, tr("Crop"));
    } else if (xf_.active) {
        const auto m = xform_mat();
        const Xform x = xf_;
        xf_ = Xform();
        xf_timer_.stop();
        clear_override();
        if (tools_->tool() == Tool::Transform) tools_->set_tool(tools_->previous_tool());
        if (m && *m != rl::transform::kIdentity)
            apply_op(rl::geom::gui::transform_matrix_op(
                         x.layer, *m, tools_->settings().transform_bicubic ? rl::transform::Interp::Bicubic : rl::transform::Interp::Nearest),
                     x.quad_mode ? tr("Distort") : tr("Free Transform"));
    }
    emit pending_changed();
    request_update();
}

void CanvasController::cancel_pending() {
    poly_active_ = false;
    poly_.clear();
    crop_ = QRectF();
    if (xf_.active) {
        xf_ = Xform();
        xf_timer_.stop();
        clear_override();
        if (tools_->tool() == Tool::Transform) tools_->set_tool(tools_->previous_tool());
    }
    drag_ = Drag::None;
    emit pending_changed();
    request_update();
}

void CanvasController::begin_free_transform() {
    if (!session_->has_document() || !require_raster_layer(tr("Free Transform"))) return;
    finish_stroke();
    clear_override();
    xf_ = Xform();
    xf_.layer = session_->active_layer();
    std::optional<QRect> b = layer_bounds(xf_.layer);
    const QRect r = b ? *b : QRect(0, 0, session_->doc().width(), session_->doc().height());
    xf_.box = QRectF(r.x(), r.y(), r.width(), r.height());
    xf_.pivot = xf_.box.center();
    xf_.quad = {xf_.box.topLeft(), xf_.box.topRight(), xf_.box.bottomRight(), xf_.box.bottomLeft()};
    // The live preview works on a copy: the document is untouched until Enter.
    ov_ = session_->state();
    ov_node_ = find_node(ov_->root, xf_.layer);
    xf_src_ = ov_node_ ? ov_node_->pixels : rl::RgbaImage();
    xf_.active = true;
    tools_->set_tool(Tool::Transform);
    xf_.active = true;  // set_tool() -> on_tool_changed() keeps it (tool is Transform)
    emit pending_changed();
    emit message(tr("Free Transform: Ctrl+drag a corner distorts, Ctrl+Shift skews, Ctrl+Alt+Shift adds perspective. Enter applies."), false);
    request_update();
}

std::optional<rl::transform::Mat3> CanvasController::xform_mat() const {
    namespace tf = rl::transform;
    std::optional<tf::Mat3> m;
    if (!xf_.quad_mode) {
        tf::Params p;
        p.tx = xf_.tx;
        p.ty = xf_.ty;
        p.sx = xf_.sx;
        p.sy = xf_.sy;
        p.rotate = xf_.rot;
        p.px = xf_.pivot.x();
        p.py = xf_.pivot.y();
        m = tf::from_params(p);
    } else {
        tf::Quad q;
        q.rx = xf_.box.x();
        q.ry = xf_.box.y();
        q.rw = xf_.box.width();
        q.rh = xf_.box.height();
        for (size_t i = 0; i < 4; ++i) q.q[i] = {xf_.quad[i].x(), xf_.quad[i].y()};
        m = tf::from_quad(q);
    }
    if (!m || !tf::inverse(*m)) return std::nullopt;
    return m;
}

std::array<QPointF, 8> CanvasController::xform_handles_canvas() const {
    const QRectF b = xf_.box;
    const QPointF src[8] = {b.topLeft(),     QPointF(b.center().x(), b.top()),    b.topRight(),
                            QPointF(b.right(), b.center().y()), b.bottomRight(), QPointF(b.center().x(), b.bottom()),
                            b.bottomLeft(),  QPointF(b.left(), b.center().y())};
    std::array<QPointF, 8> out;
    const auto m = xform_mat();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < 8; ++i) {
        std::optional<rl::select::Pt> p;
        if (m) p = rl::geom::gui::map_point(*m, src[i].x(), src[i].y());
        out[i] = p ? QPointF(p->x, p->y) : QPointF(nan, nan);
    }
    return out;
}

QPolygonF CanvasController::xform_polygon_widget() const {
    const auto h = xform_handles_canvas();
    QPolygonF poly;
    for (size_t i : {0u, 2u, 4u, 6u}) poly << to_widget(h[i]);
    return poly;
}

int CanvasController::xform_hit(const QPointF& w) const {
    const auto h = xform_handles_canvas();
    for (int i = 0; i < 8; ++i)
        if (QLineF(to_widget(h[static_cast<size_t>(i)]), w).length() <= 7.0) return i;
    if (xform_polygon_widget().containsPoint(w, Qt::OddEvenFill)) return 8;
    return -1;
}

void CanvasController::to_quad_mode() {
    if (xf_.quad_mode) return;
    const auto h = xform_handles_canvas();
    xf_.quad = {h[0], h[2], h[4], h[6]};
    xf_.quad_mode = true;
}

void CanvasController::drag_quad(const QPointF& cur, QuadDrag kind) {
    const QPointF d = cur - xf_.press;
    std::array<QPointF, 4> q = xf_.p_quad;
    const auto unit = [](const QPointF& v) {
        const double l = std::hypot(v.x(), v.y());
        return l > 1e-12 ? v / l : QPointF(1, 0);
    };
    const auto dot = [](const QPointF& a, const QPointF& b) { return a.x() * b.x() + a.y() * b.y(); };
    const int h = xf_.handle;
    if (h % 2 == 0) {  // a corner
        const size_t c = static_cast<size_t>(h / 2);
        if (kind == QuadDrag::Distort) {
            q[c] = xf_.p_quad[c] + d;
        } else {
            // Constrain to the adjacent edge that best follows the drag.
            const size_t n1 = (c + 1) % 4, n2 = (c + 3) % 4;
            const QPointF e1 = unit(xf_.p_quad[n1] - xf_.p_quad[c]), e2 = unit(xf_.p_quad[n2] - xf_.p_quad[c]);
            const bool first = std::abs(dot(d, e1)) >= std::abs(dot(d, e2));
            const QPointF e = first ? e1 : e2;
            const size_t n = first ? n1 : n2;
            const QPointF proj = e * dot(d, e);
            q[c] = xf_.p_quad[c] + proj;
            if (kind == QuadDrag::Perspective) q[n] = xf_.p_quad[n] - proj;  // the partner mirrors: a vanishing point
        }
    } else {  // an edge midpoint: its two corners
        const size_t a = static_cast<size_t>((h - 1) / 2), b = (a + 1) % 4;
        QPointF m = d;
        if (kind != QuadDrag::Distort) {
            const QPointF e = unit(xf_.p_quad[b] - xf_.p_quad[a]);
            m = e * dot(d, e);  // skew: slide the edge along itself
        }
        q[a] = xf_.p_quad[a] + m;
        q[b] = xf_.p_quad[b] + m;
    }
    // Keep the quad convex (a folded quad has no sensible inverse mapping).
    double sign = 0.0;
    for (size_t i = 0; i < 4; ++i) {
        const QPointF u = q[(i + 1) % 4] - q[i], v = q[(i + 2) % 4] - q[(i + 1) % 4];
        const double cr = u.x() * v.y() - u.y() * v.x();
        if (std::abs(cr) < 1e-9 || (sign != 0.0 && (cr > 0) != (sign > 0))) return;
        sign = cr;
    }
    const std::array<QPointF, 4> keep = xf_.quad;
    xf_.quad = q;
    if (!xform_mat()) xf_.quad = keep;
}

void CanvasController::xform_changed() {
    request_update();
    if (!xf_timer_.isActive()) xf_timer_.start();
}

void CanvasController::invalidate_override_tiles() {
    for (const QPoint& t : ov_tiles_) invalidate_tile(t.x(), t.y());
}

void CanvasController::update_xform_preview(bool final_quality) {
    if (!xf_.active || !ov_node_) return;
    const auto m = xform_mat();
    if (!m) return;
    namespace tf = rl::transform;
    tf::Interp interp = tools_->settings().transform_bicubic ? tf::Interp::Bicubic : tf::Interp::Nearest;
    // While dragging a large layer, a nearest-neighbour draft keeps the handles responsive; the
    // release renders the exact op result (what Enter commits).
    const double px = static_cast<double>(xf_src_.width()) * xf_src_.height();
    if (!final_quality && interp == tf::Interp::Bicubic && px > 1.5e6) interp = tf::Interp::Nearest;
    QElapsedTimer t;
    t.start();
    std::optional<rl::RgbaImage> out = tf::transform_image(xf_src_, *m, interp);
    xf_preview_ms_ = static_cast<double>(t.nsecsElapsed()) / 1e6;
    if (!out) return;
    const bool dense_bg = out->background().a != 0 || ov_node_->pixels.background().a != 0;
    std::vector<rl::TileKey> keys = ov_node_->pixels.sorted_keys();
    const std::vector<rl::TileKey> nk = out->sorted_keys();
    keys.insert(keys.end(), nk.begin(), nk.end());
    ov_node_->pixels = std::move(*out);
    if (dense_bg) {
        for (int ty = 0; ty < tiles_y_; ++ty)
            for (int tx = 0; tx < tiles_x_; ++tx) keys.push_back({tx, ty});
    }
    for (const rl::TileKey& k : keys) {
        if (k.tx < 0 || k.ty < 0 || k.tx >= tiles_x_ || k.ty >= tiles_y_) continue;
        invalidate_tile(k.tx, k.ty);
        ov_tiles_.emplace_back(k.tx, k.ty);
    }
    // Bound the list: duplicates only cost a re-invalidate.
    if (ov_tiles_.size() > static_cast<size_t>(4 * tiles_x_ * tiles_y_ + 64)) {
        std::sort(ov_tiles_.begin(), ov_tiles_.end(), [](const QPoint& a, const QPoint& b) { return a.y() != b.y() ? a.y() < b.y() : a.x() < b.x(); });
        ov_tiles_.erase(std::unique(ov_tiles_.begin(), ov_tiles_.end()), ov_tiles_.end());
    }
    request_update();
}

}  // namespace rl::gui
