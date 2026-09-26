// SPDX-License-Identifier: GPL-3.0-or-later
//
// BUILD-SPEC requirement 2: the dense work buffers of selection ops, canvas geometry ops, flips and
// bucket fill are charged to the memory hard limit (mem::Reservation) before they are allocated.
// Each case forces a hard limit just above what is already in use and checks that the op fails
// with a MemoryError naming its own buffers (so the refusal came from the reservation, before any
// dense allocation or tile write) and leaves the document unchanged.
#include <gtest/gtest.h>

#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "core/doc/document.hpp"
#include "core/geometry/canvas_ops.hpp"
#include "core/geometry/dense.hpp"
#include "core/paint/paint.hpp"
#include "core/script/engine.hpp"
#include "core/script/fields.hpp"
#include "core/script/registry.hpp"
#include "core/select/selection_ops.hpp"
#include "core/tile/memory.hpp"
#include "core/transform/transform.hpp"

using namespace rl;

namespace {

class LimitsScope {
public:
    explicit LimitsScope(uint64_t hard) : saved_(mem::limits()) {
        mem::Limits l = saved_;
        l.hard = hard;
        mem::set_limits(l);
    }
    ~LimitsScope() { mem::set_limits(saved_); }
    LimitsScope(const LimitsScope&) = delete;
    LimitsScope& operator=(const LimitsScope&) = delete;

private:
    mem::Limits saved_;
};

uint64_t charged_now() {
    const mem::Stats s = mem::stats();
    return s.hot_bytes + s.warm_bytes + s.reserved_bytes;
}

// 256 x 256 (one RGBA dense copy = 256 KiB, one selection plane = 64 KiB), a fully painted layer
// "a" and an active selection.
std::unique_ptr<Document> make_doc(int w = 256, int h = 256, bool paint = true) {
    std::string ops = paint ? R"({"op":"add_layer","id":"a","fill":"gradient","from":"#102030FF","to":"#F0E0D0FF","dir":"h"},)"
                            : R"({"op":"add_layer","id":"a"},)";
    ops += R"({"op":"select_rect","x":10,"y":12,"w":100,"h":90,"antialias":false})";
    const std::string s = R"({"canvas":{"w":)" + std::to_string(w) + R"(,"h":)" + std::to_string(h) +
                          R"(,"bg":"#00000000"},"ops":[)" + ops + R"(],"out":"png8"})";
    return std::move(script::run_script_text(s).doc);
}

void run_op(Document& doc, const std::string& text) {
    const script::Json op = script::Json::parse(text);
    const std::string name = op.at("op").get<std::string>();
    const script::OpSpec* spec = script::default_registry().find(name);
    ASSERT_NE(spec, nullptr) << name;
    script::OpContext ctx{doc, 0, name};
    script::Fields f(op, name);
    f.consume("op");
    spec->run(ctx, f);
    f.finish();
}

struct Snapshot {
    int w, h;
    RgbaImage pixels;
    GrayImage selection;
    explicit Snapshot(const DocState& s)
        : w(s.w), h(s.h), pixels(s.root.children.at(0).pixels), selection(s.selection.mask) {}
    void expect_same(const DocState& s) const {
        EXPECT_EQ(s.w, w);
        EXPECT_EQ(s.h, h);
        EXPECT_TRUE(s.root.children.at(0).pixels.pixels_equal(pixels));
        EXPECT_TRUE(s.selection.mask.pixels_equal(selection));
    }
};

// Runs `op` with the hard limit at (bytes in use + slack) and expects a MemoryError mentioning
// `label`; afterwards every reservation is released.
void expect_refused(const std::function<void()>& op, const std::string& label, uint64_t slack = 16u << 10) {
    const uint64_t reserved0 = mem::stats().reserved_bytes;
    {
        LimitsScope ls(charged_now() + slack);
        try {
            op();
            ADD_FAILURE() << "no MemoryError for '" << label << "'";
        } catch (const mem::MemoryError& e) {
            EXPECT_NE(std::string(e.what()).find(label), std::string::npos) << e.what();
        }
    }
    EXPECT_EQ(mem::stats().reserved_bytes, reserved0) << label;
}

}  // namespace

TEST(MemoryAccounting, CanvasGeometryOpsReserveTheirDenseCopies) {
    struct Case {
        const char* label;
        std::function<void(DocState&)> op;
    };
    const std::vector<Case> cases = {
        {"image_size (nearest)", [](DocState& s) { geom::image_size(s, 200, 180, transform::Interp::Nearest); }},
        {"crop / canvas_size", [](DocState& s) { geom::crop(s, 10, 10, 100, 100); }},
        {"crop / canvas_size", [](DocState& s) { geom::canvas_size(s, 300, 280, geom::Anchor::C); }},
        {"rotate_canvas", [](DocState& s) { geom::rotate_canvas(s, 90.0); }},
        {"flip_canvas", [](DocState& s) { geom::flip_canvas(s, true); }},
        {"flip_layer", [](DocState& s) { geom::flip_layer(s.root.children.at(0), false); }},
    };
    for (const Case& c : cases) {
        auto doc = make_doc();
        const Snapshot before(doc->state());
        expect_refused([&] { c.op(doc->state()); }, c.label);
        before.expect_same(doc->state());
        // With the normal limit the same op runs and releases its reservation.
        const uint64_t reserved0 = mem::stats().reserved_bytes;
        EXPECT_NO_THROW(c.op(doc->state())) << c.label;
        EXPECT_EQ(mem::stats().reserved_bytes, reserved0) << c.label;
    }
}

