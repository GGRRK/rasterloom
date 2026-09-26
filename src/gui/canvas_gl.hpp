// SPDX-License-Identifier: GPL-3.0-or-later
//
// GL 3.3 core canvas (docs/research/qt-platform.md section 2): premultiplied display tiles are
// uploaded into GL_TEXTURE_2D_ARRAY pages (256 tiles of 64x64 per page, free-slot list, one
// instanced draw call per page), only when a tile's generation changed; GL_NEAREST at >= 100 %
// zoom; checkerboard from gl_FragCoord (fixed in screen space); blending GL_ONE,
// GL_ONE_MINUS_SRC_ALPHA. Overlays are painted with QPainter on top, shared with the raster path.
#pragma once

#include <QOpenGLWidget>
#include <QString>

#include <vector>

#include "gui/canvas.hpp"

class QOpenGLContext;
class QOpenGLFunctions_3_3_Core;
class QOpenGLShaderProgram;

namespace rl::gui {

class CanvasController;

struct GlProbe {
    bool ok = false;
    QString vendor, renderer, version;
    QString reason;
};

// Standalone context + offscreen surface probe: 3.3 core required; a software renderer
// (llvmpipe / softpipe / swiftshader) additionally has to pass an upload-throughput check.
GlProbe probe_gl();

class CanvasGLWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit CanvasGLWidget(CanvasController* c, QWidget* parent = nullptr);
    ~CanvasGLWidget() override;
    bool gl_ok() const { return ok_; }
    bool initialized() const { return initialized_; }

    // Test hooks: draw the canvas (workspace, checker, tiles; no QPainter overlays) with GL into
    // whatever framebuffer is bound in `ctx`, without a window (offscreen QPA has no GL widgets).
    bool init_for_test(QOpenGLContext* ctx) { return init_resources(ctx); }
    void render_for_test(int w, int h) {
        c_->frame_begin();
        render_gl(w, h, 1.0);
        c_->frame_end();
    }
    void cleanup_for_test() { cleanup(); }

signals:
    void gl_failed(const QString& reason);

protected:
    void initializeGL() override;
    void paintGL() override;
    bool event(QEvent* e) override;
    void resizeEvent(class QResizeEvent* e) override;

private:
    struct Slot {
        int page = -1;
        int layer = -1;
        quint64 gen = 0;
    };
    void cleanup();
    void release_context();
    bool init_resources(QOpenGLContext* ctx);
    void render_gl(int width, int height, qreal dpr);
    void release_all_slots();
    Slot& slot_for(int tile_index);

    CanvasController* c_;
    QOpenGLFunctions_3_3_Core* f_ = nullptr;
    QOpenGLShaderProgram* tile_prog_ = nullptr;
    QOpenGLShaderProgram* rect_prog_ = nullptr;
    unsigned vao_ = 0, quad_vbo_ = 0, inst_vbo_ = 0;
    std::vector<unsigned> pages_;
    std::vector<Slot> slots_;
    std::vector<std::pair<int, int>> free_;
    quint64 layout_seen_ = 0;
    bool ok_ = false;
    bool initialized_ = false;
};

}  // namespace rl::gui
