// SPDX-License-Identifier: GPL-3.0-or-later
//
// Getting images into Rasterloom through the real MainWindow (offscreen): drag-and-drop of files
// (placed as layers on an open document; opened when there is no document; several at once; an
// unsupported file explained in the status bar), dropped image data (a browser drag), the system
// clipboard (Copy / Copy Merged / Cut to QClipboard as image/png + QImage, Paste / Paste in Place,
// an image copied in another application, Paste with no document), Clear, Layer via Copy / Cut,
// and Open of formats only Qt's image plugins read (GIF first frame, BMP, WebP).
//
// Every image that enters becomes one place_image op carrying the pixels, so each test also checks
// the pixels that arrived and, where a session script exists, that rasterloom-cli replays it
// byte-exactly.
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeData>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <fstream>
#include <memory>

#include "core/edit/place.hpp"
#include "core/composite/render.hpp"
#include "core/edit/png_payload.hpp"
#include "core/geometry/dense.hpp"
#include "core/io/export_png.hpp"
#include "core/io/image_util.hpp"
#include "core/io/png.hpp"
#include "gui/canvas.hpp"
#include "gui/image_import.hpp"
#include "gui/main_window.hpp"
#include "gui/session.hpp"

using namespace rl::gui;

namespace {

rl::io::RgbaBuffer test_image(int w, int h, uint8_t seed) {
    rl::io::RgbaBuffer b;
    b.w = w;
    b.h = h;
    b.px.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            b.at(x, y) = rl::canonicalize(rl::Rgba8{static_cast<uint8_t>(x * 11 + seed), static_cast<uint8_t>(y * 23 + seed),
                                                    static_cast<uint8_t>((x + y) * 5), static_cast<uint8_t>(40 + ((x * 7 + y * 3) % 216))});
    return b;
}

rl::io::RgbaBuffer opaque(rl::io::RgbaBuffer b) {
    for (rl::Rgba8& p : b.px) p.a = 255;
    return b;
}

// The w x h block of layer `n` at (x, y), as a buffer (out-of-canvas pixels as transparent).
rl::io::RgbaBuffer block(const rl::Node& n, int x, int y, int w, int h) {
    rl::io::RgbaBuffer b;
    b.w = w;
    b.h = h;
    b.px.resize(static_cast<size_t>(w) * h);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const int cx = x + i, cy = y + j;
            b.at(i, j) = (cx >= 0 && cy >= 0 && cx < n.pixels.width() && cy < n.pixels.height()) ? n.pixels.get(cx, cy) : rl::Rgba8{};
        }
    return b;
}

bool same(const rl::io::RgbaBuffer& a, const rl::io::RgbaBuffer& b) { return a.w == b.w && a.h == b.h && a.px == b.px; }

}  // namespace

