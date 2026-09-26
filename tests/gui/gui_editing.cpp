// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layer editing from the GUI through the doc-60 ops: rename (inline editor -> set_name), edit an
// adjustment layer (double-click -> dialog -> set_adjustment with live preview), delete_layer,
// duplicate_layer (derived ids checked first), set_group_mode (Isolate Group) and Ctrl+click on a
// thumbnail -> select_alpha (Shift adds, Alt subtracts, both intersect). Every op the GUI emits is
// checked against the doc-60 grammar, its effect on the document, and that it adds exactly one
// history record, shown as one new row in the History panel carrying that op.
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QSpinBox>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <functional>
#include <set>

#include "core/adjust/adjustment.hpp"
#include "gui/layers_panel.hpp"
#include "gui/main_window.hpp"
#include "gui/op_runner.hpp"
#include "gui/panels.hpp"
#include "gui/session.hpp"

using namespace rl::gui;

namespace {

const rl::Node* find(const rl::Node& c, const std::string& id) {
    for (const rl::Node& n : c.children) {
        if (n.id == id) return &n;
        if (n.is_group())
            if (const rl::Node* f = find(n, id)) return f;
    }
    return nullptr;
}

std::set<std::string> keys(const Json& j) {
    std::set<std::string> k;
    for (auto it = j.begin(); it != j.end(); ++it) k.insert(it.key());
    return k;
}

}  // namespace

class GuiEditing : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w_;
    std::function<void(QDialog*)> on_modal_;
    QTimer modal_timer_;

    EditorSession* s() { return w_->session(); }
    const rl::Node* node(const std::string& id) { return find(s()->doc().root(), id); }
    QTreeWidgetItem* item(const std::string& id) {
        for (QTreeWidgetItemIterator it(w_->layers_panel()->tree()); *it; ++it)
            if ((*it)->data(1, Qt::UserRole + 1).toString().toStdString() == id) return *it;
        return nullptr;
    }
    // Exactly one history record for `op`: the session and the core history grew by one, and the
    // History panel shows one more row (base row + one per entry), current, carrying that op.
    void expect_one_record(const char* op, size_t before, size_t core_before) {
        QVERIFY2(op_available(op), op);
        QCOMPARE(s()->applied_count(), before + 1);
        QCOMPARE(s()->doc().history_size(), core_before + 1);
        QCoreApplication::processEvents();
        QListWidget* h = w_->history_panel()->list();
        QCOMPARE(h->count(), int(s()->entries().size()) + 1);
        QCOMPARE(h->currentRow(), int(before + 1));
        const QString tip = h->item(int(before + 1))->toolTip();
        QVERIFY2(tip.contains(QStringLiteral("\"op\":\"%1\"").arg(QLatin1String(op))), qPrintable(tip));
        QVERIFY(!h->item(int(before + 1))->text().isEmpty());
    }
    size_t core_hist() { return s()->doc().history_size(); }

