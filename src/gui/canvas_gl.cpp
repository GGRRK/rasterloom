// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/canvas_gl.hpp"

#include <QElapsedTimer>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>
#include <QPainter>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSurfaceFormat>

#include <cmath>
#include <vector>

#include "core/composite/render.hpp"
#include "gui/canvas.hpp"
#include "gui/canvas_widgets.hpp"
#include "gui/diagnostics.hpp"
#include "gui/theme.hpp"

namespace rl::gui {

namespace {

constexpr int kLayersPerPage = 256;
constexpr int kTile = 64;

QSurfaceFormat core33() {
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    return f;
}

const char* kTileVs = R"(#version 330 core
layout(location = 0) in vec2 corner;
layout(location = 1) in vec4 tile;   // canvas x0, y0, nx, ny
layout(location = 2) in float layer;
uniform vec2 u_off;
uniform float u_zoom;
uniform vec2 u_view;
uniform float u_scale;  // canvas pixels per texel of the pyramid level (2^level)
out vec3 v_uv;
void main() {
    vec2 c = tile.xy + corner * tile.zw;
    vec2 w = c * u_zoom + u_off;
    gl_Position = vec4(w.x / u_view.x * 2.0 - 1.0, 1.0 - w.y / u_view.y * 2.0, 0.0, 1.0);
    v_uv = vec3(corner * tile.zw / (64.0 * u_scale), layer);
}
)";

const char* kTileFs = R"(#version 330 core
uniform sampler2DArray u_tex;
in vec3 v_uv;
out vec4 o;
void main() { o = texture(u_tex, v_uv); }
)";

const char* kRectVs = R"(#version 330 core
layout(location = 0) in vec2 corner;
uniform vec4 u_rect;   // widget logical x, y, w, h
uniform vec2 u_view;
void main() {
    vec2 w = u_rect.xy + corner * u_rect.zw;
    gl_Position = vec4(w.x / u_view.x * 2.0 - 1.0, 1.0 - w.y / u_view.y * 2.0, 0.0, 1.0);
}
)";

const char* kCheckerFs = R"(#version 330 core
uniform float u_cell;  // device pixels
uniform float u_fbh;   // framebuffer height: cells counted from the top, like the raster path
uniform vec3 u_c1;
uniform vec3 u_c2;
out vec4 o;
void main() {
    vec2 p = floor(vec2(gl_FragCoord.x, u_fbh - gl_FragCoord.y) / u_cell);
    float k = mod(p.x + p.y, 2.0);
    o = vec4(mix(u_c2, u_c1, k), 1.0);  // dark cell at the origin, as in the raster path
}
)";

QVector3D rgb(const QColor& c) { return QVector3D(float(c.redF()), float(c.greenF()), float(c.blueF())); }

}  // namespace

GlProbe probe_gl() {
    GlProbe r;
    QOpenGLContext ctx;
    ctx.setFormat(core33());
    if (!ctx.create()) {
        r.reason = QStringLiteral("no OpenGL context");
        return r;
    }
    QOffscreenSurface surf;
    surf.setFormat(ctx.format());
    surf.create();
    if (!ctx.makeCurrent(&surf)) {
        r.reason = QStringLiteral("cannot make the GL context current");
        return r;
    }
    auto* gl = ctx.functions();
    r.vendor = QString::fromLatin1(reinterpret_cast<const char*>(gl->glGetString(GL_VENDOR)));
    r.renderer = QString::fromLatin1(reinterpret_cast<const char*>(gl->glGetString(GL_RENDERER)));
    r.version = QString::fromLatin1(reinterpret_cast<const char*>(gl->glGetString(GL_VERSION)));
    const QSurfaceFormat f = ctx.format();
    if (f.majorVersion() * 10 + f.minorVersion() < 33 || f.profile() != QSurfaceFormat::CoreProfile) {
        r.reason = QStringLiteral("GL %1.%2 core not available").arg(f.majorVersion()).arg(f.minorVersion());
        ctx.doneCurrent();
        return r;
    }
    static const QRegularExpression sw(QStringLiteral("llvmpipe|softpipe|swiftshader"), QRegularExpression::CaseInsensitiveOption);
    if (sw.match(r.renderer).hasMatch()) {
        // Software renderer: measure 256 tile uploads; below ~8 k tiles/s the raster path wins.
        auto* f33 = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(&ctx);
        if (!f33 || !f33->initializeOpenGLFunctions()) {
            r.reason = QStringLiteral("GL 3.3 functions unavailable");
            ctx.doneCurrent();
            return r;
        }
        GLuint tex = 0;
        f33->glGenTextures(1, &tex);
        f33->glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        f33->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kTile, kTile, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        std::vector<unsigned char> px(kTile * kTile * 4, 128);
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 256; ++i)
            f33->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, kTile, kTile, 1, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        f33->glFinish();
        const double ms = static_cast<double>(t.nsecsElapsed()) / 1e6;
        f33->glDeleteTextures(1, &tex);
        if (ms > 32.0) {
            r.reason = QStringLiteral("software GL too slow (%1 ms for 256 tile uploads)").arg(ms, 0, 'f', 1);
            ctx.doneCurrent();
            return r;
        }
        r.reason = QStringLiteral("software GL, %1 ms for 256 tile uploads").arg(ms, 0, 'f', 1);
    } else {
        r.reason = QStringLiteral("GL 3.3 core probe passed");
    }
    ctx.doneCurrent();
    r.ok = true;
    return r;
}

