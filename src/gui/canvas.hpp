// SPDX-License-Identifier: GPL-3.0-or-later
//
// CanvasController: everything about the canvas except how pixels reach the screen.
//
// - View transform (zoom, pan, fit) in double precision; canvas <-> widget mapping.
// - Display tile cache: each 64x64 tile is recomposited by the core (composite::render_tile) only
//   when a diff marks it dirty, and kept as a premultiplied display copy (BUILD-SPEC D2 (b): the
//   one place outside neighbour-mixing ops where premultiplication is allowed; display only).
// - Tools: turns pointer gestures into ONE render-script op each, executed by the EditorSession.
// - Overlays (selection edges, stroke preview, marquee/lasso/crop/gradient/transform guides),
//   painted with QPainter by both backends.
//
// The two display backends (canvas_widgets.hpp) are thin: they forward input here and paint either
// with QPainter (raster fallback) or GL texture arrays (canvas_gl.hpp).
#pragma once

#include <QColor>
#include <QImage>
#include <QLineF>
#include <QObject>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "core/doc/document.hpp"
#include "core/transform/transform.hpp"
#include "core/tile/tiled_image.hpp"
#include "gui/state_diff.hpp"
#include "gui/stroke_adapter.hpp"
#include "gui/tools.hpp"

class QKeyEvent;
class QPainter;
class QWheelEvent;
class QWidget;

namespace rl::gui {

class EditorSession;

struct PointerInput {
    QPointF pos;  // widget logical coordinates, sub-pixel
    double pressure = 1.0;
    double tilt_x = 0.0;
    double tilt_y = 0.0;
    double t_ms = 0.0;  // event timestamp (ms)
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons;
    Qt::KeyboardModifiers mods;
    bool tablet = false;
    bool eraser_end = false;
    bool double_click = false;
};

class CanvasController : public QObject {
    Q_OBJECT
public:
    CanvasController(EditorSession* session, ToolState* tools, QObject* parent = nullptr);
    ~CanvasController() override;

    void set_widget(QWidget* w);
    // Rebuilds the tile cache and view for the session's current document (normally driven by the
    // session's document_reset signal).
    void sync_document() { on_document_reset(); }
    QWidget* widget() const { return widget_; }
    void request_update();
    void request_update_canvas_rect(const QRectF& canvas_rect);

    // ---- view ----------------------------------------------------------------------------------
    double zoom() const { return zoom_; }
    QPointF offset() const { return off_; }
    void set_zoom(double z, const QPointF& widget_anchor);
    void zoom_in();
    void zoom_out();
    void fit_to_view();
    // Zoom so that a widget-space rectangle fills the view (Zoom tool drag).
    void zoom_to_widget_rect(const QRectF& widget_rect);
    void actual_pixels();
    void center_on(const QPointF& canvas_pt);
    void pan_by(const QPointF& widget_delta);
    QPointF to_canvas(const QPointF& w) const { return (w - off_) / zoom_; }
    QPointF to_widget(const QPointF& c) const { return c * zoom_ + off_; }
    QRectF canvas_rect_widget() const;
    QRectF visible_canvas_rect() const;
    void viewport_resized();

    // ---- display tiles --------------------------------------------------------------------------
    QSize doc_size() const;
    int tiles_x() const { return tiles_x_; }
    int tiles_y() const { return tiles_y_; }
    // Level-0 display tile (the exact composite, Format_ARGB32_Premultiplied), rendered now if dirty.
    const QImage& display_tile(int tx, int ty);
    quint64 tile_generation(int tx, int ty) const { return tile_generation(0, tx, ty); }
    quint64 layout_generation() const { return layout_gen_; }
    // The straight (unpremultiplied) composite pixel at a canvas position (eyedropper).
    std::optional<QColor> composite_pixel(int x, int y);
    // Visible tile index range at level 0.
    QRect visible_tile_range() const { return visible_tile_range(0); }

