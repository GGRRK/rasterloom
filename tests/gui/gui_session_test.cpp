// SPDX-License-Identifier: GPL-3.0-or-later
//
// EditorSession / op runner / state diff / stroke adapter: the GUI's document plumbing, tested
// without widgets. Every expectation is about bytes or records, not appearance.
#include <QSignalSpy>
#include <QTest>

#include "core/composite/render.hpp"
#include "core/io/export_png.hpp"
#include "gui/op_runner.hpp"
#include "gui/session.hpp"
#include "gui/state_diff.hpp"
#include "gui/stroke_adapter.hpp"

using namespace rl::gui;

namespace {

rl::io::RgbaBuffer render(const EditorSession& s) { return rl::io::render_document(s.state()); }
bool same(const rl::io::RgbaBuffer& a, const rl::io::RgbaBuffer& b) { return a.w == b.w && a.h == b.h && a.px == b.px; }

}  // namespace

class GuiSessionTest : public QObject {
    Q_OBJECT
private slots:
    void one_op_one_record() {
        EditorSession s;
        s.new_document(200, 120, 0);
        QCOMPARE(s.doc().history_size(), size_t{0});
        QVERIFY(s.apply({{"op", "add_layer"}, {"id", "A"}, {"fill", "solid"}, {"color", "#FF0000"}}, "A").ok());
        QVERIFY(s.apply({{"op", "set_opacity"}, {"layer", "A"}, {"value", 0.5}}, "op").ok());
        QCOMPARE(s.doc().history_size(), size_t{2});
        QCOMPARE(s.entries().size(), size_t{2});
        QCOMPARE(s.applied_count(), size_t{2});
    }

    void failed_op_changes_nothing() {
        EditorSession s;
        s.new_document(64, 64, 0);
        const auto before = render(s);
        QSignalSpy msg(&s, &EditorSession::message);
        // Unknown field: a script error. No record, no state change.
        const OpResult r = s.apply({{"op", "add_layer"}, {"id", "X"}, {"bogus", 1}}, "bad");
        QCOMPARE(r.status, OpStatus::Error);
        QCOMPARE(s.doc().history_size(), size_t{0});
        QVERIFY(!s.doc().id_exists("X"));
        QVERIFY(same(before, render(s)));
        QCOMPARE(msg.count(), 1);
        // An unregistered op is NotAvailable, and says so.
        const OpResult n = s.apply({{"op", "definitely_not_an_op"}}, "Thing");
        QCOMPARE(n.status, OpStatus::NotAvailable);
        QVERIFY(msg.last().at(0).toString().contains(QLatin1String("not available in this build")));
        QCOMPARE(s.doc().history_size(), size_t{0});
    }

    void undo_redo_bytes_exact() {
        EditorSession s;
        s.new_document(150, 100, 0);
        const auto base = render(s);
        s.apply({{"op", "add_layer"}, {"id", "G"}, {"fill", "gradient"}, {"from", "#102030"}, {"to", "#F0E0D0"}}, "g");
        s.apply({{"op", "set_blend"}, {"layer", "G"}, {"mode", "mul"}}, "b");
        const auto after = render(s);
        s.undo(2);
        QCOMPARE(s.applied_count(), size_t{0});
        QVERIFY(same(base, render(s)));
        QVERIFY(s.can_redo());
        s.redo(2);
        QVERIFY(same(after, render(s)));
        QCOMPARE(s.doc().history_size(), size_t{2});
        // A new op after an undo drops the redo tail.
        s.undo();
        s.apply({{"op", "set_opacity"}, {"layer", "G"}, {"value", 0.25}}, "o");
        QVERIFY(!s.can_redo());
        QCOMPARE(s.entries().size(), size_t{2});
        // jump_to walks both ways.
        s.jump_to(0);
        QVERIFY(same(base, render(s)));
        s.jump_to(2);
        QCOMPARE(s.doc().find("G").node->opacity, 0.25);
    }