private slots:
    void initTestCase() {
        w_ = std::make_unique<MainWindow>();
        w_->set_confirm_close(false);
        w_->resize(1200, 800);
        w_->show();
        QVERIFY(QTest::qWaitForWindowExposed(w_.get()));
        modal_timer_.setInterval(30);
        connect(&modal_timer_, &QTimer::timeout, this, [this] {
            auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!d) return;
            if (on_modal_) {
                auto h = std::move(on_modal_);
                on_modal_ = nullptr;
                h(d);
            } else {
                d->reject();
            }
        });
        modal_timer_.start();
        w_->new_document(200, 150, 1);
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "Paint"}, {"fill", "solid"}, {"color", "#3070E0FF"}, {"rect", {20, 20, 100, 60}}}, "p").ok());
        QVERIFY(s()->apply({{"op", "add_group"}, {"id", "G"}, {"mode", "pass"}}, "g").ok());
        QVERIFY(s()->apply({{"op", "add_layer"}, {"id", "Inner"}, {"parent", "G"}}, "i").ok());
        QVERIFY(s()->apply({{"op", "add_adjustment"}, {"id", "Lv"}, {"type", "levels"}, {"params", {{"rgb", {{"in_white", 200}}}}}}, "lv").ok());
        QCoreApplication::processEvents();
    }

    void inline_rename() {
        s()->set_active_layer("Paint");
        const size_t before = s()->applied_count(), hb = core_hist();
        w_->action(QStringLiteral("rename_layer"))->trigger();
        QLineEdit* ed = w_->layers_panel()->rename_editor();
        QVERIFY(ed);
        QCOMPARE(ed->text(), QStringLiteral("Paint"));
        ed->setText(QStringLiteral("Sky – warm"));
        QTest::keyClick(ed, Qt::Key_Return);
        QCoreApplication::processEvents();
        QVERIFY(!w_->layers_panel()->rename_editor());
        const Json op = s()->last_attempted_op();
        QCOMPARE(QString::fromStdString(op["op"].get<std::string>()), QStringLiteral("set_name"));
        QCOMPARE(keys(op), (std::set<std::string>{"op", "layer", "name"}));
        QCOMPARE(QString::fromStdString(op["name"].get<std::string>()), QStringLiteral("Sky – warm"));
        expect_one_record("set_name", before, hb);
        QVERIFY(node("Paint"));  // ids never change (doc 60 §1.2)
        QVERIFY(!node("Sky – warm"));
        QCOMPARE(s()->active_layer(), std::string("Paint"));
        QCOMPARE(QString::fromStdString(node("Paint")->name), QStringLiteral("Sky – warm"));
        QVERIFY(item("Paint")->text(1).startsWith(QStringLiteral("Sky – warm")));  // display_name = name
        QVERIFY(item("Inner")->text(1).startsWith(QStringLiteral("Inner")));       // display_name = id
        QVERIFY(!op_available("rename_layer"));                                    // the GUI-local op is retired
        // Esc cancels.
        w_->action(QStringLiteral("rename_layer"))->trigger();
        ed = w_->layers_panel()->rename_editor();
        QVERIFY(ed);
        ed->setText(QStringLiteral("nope"));
        QTest::keyClick(ed, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QCOMPARE(s()->applied_count(), before + 1);
        // Double-click on a raster row opens the same inline editor; clearing the field resets the
        // name (set_name "" -> the row shows the id again).
        {
            QTreeWidget* t = w_->layers_panel()->tree();
            const QRect r = t->visualRect(t->indexFromItem(item("Paint"), 1));
            QTest::mouseClick(t->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());  // press + release, then the double-click
            QTest::mouseDClick(t->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
            ed = w_->layers_panel()->rename_editor();
            QVERIFY(ed);
            QCOMPARE(ed->text(), QStringLiteral("Sky – warm"));
            ed->clear();
            const size_t b2 = s()->applied_count(), h2 = core_hist();
            QTest::keyClick(ed, Qt::Key_Return);
            QCoreApplication::processEvents();
            expect_one_record("set_name", b2, h2);
            QCOMPARE(s()->last_attempted_op()["name"].get<std::string>(), std::string());
            QVERIFY(node("Paint")->name.empty());
            QVERIFY(item("Paint")->text(1).startsWith(QStringLiteral("Paint")));
            s()->undo();
            QCOMPARE(QString::fromStdString(node("Paint")->name), QStringLiteral("Sky – warm"));
        }
        s()->undo();
        QVERIFY(node("Paint"));
        QVERIFY(node("Paint")->name.empty());
    }

    void layer_properties_routes_by_kind() {
        // Raster: Layer Properties opens the name editor (set_name).
        s()->set_active_layer("Paint");
        w_->action(QStringLiteral("layer_properties"))->trigger();
        QLineEdit* ed = w_->layers_panel()->rename_editor();
        QVERIFY(ed);
        ed->setText(QStringLiteral("Props"));
        size_t before = s()->applied_count(), hb = core_hist();
        QTest::keyClick(ed, Qt::Key_Return);
        QCoreApplication::processEvents();
        expect_one_record("set_name", before, hb);
        s()->undo();
        // Adjustment: Layer Properties opens the adjustment dialog (set_adjustment).
        s()->set_active_layer("Lv");
        bool saw = false;
        on_modal_ = [&](QDialog* d) {
            saw = true;
            QSpinBox* in_white = nullptr;
            for (QSpinBox* sb : d->findChildren<QSpinBox*>())
                if (sb->value() == 200) in_white = sb;
            QVERIFY(in_white);
            in_white->setValue(190);
            d->accept();  // at once: no wait for the debounced preview (the commit must still carry 190)
        };
        before = s()->applied_count();
        hb = core_hist();
        w_->action(QStringLiteral("layer_properties"))->trigger();
        QTRY_VERIFY(saw);
        QCoreApplication::processEvents();
        QCOMPARE(QString::fromStdString(s()->last_attempted_op()["op"].get<std::string>()), QStringLiteral("set_adjustment"));
        expect_one_record("set_adjustment", before, hb);
        QCOMPARE(int(std::get<rl::adjust::LevelsParams>(node("Lv")->adjustment->params()).rgb.in_white), 190);
        s()->undo();
    }

    void edit_adjustment_by_double_click() {
        QTreeWidgetItem* it = item("Lv");
        QVERIFY(it);
        const size_t before = s()->applied_count(), hb = core_hist();
        bool saw_dialog = false;
        on_modal_ = [&](QDialog* d) {
            saw_dialog = true;
            // Prefilled from the layer's params: RGB input white 200.
            const auto spins = d->findChildren<QSpinBox*>();
            QSpinBox* in_white = nullptr;
            for (QSpinBox* sb : spins)
                if (sb->value() == 200) in_white = sb;
            QVERIFY(in_white);
            in_white->setValue(180);
            QTest::qWait(250);  // debounced live preview
            d->accept();
        };
        QTreeWidget* t = w_->layers_panel()->tree();
        t->scrollToItem(it);
        const QRect r = t->visualRect(t->indexFromItem(it, 1));
        QTest::mouseClick(t->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());  // select, as a user would
        QTest::mouseDClick(t->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
        QTRY_VERIFY(saw_dialog);
        QCoreApplication::processEvents();
        const Json op = s()->last_attempted_op();
        QCOMPARE(QString::fromStdString(op["op"].get<std::string>()), QStringLiteral("set_adjustment"));
        QCOMPARE(keys(op), (std::set<std::string>{"op", "layer", "params"}));
        QCOMPARE(op["params"]["rgb"]["in_white"].get<int>(), 180);
        expect_one_record("set_adjustment", before, hb);  // the live previews left no records
        const auto& p = std::get<rl::adjust::LevelsParams>(node("Lv")->adjustment->params());
        QCOMPARE(int(p.rgb.in_white), 180);
        s()->undo();
        QCOMPARE(int(std::get<rl::adjust::LevelsParams>(node("Lv")->adjustment->params()).rgb.in_white), 200);
    }

    void duplicate_group_mode_select_alpha_delete() {
        // Duplicate (a group: derived ids must be free).
        s()->set_active_layer("G");
        size_t before = s()->applied_count(), hb = core_hist();
        w_->action(QStringLiteral("duplicate_layer"))->trigger();
        Json op = s()->last_attempted_op();
        QCOMPARE(keys(op), (std::set<std::string>{"op", "layer", "id"}));
        QCOMPARE(QString::fromStdString(op["id"].get<std::string>()), QStringLiteral("G copy"));
        expect_one_record("duplicate_layer", before, hb);
        QVERIFY(node("G copy") && node("G copy/Inner"));
        QCOMPARE(s()->active_layer(), std::string("G copy"));
        QVERIFY(item("G copy"));
        QCOMPARE(QString::fromStdString(node("G copy")->name), QStringLiteral("G copy"));  // doc 60 §4.4: display name + " copy"
        s()->undo();
        QVERIFY(!node("G copy"));
        // Isolate Group.
        s()->set_active_layer("G");
        before = s()->applied_count();
        hb = core_hist();
        QVERIFY(!w_->action(QStringLiteral("isolate_group"))->isChecked());
        w_->action(QStringLiteral("isolate_group"))->trigger();
        op = s()->last_attempted_op();
        QCOMPARE(keys(op), (std::set<std::string>{"op", "layer", "mode"}));
        QCOMPARE(QString::fromStdString(op["mode"].get<std::string>()), QStringLiteral("isolated"));
        expect_one_record("set_group_mode", before, hb);
        QCOMPARE(node("G")->mode, rl::BlendMode::Norm);  // doc 60 §6: pass -> isolated = norm
        QVERIFY(w_->action(QStringLiteral("isolate_group"))->isChecked());
        QVERIFY(item("G")->text(1).contains(QStringLiteral("Isolated")));
        // ... and back to Pass Through.
        before = s()->applied_count();
        hb = core_hist();
        w_->action(QStringLiteral("isolate_group"))->trigger();
        QCOMPARE(QString::fromStdString(s()->last_attempted_op()["mode"].get<std::string>()), QStringLiteral("pass"));
        expect_one_record("set_group_mode", before, hb);
        QCOMPARE(node("G")->mode, rl::BlendMode::Pass);
        QVERIFY(item("G")->text(1).contains(QStringLiteral("Pass Through")));
        // Ctrl+click the thumbnail of a raster layer.
        for (const auto& [mods, mode] : std::vector<std::pair<Qt::KeyboardModifiers, const char*>>{
                 {Qt::ControlModifier, "new"},
                 {Qt::ControlModifier | Qt::ShiftModifier, "add"},
                 {Qt::ControlModifier | Qt::AltModifier, "subtract"},
                 {Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier, "intersect"}}) {
            QTreeWidgetItem* it = item("Paint");
            QVERIFY(it);
            QTreeWidget* t = w_->layers_panel()->tree();
            t->scrollToItem(it);
            const QRect col = t->visualRect(t->indexFromItem(it, 1));
            before = s()->applied_count();
            hb = core_hist();
            QTest::mouseClick(t->viewport(), Qt::LeftButton, mods, QPoint(col.left() + 12, col.center().y()));
            op = s()->last_attempted_op();
            if (std::string(mode) == "new" || std::string(mode) == "add") QVERIFY(s()->state().selection.active());
            QCOMPARE(QString::fromStdString(op["op"].get<std::string>()), QStringLiteral("select_alpha"));
            QCOMPARE(QString::fromStdString(op["layer"].get<std::string>()), QStringLiteral("Paint"));
            QCOMPARE(QString::fromStdString(op["mode"].get<std::string>()), QString::fromLatin1(mode));
            expect_one_record("select_alpha", before, hb);
        }
        // (After new, add, subtract and intersect of the same alpha the selection is empty again.)
        QVERIFY(!s()->state().selection.active());
        // A plain click on the thumbnail (no Ctrl) only selects the row: no op.
        {
            QTreeWidget* t = w_->layers_panel()->tree();
            const QRect col = t->visualRect(t->indexFromItem(item("Inner"), 1));
            before = s()->applied_count();
            QTest::mouseClick(t->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(col.left() + 12, col.center().y()));
            QCOMPARE(s()->applied_count(), before);
            QCOMPARE(s()->active_layer(), std::string("Inner"));
        }
        // Delete (the core's doc-60 op) + undo.
        s()->set_active_layer("Paint");
        before = s()->applied_count();
        hb = core_hist();
        w_->action(QStringLiteral("delete_layer"))->trigger();
        expect_one_record("delete_layer", before, hb);
        QVERIFY(!node("Paint"));
        s()->undo();
        QVERIFY(node("Paint"));
    }

    void cleanupTestCase() { w_.reset(); }
};

QTEST_MAIN(GuiEditing)
#include "gui_editing.moc"