    // ---- display mip pyramid (zoom < 100 %) ----
    // Level L holds the composite box-filtered 2^L : 1 (premultiplied 2x2 means of level L-1), in
    // 64 x 64 tiles; a level-L tile covers 64 * 2^L canvas pixels per side. Display only: nothing
    // reads it back, so it is outside the byte-exactness contract.
    static int level_for_zoom(double zoom);
    int display_level() const { return std::min(level_for_zoom(zoom_), std::max(0, level_count() - 1)); }
    int level_count() const { return static_cast<int>(levels_.size()); }
    int level_tiles_x(int level) const { return levels_[static_cast<size_t>(level)].tx; }
    int level_tiles_y(int level) const { return levels_[static_cast<size_t>(level)].ty; }
    int level_offset(int level) const { return levels_[static_cast<size_t>(level)].offset; }
    int total_level_tiles() const;
    QRect visible_tile_range(int level) const;
    QRectF level_tile_canvas_rect(int level, int tx, int ty) const;  // clipped to the canvas
    // Budgeted access used while painting: may return a stale image, or nullptr when nothing is
    // ready yet (the frame then schedules a follow-up until display_complete()).
    const QImage* level_tile(int level, int tx, int ty);
    quint64 tile_generation(int level, int tx, int ty) const;
    bool display_complete() const { return complete_; }
    // Renders everything visible at the current level now (tests, grabs): no budget.
    void finish_display();
    // Budget for tile rendering outside a paint (the Navigator); 0 = unlimited.
    void set_render_budget_ms(double ms);
    // The state the canvas shows: the document, or a live preview of it (stroke, transform).
    const rl::DocState& display_state() const;

    void paint_raster(QPainter& p, const QRect& update_rect);
    void paint_overlays(QPainter& p);
    void frame_begin();
    void frame_end();

    // ---- input ------------------------------------------------------------------------------------
    void pointer_press(const PointerInput& in);
    void pointer_move(const PointerInput& in);
    void pointer_release(const PointerInput& in);
    void hover(const PointerInput& in);
    bool key_press(QKeyEvent* e);
    bool key_release(QKeyEvent* e);
    void wheel(QWheelEvent* e);
    void focus_lost();
    void pointer_left();
    bool stroke_active() const { return stroke_.active(); }
    void commit_stroke_if_any() { finish_stroke(); }
    bool tablet_down() const { return tablet_down_; }
    void set_tablet_down(bool v) { tablet_down_ = v; }

    // ---- pending tool states (crop, polygon lasso, free transform) ----------------------------------
    bool has_pending() const;
    void commit_pending();
    void cancel_pending();
    void begin_free_transform();
    bool transform_quad_mode() const { return xf_.active && xf_.quad_mode; }
    // The transform box's corners TL, TR, BR, BL in canvas coordinates (as displayed).
    std::array<QPointF, 4> transform_corners() const {
        const auto h = xform_handles_canvas();
        return {h[0], h[2], h[4], h[6]};
    }
    double last_transform_preview_ms() const { return xf_preview_ms_; }
    // Test hook: the live preview's pixels of the transformed layer (nullptr when none).
    const rl::RgbaImage* transform_preview_pixels() const { return xf_.active && ov_node_ ? &ov_node_->pixels : nullptr; }
    QRect crop_rect() const;

    // Clone stamp tool state (doc 40 §4: tool state, not document state).
    std::optional<QPointF> clone_source() const { return clone_src_; }