TEST(MemoryAccounting, SelectionOpsReserveTheirPlanes) {
    struct Case {
        const char* label;
        std::function<void(Document&)> op;
    };
    const std::vector<Case> cases = {
        {"selection shape coverage", [](Document& d) { run_op(d, R"({"op":"select_ellipse","x":5,"y":5,"w":90,"h":70})"); }},
        {"selection shape coverage",
         [](Document& d) { run_op(d, R"({"op":"select_polygon","points":[[1,1],[200,30],[40,220]],"mode":"add"})"); }},
        {"select_inverse plane", [](Document& d) { select::select_inverse(d.state().selection); }},
        {"feather planes", [](Document& d) { select::feather(d.state().selection, 3.0); }},
        {"expand/contract planes", [](Document& d) { select::expand(d.state().selection, 2); }},
        {"expand/contract planes", [](Document& d) { select::contract(d.state().selection, 2); }},
        {"select_wand layer copy", [](Document& d) { run_op(d, R"({"op":"select_wand","layer":"a","x":3,"y":3})"); }},
        {"select_alpha planes", [](Document& d) { run_op(d, R"({"op":"select_alpha","layer":"a"})"); }},
        {"bucket_fill layer copy", [](Document& d) {
             paint::BucketParams p;
             p.x = 3;
             p.y = 3;
             p.color = Rgba8{200, 10, 10, 255};
             paint::bucket_fill(d.state().root.children.at(0), d.state().selection, p);
         }},
    };
    for (const Case& c : cases) {
        auto doc = make_doc();
        const Snapshot before(doc->state());
        expect_refused([&] { c.op(*doc); }, c.label);
        before.expect_same(doc->state());
        const uint64_t reserved0 = mem::stats().reserved_bytes;
        EXPECT_NO_THROW(c.op(*doc)) << c.label;
        EXPECT_EQ(mem::stats().reserved_bytes, reserved0) << c.label;
    }
}

TEST(MemoryAccounting, HelpersChargeTheirOwnBuffers) {
    auto doc = make_doc();
    const DocState& s = doc->state();
    const std::vector<Rgba8> px = geom::to_dense(s.root.children.at(0).pixels);
    const std::vector<uint8_t> B = select::rasterize_rect(256, 256, 20, 20, 50, 50, false);
    Selection sel = s.selection;
    // combine: B plus the dense S (2 x 64 KiB) against 16 KiB of room.
    expect_refused([&] { select::combine(sel, B, select::Mode::Add); }, "selection combine planes");
    expect_refused([&] { (void)select::region(px, 256, 256, 3, 3, 0, false, false); }, "colour region planes");
    expect_refused([&] { (void)select::edge_mask(s.selection.mask, 128); }, "selection edge planes");
    expect_refused([&] { (void)select::outline_polylines(s.selection.mask, 128); }, "selection outline plane");
    // Room for region's two planes but not for its growing flood-fill stack (a uniform layer floods
    // the whole canvas, so the stack grows well past 1 KiB).
    const std::vector<Rgba8> flat(static_cast<size_t>(256) * 256, Rgba8{7, 7, 7, 255});
    expect_refused([&] { (void)select::region(flat, 256, 256, 128, 128, 0, true, false); }, "flood-fill stack",
                   (2u * 65536u) + 1024u);
}

TEST(MemoryAccounting, HugeCanvasOpFailsCleanlyInsteadOfOom) {
    // 16384 x 16384 with an empty (sparse) layer costs almost nothing to hold; flipping it needs two
    // 1 GiB dense copies. Under a 256 MiB hard limit the op must refuse up front.
    auto doc = make_doc(16384, 16384, /*paint=*/false);
    DocState& s = doc->state();
    LimitsScope ls(charged_now() + (256ull << 20));
    const uint64_t reserved0 = mem::stats().reserved_bytes;
    EXPECT_THROW(geom::flip_canvas(s, true), mem::MemoryError);
    EXPECT_THROW(geom::crop(s, 0, 0, 16384, 16384), mem::MemoryError);
    EXPECT_THROW(select::feather(s.selection, 4.0), mem::MemoryError);  // 10 B/px = 2.5 GiB
    EXPECT_THROW(select::select_inverse(s.selection), mem::MemoryError);  // 256 MiB + what is in use
    EXPECT_EQ(s.w, 16384);
    EXPECT_EQ(mem::stats().reserved_bytes, reserved0);
    EXPECT_EQ(s.root.children.at(0).pixels.allocated_tiles(), 0u);
}