    void preview_cancel_and_commit() {
        EditorSession s;
        s.new_document(80, 80, 0);
        s.apply({{"op", "add_layer"}, {"id", "L"}, {"fill", "solid"}, {"color", "#336699"}}, "L");
        const auto before = render(s);
        const size_t rec = s.doc().history_size();
        QVERIFY(s.preview({{"op", "set_opacity"}, {"layer", "L"}, {"value", 0.3}}).ok());
        QVERIFY(s.preview({{"op", "set_opacity"}, {"layer", "L"}, {"value", 0.6}}).ok());  // replaces, never stacks
        QCOMPARE(s.doc().history_size(), rec + 1);
        s.cancel_preview();
        QVERIFY(same(before, render(s)));
        QCOMPARE(s.doc().history_size(), rec);
        s.preview({{"op", "set_opacity"}, {"layer", "L"}, {"value", 0.6}});
        s.commit_preview({{"op", "set_opacity"}, {"layer", "L"}, {"value", 0.6}}, "opacity");
        QCOMPARE(s.doc().history_size(), rec + 1);
        QCOMPARE(s.entries().back().label, QStringLiteral("opacity"));
        QCOMPARE(s.doc().find("L").node->opacity, 0.6);
        // Committing an op other than the one previewed (OK pressed before the debounced preview
        // ran) records that op AND its result, never the stale preview.
        QVERIFY(s.preview({{"op", "set_opacity"}, {"layer", "L"}, {"value", 0.2}}).ok());
        QVERIFY(s.commit_preview({{"op", "set_opacity"}, {"layer", "L"}, {"value", 0.9}}, "late").ok());
        QCOMPARE(s.doc().find("L").node->opacity, 0.9);
        QCOMPARE(s.doc().history_size(), rec + 2);
        QCOMPARE(s.entries().back().op["value"].get<double>(), 0.9);
        QVERIFY(!s.previewing());
    }

    void history_depth_trims_entries() {
        EditorSession s;
        s.set_history_depth(3);
        s.new_document(32, 32, 0);
        for (int i = 0; i < 5; ++i) s.apply({{"op", "add_layer"}, {"id", "L" + std::to_string(i)}}, "l");
        QCOMPARE(s.doc().history_size(), size_t{3});
        QCOMPARE(s.applied_count(), size_t{3});
        s.undo(3);
        QVERIFY(!s.can_undo());
        QVERIFY(s.doc().id_exists("L1"));  // the two oldest records were dropped by the core
    }

    void core_layer_ops_no_gui_local_ops() {
        EditorSession s;
        s.new_document(32, 32, 1);
        s.apply({{"op", "add_group"}, {"id", "G"}}, "g");
        s.apply({{"op", "add_layer"}, {"id", "C"}, {"parent", "G"}}, "c");
        // The GUI registers no ops of its own: the id-changing rename is gone (doc 60 §1.2) and
        // delete_layer / set_name are the core's.
        QCOMPARE(s.apply({{"op", "rename_layer"}, {"layer", "C"}, {"id", "Child"}}, "r").status, OpStatus::NotAvailable);
        QVERIFY(s.apply({{"op", "set_name"}, {"layer", "C"}, {"name", "Child"}}, "n").ok());
        QVERIFY(s.doc().id_exists("C"));
        QVERIFY(!s.doc().id_exists("Child"));
        QVERIFY(diff_states(s.entries()[s.applied_count() - 2].after, s.state()).tree_changed);  // the panel rebuilds
        // Core validation: unknown field (the old grammar's "id") is an error and records nothing.
        const size_t n = s.applied_count();
        QCOMPARE(s.apply({{"op", "set_name"}, {"layer", "C"}, {"id", "x"}}, "bad").status, OpStatus::Error);
        QCOMPARE(s.applied_count(), n);
        QVERIFY(s.apply({{"op", "delete_layer"}, {"layer", "G"}}, "d").ok());
        QVERIFY(!s.doc().id_exists("G"));
        QVERIFY(!s.doc().id_exists("C"));
        s.undo();
        QVERIFY(s.doc().id_exists("C"));
    }

