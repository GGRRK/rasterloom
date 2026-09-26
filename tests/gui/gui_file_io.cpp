// SPDX-License-Identifier: GPL-3.0-or-later
//
// File I/O through the GUI (src/gui/file_io.*, MainWindow): save .orp / .ora, export PNG / JPEG
// (quality) / TIFF / PSD / PSB / ORA, reopen each, dirty-state tracking and the window title,
// atomic saves (a failed save never truncates the existing file and leaves no temporary file),
// import warnings in one dialog (the independent PSD corpus: adjustment, CMYK, 16-bit, vector
// mask, smart object, text), a > 16384 px PSB opening with a warning, drag-and-drop open and
// the recent-files list.
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QListWidget>
#include <QMimeData>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <sys/stat.h>

#include "core/io/export_png.hpp"
#include "core/io/file_io.hpp"
#include "core/io/png.hpp"
#include "gui/file_io.hpp"
#include "gui/main_window.hpp"
#include "gui/session.hpp"
#include "gui/theme.hpp"

using namespace rl::gui;

namespace {

std::vector<uint8_t> composite(const rl::DocState& s, const QString& tmp_png) {
    rl::io::write_document_png(s, tmp_png.toStdString());
    const rl::io::RgbaBuffer b = rl::io::read_png(tmp_png.toStdString());
    std::vector<uint8_t> v;
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const rl::Rgba8 p = b.at(x, y);
            v.insert(v.end(), {p.r, p.g, p.b, p.a});
        }
    return v;
}