    const StrokeAdapter& stroke() const { return stroke_; }

signals:
    void view_changed();
    void display_progress();  // a frame rendered tiles (the Navigator follows)
    void cursor_moved(QPointF canvas_pos, bool inside);
    void color_sampled(const QColor& c, bool to_background);
    void message(const QString& text, bool warning);
    void pending_changed();

private slots:
    void on_document_reset();
    void on_tiles_dirty(const rl::gui::StateDiff& d);
    void on_selection_changed();
    void on_tool_changed();
    void on_airbrush_tick();

private:
    struct CachedTile {
        QImage img;
        bool valid = false;
        quint64 gen = 0;
    };
    struct Level {
        int tx = 0, ty = 0;  // tiles
        int w = 0, h = 0;    // pixels at this level
        int offset = 0;      // tiles in all lower levels
        std::vector<CachedTile> tiles;
    };
    enum class Drag { None, Pan, Stroke, Marquee, Lasso, Gradient, Move, Crop, CropMove, XMove, XScale, XRotate, XQuad, ZoomDrag };
    struct Xform {
        bool active = false;
        std::string layer;
        QRectF box;          // canvas rect being transformed (the layer's content bounds)
        // Params form (move / scale / rotate about the pivot) until a corner is distorted; then the
        // quad form (the destination corners TL, TR, BR, BL of `box`, doc 30 §12.2).
        bool quad_mode = false;
        QPointF pivot;
        double tx = 0.0, ty = 0.0, sx = 1.0, sy = 1.0, rot = 0.0;
        std::array<QPointF, 4> quad;
        // Drag state.
        int handle = -1;
        QPointF press;
        double p_tx = 0, p_ty = 0, p_sx = 1, p_sy = 1, p_rot = 0;
        std::array<QPointF, 4> p_quad;
        Qt::KeyboardModifiers mods;
    };
    enum class QuadDrag { Distort, Skew, Perspective };
    bool require_raster_layer(const QString& what);
    std::optional<QRect> layer_bounds(const std::string& id) const;
    std::optional<rl::transform::Mat3> xform_mat() const;  // nullopt: degenerate
    std::array<QPointF, 8> xform_handles_canvas() const;   // corners TL,T,TR,R,BR,B,BL,L mapped
    QPolygonF xform_polygon_widget() const;
    int xform_hit(const QPointF& w) const;  // 0..7 handle, 8 inside, -1 outside
    void xform_changed();                   // schedules the live preview
    void update_xform_preview(bool final_quality);
    void to_quad_mode();
    void drag_quad(const QPointF& cur, QuadDrag kind);
    void invalidate_override_tiles();
    void apply_op(const Json& op, const QString& label);
    void build_levels();
    void invalidate_tile(int tx, int ty);
    void invalidate_all();
    bool over_budget() const;
    void render_level0(int tx, int ty, CachedTile& ct);
    void downsample_into(int level, int tx, int ty, CachedTile& ct, bool& all_valid);
    void finish_stroke();
    void apply_stroke_update(const StrokeUpdate& u);
    void clear_override();
    void rebuild_selection_edges();
    void start_stroke(const PointerInput& in, Tool t);
    StrokeSample sample_from(const PointerInput& in) const;
    void set_temp_hand(bool on);
    Tool effective_tool() const;
    void update_cursor();
    void clamp_offset();

    EditorSession* session_;
    ToolState* tools_;
    QWidget* widget_ = nullptr;

    double zoom_ = 1.0;
    bool zoom_alt_ = false;                    // Zoom tool press had Alt (zoom out on a click)
    static constexpr double kZoomDragMin = 6;  // px: a smaller Zoom-tool drag counts as a click
    QPointF off_{0, 0};
    bool fitted_ = true;  // re-fit on resize until the user zooms

    int tiles_x_ = 0, tiles_y_ = 0;
    std::vector<Level> levels_;
    bool complete_ = true;
    qint64 deadline_ns_ = -1;  // < 0: no budget
    double budget_ms_ = 0.0;
    std::optional<rl::DocState> ov_;  // display override (live stroke / transform preview)
    rl::Node* ov_node_ = nullptr;
    std::vector<QPoint> ov_tiles_;     // tiles the override has changed (re-dirtied when it ends)
    rl::RgbaImage xf_src_;             // the layer's pixels when Free Transform began
    QTimer xf_timer_;                  // coalesces live-preview updates
    bool xf_dragging_ = false;
    double xf_preview_ms_ = 0.0;
    rl::DocState stroke_before_;
    quint64 gen_counter_ = 0;
    quint64 layout_gen_ = 1;
    rl::RgbaTile scratch_;
    int frame_tiles_ = 0;
    qint64 frame_start_ns_ = 0;

    QVector<QLineF> sel_edges_;
    QTimer ants_timer_;
    int ants_phase_ = 0;

    // gesture state
    Drag drag_ = Drag::None;
    QPointF press_w_, press_c_, last_w_, cur_c_;
    Qt::KeyboardModifiers press_mods_;
    SelMode gesture_sel_mode_ = SelMode::New;
    QPolygonF lasso_;
    QPolygonF poly_;          // polygon lasso vertices (canvas)
    bool poly_active_ = false;
    std::optional<QRect> move_bounds_;
    QRectF crop_;             // pending crop (canvas), empty = none
    Xform xf_;
    bool temp_hand_ = false;
    bool tablet_down_ = false;
    bool hover_inside_ = false;
    QPointF hover_c_;

    StrokeAdapter stroke_;
    double stroke_t0_ = 0.0;
    qint64 stroke_wall0_ns_ = 0;
    QTimer airbrush_timer_;
    qint64 airbrush_wall0_ = 0;
    std::optional<QPointF> clone_src_;
    std::optional<QPoint> clone_off_;
};

}  // namespace rl::gui