    void diff_marks_only_written_tiles() {
        rl::Document d(256, 256, rl::Rgba8{0, 0, 0, 0});
        QVERIFY(apply_op(d, {{"op", "add_layer"}, {"id", "A"}}).ok());
        const rl::DocState before = d.state();
        // A rect inside tile (1, 2) only.
        QVERIFY(apply_op(d, {{"op", "add_layer"}, {"id", "B"}, {"fill", "solid"}, {"rect", {70, 140, 10, 10}}}).ok());
        StateDiff tree = diff_states(before, d.state());
        QVERIFY(tree.all_tiles);  // a new node changes the tree: full recomposite
        const rl::DocState mid = d.state();
        QVERIFY(apply_op(d, {{"op", "set_mask_enabled"}, {"layer", "B"}, {"value", true}}).status == OpStatus::Error);
        QVERIFY(apply_op(d, {{"op", "add_mask"}, {"layer", "B"}, {"fill", "solid"}, {"value", 0}, {"rect", {70, 140, 4, 4}}, {"outside", 255}}).ok());
        StateDiff m = diff_states(mid, d.state());
        QVERIFY(m.all_tiles);  // mask added: compositing props changed
        const rl::DocState s2 = d.state();
        rl::Node& b = *d.find("B").node;
        b.pixels.set(200, 10, rl::Rgba8{1, 2, 3, 255});  // direct tile write (as a paint op would)
        StateDiff px = diff_states(s2, d.state());
        QVERIFY(!px.all_tiles);
        QVERIFY(px.tile_dirty(3, 0));
        QVERIFY(!px.tile_dirty(1, 2));
        QVERIFY(!px.tree_changed);
    }

    void stroke_op_shape() {
        rl::Document doc(64, 48, rl::Rgba8{0, 0, 0, 0}, 50);
        QVERIFY(apply_op(doc, Json{{"op", "add_layer"}, {"id", "L"}}).ok());
        const size_t records = doc.history_size();
        StrokeSetup st;
        st.kind = StrokeKind::Brush;
        st.layer = "L";
        st.color = QColor(0x1e, 0x3a, 0x8a);
        st.brush.size = 12;
        st.brush.pressure_size = true;
        StrokeAdapter a;
        StrokeUpdate first;
        QVERIFY(a.begin(doc, st, {10, 20, 0.5, 0, 0, 0}, &first).empty());
        QVERIFY(first.dabs >= 1);
        QVERIFY(!first.tiles.empty());
        QCOMPARE(doc.history_size(), records + 1);  // the session's one record, pushed at begin
        a.add_sample({30, 20, 0.7, 5, -5, 8});
        QVERIFY(a.add_release_sample({30, 20, 0.7, 5, -5, 8}).rect.isNull());  // same position and time: dropped (doc 40 §5)
        a.add_release_sample({31, 20, 0.7, 5, -5, 9});
        QVERIFY(a.preview_pixels() != nullptr);
        const Json op = a.finish();
        QVERIFY(!a.active());
        QCOMPARE(QString::fromStdString(op["op"].get<std::string>()), QStringLiteral("brush_stroke"));
        QCOMPARE(QString::fromStdString(op["color"].get<std::string>()), QStringLiteral("#1E3A8A"));
        QCOMPARE(op["samples"].size(), size_t{3});
        QCOMPARE(op["size_curve"].size(), size_t{2});
        QVERIFY(op["opacity_curve"].is_null());
        QCOMPARE(QString::fromStdString(op["mode"].get<std::string>()), QStringLiteral("wash"));
        QCOMPARE(doc.history_size(), records + 1);
        // Clone: source always present (self-contained op).
        st.kind = StrokeKind::Clone;
        st.clone_source = QPointF(5, 6);
        const Json c = StrokeAdapter::make_op(st, {{1, 2, 1, 0, 0, 0}});
        QVERIFY(!c.contains("color"));
        QCOMPARE(c["source"][0].get<double>(), 5.0);
        // An invalid setup leaves the document untouched.
        st.kind = StrokeKind::Brush;
        st.layer = "nope";
        StrokeAdapter b;
        QVERIFY(!b.begin(doc, st, {1, 1, 1, 0, 0, 0}).empty());
        QVERIFY(!b.active());
        QCOMPARE(doc.history_size(), records + 1);
    }
};

QTEST_GUILESS_MAIN(GuiSessionTest)
#include "gui_session_test.moc"