QByteArray read_all(const QString& p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

class GuiFileIo : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w_;
    QTemporaryDir tmp_;

    EditorSession* s() { return w_->session(); }
    QString path(const QString& n) { return tmp_.filePath(n); }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        qputenv("XDG_CONFIG_HOME", tmp_.filePath(QStringLiteral("config")).toLocal8Bit());
        w_ = std::make_unique<MainWindow>();
        w_->set_confirm_close(false);  // no modal prompts: errors go to the status line
        w_->resize(1200, 800);
        w_->show();
        QVERIFY(QTest::qWaitForWindowExposed(w_.get()));
    }

    void save_reopen_and_title() {
        w_->new_document(160, 120, 0);
        QVERIFY(!s()->modified());
        QVERIFY(w_->windowTitle().startsWith(QStringLiteral("Untitled - Rasterloom")));
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "Red"}, {"fill", "solid"}, {"color", "#E03020C0"}, {"rect", {10, 10, 80, 60}}}, "l").ok());
        QVERIFY(s()->apply({{"op", "add_adjustment"}, {"id", "Inv"}, {"type", "invert"}, {"params", Json::object()}, {"parent", "root"}}, "a").ok());
        // Omitted params and an empty object must store the same canonical text (core node.hpp).
        QVERIFY(s()->apply({{"op", "add_adjustment"}, {"id", "Inv2"}, {"type", "invert"}}, "a2").ok());
        QVERIFY(s()->apply({{"op", "set_name"}, {"layer", "Inv2"}, {"name", "Invert again"}}, "n").ok());
        QVERIFY(s()->apply({{"op", "add_mask"}, {"layer", "Red"}, {"fill", "solid"}, {"value", 255}}, "m").ok());
        QVERIFY(s()->modified());
        QVERIFY2(w_->windowTitle().startsWith(QStringLiteral("Untitled *")), qPrintable(w_->windowTitle()));
        const std::vector<uint8_t> ref = composite(s()->state(), path(QStringLiteral("ref.png")));
        const std::string tree = rl::io::dump_tree_json(s()->state());

        QVERIFY(w_->save_to(path(QStringLiteral("doc.orp"))));
        QVERIFY(!s()->modified());
        QVERIFY2(w_->windowTitle().startsWith(QStringLiteral("doc.orp - Rasterloom")), qPrintable(w_->windowTitle()));
        QVERIFY(w_->save_to(path(QStringLiteral("doc.ora"))));
        QCOMPARE(read_all(path(QStringLiteral("doc.orp"))), read_all(path(QStringLiteral("doc.ora"))));  // same bytes

        QVERIFY(w_->open_file(path(QStringLiteral("doc.orp"))));
        QVERIFY(!s()->modified());
        // Empty adjustment params are stored canonically as "{}" by add_adjustment and the readers
        // alike (core node.hpp), so the tree dump matches with no special-casing.
        QCOMPARE(rl::io::dump_tree_json(s()->state()), tree);
        QVERIFY(composite(s()->state(), path(QStringLiteral("re.png"))) == ref);
        QVERIFY(w_->last_message().startsWith(QStringLiteral("Opened doc.orp")));
    }

    void exports_reopen() {
        const std::vector<uint8_t> ref = composite(s()->state(), path(QStringLiteral("ref2.png")));
        const QString orp = s()->file_path();
        for (const char* ext : {"png", "tif", "psd", "psb", "ora"}) {
            const QString p = path(QStringLiteral("x.") + QLatin1String(ext));
            QVERIFY2(w_->export_to(p, {92, QLatin1String(ext) == QLatin1String("psb")}), ext);
            QVERIFY(!s()->modified());
            QCOMPARE(s()->file_path(), orp);  // export does not rename the document
            LoadResult r = load_document(p, 50);
            QVERIFY2(r.doc, qPrintable(r.error));
            QVERIFY2(composite(r.doc->state(), path(QStringLiteral("x.png.check.png"))) == ref, ext);
        }
        // PSB really is PSB (version 2).
        const QByteArray psb = read_all(path(QStringLiteral("x.psb")));
        QCOMPARE(psb.left(6), QByteArray("8BPS\x00\x02", 6));
        // JPEG quality reaches the encoder.
        QVERIFY(w_->export_to(path(QStringLiteral("q20.jpg")), {20, false}));
        QVERIFY(w_->export_to(path(QStringLiteral("q98.jpg")), {98, false}));
        QVERIFY(QFileInfo(path(QStringLiteral("q20.jpg"))).size() < QFileInfo(path(QStringLiteral("q98.jpg"))).size());
        QVERIFY(load_document(path(QStringLiteral("q98.jpg")), 50).doc);
    }

    void failed_save_keeps_old_file() {
        const QString dir = path(QStringLiteral("ro"));
        QVERIFY(QDir().mkpath(dir));
        const QString target = dir + QStringLiteral("/keep.orp");
        QVERIFY(w_->save_to(target));
        const QByteArray old = read_all(target);
        QVERIFY(!old.isEmpty());
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "More"}, {"fill", "solid"}, {"color", "#00FF00FF"}}, "more").ok());
        QVERIFY(s()->modified());
        QVERIFY(::chmod(dir.toLocal8Bit().constData(), 0555) == 0);
        const bool ok = w_->save_to(target);
        ::chmod(dir.toLocal8Bit().constData(), 0755);
        if (ok) QSKIP("running with permissions that ignore a read-only directory (root?)");
        QVERIFY(s()->modified());                        // still dirty: nothing was saved
        QCOMPARE(read_all(target), old);                 // the old file is intact
        QCOMPARE(QDir(dir).entryList(QDir::Files | QDir::Hidden).size(), 1);  // no temporary left
        QVERIFY2(w_->last_message().contains(QLatin1String("Could not save")), qPrintable(w_->last_message()));
    }

    void import_warnings_one_dialog() {
        const QString corpus = path(QStringLiteral("corpus"));
        QProcess p;
        p.start(QStringLiteral("python3"), {QStringLiteral(RL_PSD_CORPUS_GEN), corpus});
        QVERIFY(p.waitForFinished(120000));
        if (p.exitCode() != 0) QSKIP("python3 with numpy is needed to generate the PSD corpus");
        int with_warnings = 0;
        for (const char* f : {"adjustment.psd", "cmyk.psd", "depth16.psd", "mask_vector.psd", "smart_object.psd", "text_layer.psd",
                              "layer_style.psd", "nested_groups.psd"}) {
            QVERIFY2(w_->open_file(corpus + QLatin1Char('/') + QLatin1String(f)), f);
            LoadResult r = load_document(corpus + QLatin1Char('/') + QLatin1String(f), 50);
            QCoreApplication::processEvents();
            if (r.warnings.isEmpty()) {
                QVERIFY2(!w_->warnings_dialog() || !w_->warnings_dialog()->isVisible() ||
                             w_->warnings_dialog()->findChild<QListWidget*>()->count() == 0,
                         f);
                continue;
            }
            ++with_warnings;
            QDialog* d = w_->warnings_dialog();
            QVERIFY2(d && d->isVisible(), f);
            QVERIFY(!d->isModal());
            auto* list = d->findChild<QListWidget*>(QStringLiteral("WarningList"));
            QVERIFY(list);
            QCOMPARE(list->count(), static_cast<int>(r.warnings.size()));  // every warning, one dialog
            qInfo("%s: %d warning(s), first: %s", f, list->count(), qPrintable(list->item(0)->text()));
            d->close();
        }
        QVERIFY2(with_warnings >= 3, "CMYK, 16-bit and the unsupported adjustment must report what they converted");
        // A > 16384 px PSB opens with the large-size note first.
        QVERIFY(w_->open_file(corpus + QStringLiteral("/psb_30001.psb")));
        QVERIFY(s()->doc().width() > kLargeSide || s()->doc().height() > kLargeSide);
        QDialog* d = w_->warnings_dialog();
        QVERIFY(d && d->isVisible());
        QVERIFY(d->findChild<QListWidget*>()->item(0)->text().contains(QStringLiteral("16384")));
        d->close();
    }

    void drag_and_drop_and_recent() {
        const QString target = path(QStringLiteral("doc.orp"));
        w_->new_document(50, 40, 0);
        QMimeData md;
        md.setUrls({QUrl::fromLocalFile(target)});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w_.get(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w_.get(), &drop);
        QVERIFY(drop.isAccepted());
        QTRY_COMPARE(s()->file_path(), target);
        QCOMPARE(s()->doc().width(), 160);
        // Unsupported files are refused at drag-enter.
        QMimeData bad;
        bad.setUrls({QUrl::fromLocalFile(path(QStringLiteral("notes.txt")))});
        QDragEnterEvent enter2(QPoint(10, 10), Qt::CopyAction, &bad, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w_.get(), &enter2);
        QVERIFY(!enter2.isAccepted());
        // Recent files: most recent first, absolute paths.
        QSettings st(QStringLiteral("Rasterloom"), QStringLiteral("Rasterloom"));
        const QStringList recent = st.value(QStringLiteral("recent")).toStringList();
        QVERIFY(!recent.isEmpty());
        QCOMPARE(recent.front(), QFileInfo(target).absoluteFilePath());
    }

    void cleanupTestCase() { w_.reset(); }
};

QTEST_MAIN(GuiFileIo)
#include "gui_file_io.moc"