class GuiImport : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w_;
    QTemporaryDir tmp_;

    EditorSession* s() { return w_->session(); }
    QString path(const QString& n) { return tmp_.filePath(n); }
    const rl::Node* node(const std::string& id) {
        rl::NodeRef r = s()->doc().find(id);
        return r.node;
    }
    const rl::Node* top() { return &s()->doc().root().children.back(); }

    QString write_png(const QString& name, const rl::io::RgbaBuffer& b) {
        const QString p = path(name);
        rl::io::write_png(p.toStdString(), b);
        return p;
    }

    void drop(QMimeData& md, bool expect_enter = true) {
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w_.get(), &enter);
        QCOMPARE(enter.isAccepted(), expect_enter);
        if (!expect_enter) return;
        QDropEvent ev(QPointF(20, 20), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w_.get(), &ev);
        QVERIFY(ev.isAccepted());
    }
    void drop_files(const QStringList& files) {
        QMimeData md;
        QList<QUrl> urls;
        for (const QString& f : files) urls << QUrl::fromLocalFile(f);
        md.setUrls(urls);
        drop(md);
    }
    void trigger(const char* key) {
        QAction* a = w_->action(QString::fromLatin1(key));
        QVERIFY2(a, key);
        a->trigger();
    }
    // A real (non-placeholder) document with one opaque layer "L" of `img` at (0, 0).
    void doc_with_layer(int W, int H, const rl::io::RgbaBuffer& img) {
        w_->new_document(W, H, 1);
        QVERIFY(s()->apply({{"op", "place_image"}, {"id", "L"}, {"png", rl::edit::encode_payload(img)}, {"x", 0}, {"y", 0}}, "setup").ok());
        s()->set_active_layer("L");
    }
    void check_cli_replay() {
        const std::optional<Json> script = s()->session_script();
        QVERIFY(script.has_value());
        const QString sp = path(QStringLiteral("s.json")), out = path(QStringLiteral("cli.png")), gui = path(QStringLiteral("gui.png"));
        {
            std::ofstream f(sp.toStdString());
            f << script->dump(1);
        }
        QProcess p;
        p.start(QStringLiteral(RL_CLI), {QStringLiteral("--render-script"), sp, QStringLiteral("--out"), out});
        QVERIFY(p.waitForFinished(60000));
        QVERIFY2(p.exitCode() == 0, qPrintable(QString::fromLocal8Bit(p.readAllStandardError())));
        rl::io::write_document_png(s()->state(), gui.toStdString());
        QVERIFY(rl::io::read_png(out.toStdString()).px == rl::io::read_png(gui.toStdString()).px);
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        qputenv("XDG_CONFIG_HOME", tmp_.filePath(QStringLiteral("config")).toLocal8Bit());
    }
    // A fresh window per test: its untouched start-up document is "no document open".
    void init() {
        w_ = std::make_unique<MainWindow>();
        w_->set_confirm_close(false);
        w_->resize(1200, 800);
        w_->show();
        QVERIFY(QTest::qWaitForWindowExposed(w_.get()));
        QVERIFY(s()->is_placeholder());
        QGuiApplication::clipboard()->clear();
    }
    void cleanup() { w_.reset(); }

    // ---- drag-and-drop -------------------------------------------------------------------------------------------
    void drop_places_layer() {
        const rl::io::RgbaBuffer img = test_image(30, 20, 3);
        const QString f = write_png(QStringLiteral("sticker.png"), img);
        w_->new_document(100, 80, 0);
        s()->set_active_layer("Background");
        const size_t entries = s()->applied_count();
        drop_files({f});
        QTRY_COMPARE(s()->doc().root().children.size(), size_t{2});
        QCOMPARE(s()->applied_count(), entries + 1);  // one history record
        const rl::Node* n = top();
        QCOMPARE(QString::fromStdString(n->name), QStringLiteral("sticker"));  // named after the file
        QCOMPARE(s()->active_layer(), n->id);
        QVERIFY(same(block(*n, 35, 30, 30, 20), img));  // centred: ((100-30)/2, (80-20)/2)
        QCOMPARE(block(*n, 34, 30, 1, 1).px[0].a, uint8_t{0});
        QCOMPARE(s()->file_path(), QString());
        check_cli_replay();
    }

    void drop_opens_when_no_document() {
        const rl::io::RgbaBuffer img = test_image(64, 48, 9);
        const QString f = write_png(QStringLiteral("photo.png"), img);
        drop_files({f});
        QTRY_COMPARE(s()->file_path(), f);
        QCOMPARE(s()->doc().width(), 64);
        QCOMPARE(s()->doc().height(), 48);
        QCOMPARE(s()->doc().root().children.size(), size_t{1});
        QVERIFY(same(block(*top(), 0, 0, 64, 48), img));
    }

    void drop_multiple_files() {
        const QString a = write_png(QStringLiteral("a.png"), test_image(40, 30, 1));
        const QString b = write_png(QStringLiteral("b.png"), test_image(10, 10, 2));
        const QString bmp = path(QStringLiteral("c.bmp"));
        QVERIFY(QImageWriter(bmp, "bmp").write(to_qimage(opaque(test_image(12, 8, 5)))));
        const QString txt = path(QStringLiteral("readme.txt"));
        {
            QFile t(txt);
            QVERIFY(t.open(QIODevice::WriteOnly));
            t.write("not an image");
        }
        // No document: the first file opens, the others are placed into it, the text file is skipped.
        drop_files({a, b, bmp, txt});
        QTRY_COMPARE(s()->doc().root().children.size(), size_t{3});
        QCOMPARE(s()->file_path(), a);
        QCOMPARE(s()->doc().width(), 40);
        QCOMPARE(QString::fromStdString(s()->doc().root().children[1].name), QStringLiteral("b"));
        QCOMPARE(QString::fromStdString(s()->doc().root().children[2].name), QStringLiteral("c"));
        QVERIFY(same(block(s()->doc().root().children[1], 15, 10, 10, 10), test_image(10, 10, 2)));
        QVERIFY(same(block(s()->doc().root().children[2], 14, 11, 12, 8), opaque(test_image(12, 8, 5))));
        QVERIFY2(w_->last_message().contains(QStringLiteral("readme.txt")), qPrintable(w_->last_message()));
    }

    void drop_browser_image() {
        // What a browser drag carries: the image's web address plus the image itself.
        w_->new_document(50, 50, 0);
        const rl::io::RgbaBuffer img = test_image(20, 16, 7);
        QMimeData md;
        md.setUrls({QUrl(QStringLiteral("https://example.com/cat.png"))});
        md.setImageData(to_qimage(img));
        drop(md);
        QTRY_COMPARE(s()->doc().root().children.size(), size_t{2});
        QVERIFY(same(block(*top(), 15, 17, 20, 16), img));
        // A web address alone cannot be used: explained, nothing changes.
        QMimeData link;
        link.setUrls({QUrl(QStringLiteral("https://example.com/page.html"))});
        drop(link);
        QTRY_VERIFY(w_->last_message().contains(QStringLiteral("web address")));
        QCOMPARE(s()->doc().root().children.size(), size_t{2});
    }

    void drop_unsupported_message() {
        w_->new_document(50, 40, 0);
        const QString txt = path(QStringLiteral("notes.txt"));
        {
            QFile t(txt);
            QVERIFY(t.open(QIODevice::WriteOnly));
            t.write("plain text");
        }
        const size_t entries = s()->applied_count();
        QMimeData md;
        md.setUrls({QUrl::fromLocalFile(txt)});
        drop(md);  // accepted at drag-enter so the drop can explain itself
        QTRY_VERIFY2(w_->last_message().contains(QStringLiteral("notes.txt")) &&
                         w_->last_message().contains(QStringLiteral("not an image format")),
                     qPrintable(w_->last_message()));
        QCOMPARE(s()->applied_count(), entries);
        QMimeData empty;
        empty.setText(QStringLiteral("just words"));
        drop(empty, false);  // plain text is refused at drag-enter
    }

    // ---- clipboard -------------------------------------------------------------------------------------------------
    void copy_paste_clipboard() {
        const rl::io::RgbaBuffer img = test_image(60, 40, 4);
        doc_with_layer(80, 60, img);
        QVERIFY(s()->apply({{"op", "select_rect"}, {"x", 10.0}, {"y", 5.0}, {"w", 20.0}, {"h", 12.0}}, "sel").ok());
        const size_t entries = s()->applied_count();
        trigger("copy");
        QCOMPARE(s()->applied_count(), entries);  // Copy is not a history step
        // Other applications read image/png and a QImage.
        const QMimeData* md = QGuiApplication::clipboard()->mimeData();
        QVERIFY(md->hasFormat(QStringLiteral("image/png")));
        const QByteArray png = md->data(QStringLiteral("image/png"));
        const rl::io::RgbaBuffer onclip = rl::io::decode_png(std::vector<uint8_t>(png.begin(), png.end()));
        QVERIFY(same(onclip, block(*node("L"), 10, 5, 20, 12)));
        QVERIFY(same(from_qimage(QGuiApplication::clipboard()->image()), onclip));
        // Paste: a new layer directly above the active one, centred on the selection.
        trigger("paste");
        QCOMPARE(s()->applied_count(), entries + 1);
        const Json op = s()->last_attempted_op();
        QCOMPARE(QString::fromStdString(op["op"].get<std::string>()), QStringLiteral("place_image"));
        QCOMPARE(QString::fromStdString(op["above"].get<std::string>()), QStringLiteral("L"));
        QCOMPARE(op["x"].get<int>(), 10);
        QCOMPARE(op["y"].get<int>(), 5);
        const rl::Node* p = top();
        QCOMPARE(s()->active_layer(), p->id);
        QVERIFY(same(block(*p, 10, 5, 20, 12), onclip));
        // Without a selection the paste is centred on the visible canvas (all of it after Fit).
        QVERIFY(s()->apply({{"op", "deselect"}}, "desel").ok());
        w_->canvas()->fit_to_view();
        trigger("paste");
        const Json op2 = s()->last_attempted_op();
        QCOMPARE(op2["x"].get<int>(), 30);  // (80 - 20) / 2
        QCOMPARE(op2["y"].get<int>(), 24);  // (60 - 12) / 2
        QVERIFY(same(block(*top(), 30, 24, 20, 12), onclip));
        // Select All, then subtract the left half: centred on what stays selected (x 40..79).
        QVERIFY(s()->apply({{"op", "select_all"}}, "all").ok());
        QVERIFY(s()->apply({{"op", "select_rect"}, {"x", 0.0}, {"y", 0.0}, {"w", 40.0}, {"h", 60.0}, {"mode", "subtract"}}, "sub").ok());
        trigger("paste");
        QCOMPARE(s()->last_attempted_op()["x"].get<int>(), 50);  // 60 - 20 / 2
        QCOMPARE(s()->last_attempted_op()["y"].get<int>(), 24);  // 30 - 12 / 2
        check_cli_replay();
    }

    void paste_from_other_application() {
        w_->new_document(64, 64, 0);
        w_->canvas()->fit_to_view();
        // A screenshot tool / browser "Copy image": a bare QImage, no Rasterloom origin.
        const rl::io::RgbaBuffer img = test_image(17, 9, 11);
        auto* md = new QMimeData;
        md->setImageData(to_qimage(img));
        QGuiApplication::clipboard()->setMimeData(md);
        trigger("paste_in_place");  // no origin: behaves as Paste (centred)
        QCOMPARE(s()->last_attempted_op()["x"].get<int>(), 23);
        QVERIFY(same(block(*top(), 23, 27, 17, 9), img));
        // A PNG with a palette / 16-bit channels also pastes (decoded, then re-encoded as 8-bit RGBA).
        QImage pal = to_qimage(opaque(img)).convertToFormat(QImage::Format_Indexed8);
        auto* md2 = new QMimeData;
        md2->setImageData(pal);
        QGuiApplication::clipboard()->setMimeData(md2);
        trigger("paste");
        QVERIFY(same(block(*top(), 23, 27, 17, 9), from_qimage(pal)));
        // Nothing image-like: explained.
        QGuiApplication::clipboard()->setText(QStringLiteral("hello"));
        const size_t entries = s()->applied_count();
        trigger("paste");
        QCOMPARE(s()->applied_count(), entries);
        QVERIFY(w_->last_message().contains(QStringLiteral("no image")));
        check_cli_replay();
    }

    void paste_in_place_and_copy_merged() {
        doc_with_layer(70, 50, test_image(70, 50, 6));
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "T"}, {"fill", "solid"}, {"color", "#2060E080"}, {"rect", {30, 20, 30, 20}}}, "t").ok());
        QVERIFY(s()->apply({{"op", "select_ellipse"}, {"x", 40.0}, {"y", 12.0}, {"w", 24.0}, {"h", 30.0}, {"antialias", true}}, "sel").ok());
        s()->set_active_layer("L");
        trigger("copy");
        trigger("paste_in_place");
        // The pasted layer (directly above L, under T) equals Layer via Copy (docs/math/60 §14.4.1),
        // which the goldens prove against the reference.
        const std::string pasted = s()->last_attempted_op()["id"].get<std::string>();
        const rl::NodeRef lref = s()->doc().find("L");
        QCOMPARE(lref.parent->children[lref.index + 1].id, pasted);
        Json lvc = {{"op", "layer_via_copy"}, {"layer", "L"}, {"id", "via"}};
        rl::Document ref(70, 50, rl::Rgba8{});
        ref.state() = s()->state();
        QVERIFY(rl::gui::apply_op(ref, {{"op", "delete_layer"}, {"layer", pasted}}).ok());
        QVERIFY(rl::gui::apply_op(ref, lvc).ok());
        QVERIFY(geomless_equal(*node(pasted), *ref.find("via").node));
        // Copy Merged: the composite through the selection.
        trigger("copy_merged");
        const std::optional<rl::edit::Copied> merged = rl::edit::copy_region(s()->state(), std::string());
        QVERIFY(merged.has_value());
        QVERIFY(same(from_qimage(QGuiApplication::clipboard()->image()), merged->pixels));
        trigger("paste_in_place");
        QCOMPARE(s()->last_attempted_op()["x"].get<int>(), merged->x);
        QCOMPARE(s()->last_attempted_op()["y"].get<int>(), merged->y);
        check_cli_replay();
    }

    void paste_new_document() {
        // No document open (the untouched start-up canvas): Paste makes a document sized to the image.
        const rl::io::RgbaBuffer img = test_image(33, 21, 8);
        auto* md = new QMimeData;
        md->setImageData(to_qimage(img));
        QGuiApplication::clipboard()->setMimeData(md);
        trigger("paste");
        QCOMPARE(s()->doc().width(), 33);
        QCOMPARE(s()->doc().height(), 21);
        QCOMPARE(s()->doc().root().children.size(), size_t{1});
        QVERIFY(same(block(*top(), 0, 0, 33, 21), img));
        QVERIFY(!s()->is_placeholder());
        check_cli_replay();  // the new document's base script carries the pasted pixels
        // The next paste goes into this document as a layer.
        trigger("paste");
        QCOMPARE(s()->doc().root().children.size(), size_t{2});
    }

    void cut_and_clear() {
        const rl::io::RgbaBuffer img = opaque(test_image(40, 30, 2));
        doc_with_layer(40, 30, img);
        QVERIFY(s()->apply({{"op", "select_rect"}, {"x", 4.0}, {"y", 3.0}, {"w", 10.0}, {"h", 6.0}}, "sel").ok());
        const size_t entries = s()->applied_count();
        trigger("cut");
        QCOMPARE(s()->applied_count(), entries + 1);  // one record: the clear
        QCOMPARE(QString::fromStdString(s()->last_attempted_op()["op"].get<std::string>()), QStringLiteral("clear"));
        QVERIFY(same(from_qimage(QGuiApplication::clipboard()->image()), block_of(img, 4, 3, 10, 6)));
        QCOMPARE(block(*node("L"), 4, 3, 10, 6).px, std::vector<rl::Rgba8>(60, rl::Rgba8{}));
        trigger("paste_in_place");
        QVERIFY(same(block(*top(), 4, 3, 10, 6), block_of(img, 4, 3, 10, 6)));
        // Clear (Delete) needs a selection; with one it clears through it.
        s()->set_active_layer("L");
        QVERIFY(s()->apply({{"op", "deselect"}}, "d").ok());
        const size_t e2 = s()->applied_count();
        trigger("clear");
        QCOMPARE(s()->applied_count(), e2);
        QVERIFY(w_->last_message().contains(QStringLiteral("selection")));
        QVERIFY(s()->apply({{"op", "select_rect"}, {"x", 20.0}, {"y", 10.0}, {"w", 5.0}, {"h", 5.0}}, "sel").ok());
        trigger("clear");
        QCOMPARE(block(*node("L"), 20, 10, 5, 5).px, std::vector<rl::Rgba8>(25, rl::Rgba8{}));
        QCOMPARE(block(*node("L"), 26, 10, 1, 1).px[0], img.at(26, 10));
        check_cli_replay();
    }

    void layer_via_copy_and_cut() {
        const rl::io::RgbaBuffer img = opaque(test_image(40, 30, 4));
        doc_with_layer(40, 30, img);
        QVERIFY(s()->apply({{"op", "select_rect"}, {"x", 2.0}, {"y", 2.0}, {"w", 8.0}, {"h", 8.0}}, "sel").ok());
        trigger("duplicate_layer");  // Ctrl+J with a selection = Layer via Copy
        QCOMPARE(QString::fromStdString(s()->last_attempted_op()["op"].get<std::string>()), QStringLiteral("layer_via_copy"));
        QVERIFY(same(block(*top(), 2, 2, 8, 8), block_of(img, 2, 2, 8, 8)));
        QCOMPARE(block(*top(), 10, 2, 1, 1).px[0].a, uint8_t{0});
        s()->set_active_layer("L");
        trigger("layer_via_cut");
        QCOMPARE(s()->last_attempted_op()["cut"].get<bool>(), true);
        QCOMPARE(block(*node("L"), 2, 2, 8, 8).px, std::vector<rl::Rgba8>(64, rl::Rgba8{}));
        check_cli_replay();
    }

    // ---- formats only Qt reads -----------------------------------------------------------------------------------
    void open_gif_bmp_webp() {
        // GIF: Qt has no GIF writer, so a committed two-frame fixture; the first frame opens.
        QVERIFY(w_->open_file(QStringLiteral(RL_FIXTURES "/two_frames.gif")));
        QCOMPARE(s()->doc().width(), 6);
        QCOMPARE(s()->doc().height(), 4);
        QCOMPARE(top()->pixels.get(0, 0), (rl::Rgba8{0, 255, 0, 255}));
        QCOMPARE(top()->pixels.get(1, 0), (rl::Rgba8{255, 0, 0, 255}));
        QCOMPARE(top()->pixels.get(4, 2), (rl::Rgba8{0, 0, 255, 255}));
        QVERIFY2(w_->warnings_dialog() && w_->warnings_dialog()->isVisible(), "the frame warning is shown");
        w_->warnings_dialog()->close();
        // BMP (24-bit: opaque).
        const rl::io::RgbaBuffer img = opaque(test_image(23, 17, 12));
        const QString bmp = path(QStringLiteral("x.bmp"));
        QVERIFY(QImageWriter(bmp, "bmp").write(to_qimage(img)));
        QVERIFY(w_->open_file(bmp));
        QCOMPARE(s()->doc().width(), 23);
        QVERIFY(same(block(*top(), 0, 0, 23, 17), img));
        // WebP (lossless, with alpha), through the qtimageformats plugin.
        QVERIFY2(QImageWriter::supportedImageFormats().contains("webp") && QImageReader::supportedImageFormats().contains("webp"),
                 "Qt's WebP plugin (qtimageformats) is not installed");
        const rl::io::RgbaBuffer rgba = test_image(19, 13, 13);
        const QString webp = path(QStringLiteral("x.webp"));
        QImageWriter ww(webp, "webp");
        ww.setQuality(100);  // lossless
        QVERIFY(ww.write(to_qimage(rgba)));
        QVERIFY(w_->open_file(webp));
        QCOMPARE(s()->doc().width(), 19);
        QVERIFY(same(block(*top(), 0, 0, 19, 13), rgba));
        // The Open dialog lists them.
        QVERIFY(open_filter().contains(QStringLiteral("*.gif")));
        QVERIFY(open_filter().contains(QStringLiteral("*.webp")));
        QVERIFY(open_filter().contains(QStringLiteral("*.bmp")));
        // An extension-less GIF is recognised by content.
        const QString noext = path(QStringLiteral("gif_without_extension"));
        QVERIFY(QFile::copy(QStringLiteral(RL_FIXTURES "/two_frames.gif"), noext));
        QVERIFY(can_read_image(noext));
    }

    void export_webp_bmp() {
        // File > Export > WebP (lossless, alpha kept) / BMP (flattened onto white), reopened.
        const rl::io::RgbaBuffer img = test_image(29, 21, 14);
        doc_with_layer(29, 21, img);
        rl::io::RgbaBuffer comp;
        comp.w = 29;
        comp.h = 21;
        comp.px = rl::geom::to_dense(rl::composite::render_image(s()->state(), true));
        QVERIFY2(QImageWriter::supportedImageFormats().contains("webp"), "Qt's WebP plugin (qtimageformats) is not installed");
        QVERIFY(w_->action(QStringLiteral("export_webp")));
        const QString webp = path(QStringLiteral("out.webp"));
        QVERIFY(w_->export_to(webp));
        const ImportedImage back = read_with_qt(webp);
        QVERIFY2(back.ok(), qPrintable(back.error));
        QVERIFY(same(back.img, comp));
        const QString bmp = path(QStringLiteral("out.bmp"));
        QVERIFY(w_->export_to(bmp));
        rl::io::RgbaBuffer white = comp;
        for (rl::Rgba8& p : white.px) p = rl::io::matte_white(p);
        QVERIFY(same(read_with_qt(bmp).img, white));
        QVERIFY(s()->file_path().isEmpty());  // an export does not rename the document
    }

    void cleanupTestCase() {}

private:
    static bool geomless_equal(const rl::Node& a, const rl::Node& b) {
        return rl::geom::to_dense(a.pixels) == rl::geom::to_dense(b.pixels);
    }
    static rl::io::RgbaBuffer block_of(const rl::io::RgbaBuffer& img, int x, int y, int w, int h) {
        rl::io::RgbaBuffer b;
        b.w = w;
        b.h = h;
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) b.px.push_back(img.at(x + i, y + j));
        return b;
    }
};

QTEST_MAIN(GuiImport)
#include "gui_import.moc"