CanvasGLWidget::CanvasGLWidget(CanvasController* c, QWidget* parent) : QOpenGLWidget(parent), c_(c) {
    setFormat(core33());
    canvas_input::setup(this);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

CanvasGLWidget::~CanvasGLWidget() {
    if (initialized_) {
        makeCurrent();
        cleanup();
        doneCurrent();
    }
}

void CanvasGLWidget::cleanup() {
    if (!f_) return;
    if (!pages_.empty()) f_->glDeleteTextures(static_cast<GLsizei>(pages_.size()), pages_.data());
    pages_.clear();
    if (inst_vbo_) f_->glDeleteBuffers(1, &inst_vbo_);
    if (quad_vbo_) f_->glDeleteBuffers(1, &quad_vbo_);
    if (vao_) f_->glDeleteVertexArrays(1, &vao_);
    vao_ = quad_vbo_ = inst_vbo_ = 0;
    delete tile_prog_;
    delete rect_prog_;
    tile_prog_ = rect_prog_ = nullptr;
    slots_.clear();
    free_.clear();
    f_ = nullptr;
}

void CanvasGLWidget::release_context() {
    makeCurrent();
    cleanup();
    doneCurrent();
}

void CanvasGLWidget::initializeGL() {
    initialized_ = true;
    // Reparenting destroys the context; re-upload everything when it comes back.
    // A member function, not a lambda: Qt rejects Qt::UniqueConnection for functors and would
    // silently skip the connection.
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &CanvasGLWidget::release_context,
            Qt::UniqueConnection);
    init_resources(context());
}

bool CanvasGLWidget::init_resources(QOpenGLContext* ctx) {
    f_ = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(ctx);
    if (!f_ || !f_->initializeOpenGLFunctions()) {
        f_ = nullptr;
        ok_ = false;
        emit gl_failed(QStringLiteral("GL 3.3 core functions unavailable in the widget context"));
        return false;
    }
    tile_prog_ = new QOpenGLShaderProgram;
    rect_prog_ = new QOpenGLShaderProgram;
    bool good = tile_prog_->addShaderFromSourceCode(QOpenGLShader::Vertex, kTileVs) &&
                tile_prog_->addShaderFromSourceCode(QOpenGLShader::Fragment, kTileFs) && tile_prog_->link() &&
                rect_prog_->addShaderFromSourceCode(QOpenGLShader::Vertex, kRectVs) &&
                rect_prog_->addShaderFromSourceCode(QOpenGLShader::Fragment, kCheckerFs) && rect_prog_->link();
    if (!good) {
        ok_ = false;
        emit gl_failed(QStringLiteral("shader compile/link failed: %1 %2").arg(tile_prog_->log(), rect_prog_->log()));
        return false;
    }
    f_->glGenVertexArrays(1, &vao_);
    f_->glBindVertexArray(vao_);
    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    f_->glGenBuffers(1, &quad_vbo_);
    f_->glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    f_->glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    f_->glEnableVertexAttribArray(0);
    f_->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    f_->glGenBuffers(1, &inst_vbo_);
    f_->glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
    f_->glEnableVertexAttribArray(1);
    f_->glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    f_->glVertexAttribDivisor(1, 1);
    f_->glEnableVertexAttribArray(2);
    f_->glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(4 * sizeof(float)));
    f_->glVertexAttribDivisor(2, 1);
    f_->glBindVertexArray(0);
    f_->glBindBuffer(GL_ARRAY_BUFFER, 0);
    ok_ = true;
    layout_seen_ = 0;
    return true;
}

void CanvasGLWidget::release_all_slots() {
    free_.clear();
    for (int p = static_cast<int>(pages_.size()) - 1; p >= 0; --p)
        for (int l = kLayersPerPage - 1; l >= 0; --l) free_.emplace_back(p, l);
    slots_.assign(static_cast<size_t>(c_->total_level_tiles()), Slot{});
}

CanvasGLWidget::Slot& CanvasGLWidget::slot_for(int idx) {
    Slot& s = slots_[static_cast<size_t>(idx)];
    if (s.page >= 0) return s;
    if (free_.empty()) {
        GLuint tex = 0;
        f_->glGenTextures(1, &tex);
        f_->glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        f_->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kTile, kTile, kLayersPerPage, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        f_->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f_->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        pages_.push_back(tex);
        const int p = static_cast<int>(pages_.size()) - 1;
        for (int l = kLayersPerPage - 1; l >= 0; --l) free_.emplace_back(p, l);
    }
    s.page = free_.back().first;
    s.layer = free_.back().second;
    s.gen = 0;
    free_.pop_back();
    return s;
}

void CanvasGLWidget::paintGL() {
    c_->frame_begin();
    if (!ok_ || !f_) {
        QPainter p(this);
        p.fillRect(rect(), theme::kWorkspace);
        p.setPen(theme::kWarn);
        p.drawText(rect(), Qt::AlignCenter, tr("OpenGL canvas unavailable"));
        c_->frame_end();
        return;
    }
    render_gl(width(), height(), devicePixelRatioF());
    QPainter p(this);
    c_->paint_overlays(p);
    p.end();
    c_->frame_end();
}

void CanvasGLWidget::render_gl(int width, int height, qreal dpr) {
    const QColor ws = theme::kWorkspace;
    f_->glClearColor(float(ws.redF()), float(ws.greenF()), float(ws.blueF()), 1.0f);
    f_->glClear(GL_COLOR_BUFFER_BIT);
    const QRectF cr = c_->canvas_rect_widget();
    if (!cr.isEmpty()) {
        if (layout_seen_ != c_->layout_generation()) {
            layout_seen_ = c_->layout_generation();
            release_all_slots();
        }
        f_->glBindVertexArray(vao_);
        // Checkerboard under the canvas.
        rect_prog_->bind();
        rect_prog_->setUniformValue("u_rect", QVector4D(float(cr.x()), float(cr.y()), float(cr.width()), float(cr.height())));
        rect_prog_->setUniformValue("u_view", QVector2D(float(width), float(height)));
        rect_prog_->setUniformValue("u_cell", float(theme::kCheckerCell * dpr));
        rect_prog_->setUniformValue("u_fbh", float(height * dpr));
        rect_prog_->setUniformValue("u_c1", rgb(theme::kChecker1));
        rect_prog_->setUniformValue("u_c2", rgb(theme::kChecker2));
        f_->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        rect_prog_->release();

        // Upload dirty visible tiles of the display level (the CPU mip pyramid below 100 %), group
        // instances per page. Slots are per (level, tile); a level switch re-uses what was uploaded.
        const int level = c_->display_level();
        const QRect tr = c_->visible_tile_range(level);
        std::vector<std::vector<float>> inst(pages_.size() + 8);
        for (int ty = tr.top(); ty <= tr.bottom() && !tr.isEmpty(); ++ty) {
            for (int tx = tr.left(); tx <= tr.right(); ++tx) {
                const QImage* img = c_->level_tile(level, tx, ty);
                if (!img) continue;
                const quint64 gen = c_->tile_generation(level, tx, ty);
                Slot& s = slot_for(c_->level_offset(level) + ty * c_->level_tiles_x(level) + tx);
                if (s.gen != gen) {
                    f_->glBindTexture(GL_TEXTURE_2D_ARRAY, pages_[static_cast<size_t>(s.page)]);
                    f_->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    // Format_ARGB32_Premultiplied = one 0xAARRGGBB word per pixel on any byte order.
                    f_->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, s.layer, kTile, kTile, 1, GL_BGRA,
                                        GL_UNSIGNED_INT_8_8_8_8_REV, img->constBits());
                    s.gen = gen;
                }
                const QRectF t = c_->level_tile_canvas_rect(level, tx, ty);
                if (static_cast<size_t>(s.page) >= inst.size()) inst.resize(static_cast<size_t>(s.page) + 1);
                auto& v = inst[static_cast<size_t>(s.page)];
                v.insert(v.end(), {float(t.x()), float(t.y()), float(t.width()), float(t.height()), float(s.layer)});
            }
        }
        f_->glEnable(GL_BLEND);
        f_->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied display copies
        tile_prog_->bind();
        tile_prog_->setUniformValue("u_off", QVector2D(float(c_->offset().x()), float(c_->offset().y())));
        tile_prog_->setUniformValue("u_zoom", float(c_->zoom()));
        tile_prog_->setUniformValue("u_view", QVector2D(float(width), float(height)));
        tile_prog_->setUniformValue("u_tex", 0);
        tile_prog_->setUniformValue("u_scale", float(1 << level));
        f_->glActiveTexture(GL_TEXTURE0);
        const bool exact = std::abs(c_->zoom() * double(1 << level) - 1.0) < 1e-9;
        const GLint filter = c_->zoom() >= 1.0 || exact ? GL_NEAREST : GL_LINEAR;
        for (size_t p = 0; p < pages_.size() && p < inst.size(); ++p) {
            if (inst[p].empty()) continue;
            f_->glBindTexture(GL_TEXTURE_2D_ARRAY, pages_[p]);
            f_->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, filter);
            f_->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, filter);
            f_->glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
            f_->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(inst[p].size() * sizeof(float)), inst[p].data(), GL_STREAM_DRAW);
            f_->glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(inst[p].size() / 5));
        }
        tile_prog_->release();
        f_->glDisable(GL_BLEND);
        f_->glBindBuffer(GL_ARRAY_BUFFER, 0);
        f_->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        f_->glBindVertexArray(0);
    }
}

bool CanvasGLWidget::event(QEvent* e) {
    if (canvas_input::handle(this, c_, e)) return true;
    return QOpenGLWidget::event(e);
}

void CanvasGLWidget::resizeEvent(QResizeEvent* e) {
    QOpenGLWidget::resizeEvent(e);
    c_->viewport_resized();
}

}  // namespace rl::gui
