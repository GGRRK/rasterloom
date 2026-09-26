// SPDX-License-Identifier: GPL-3.0-or-later
//
// docs/math/60-editing-ops.md: delete_layer, set_name, duplicate_layer, set_adjustment,
// set_group_mode, select_alpha, the name rules (§1.1) and mutation hooks 40 and 41.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "core/adjust/adjustment.hpp"
#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/edit/names.hpp"
#include "core/script/engine.hpp"
#include "../test_util.hpp"

using namespace rl;
using rltest::make_script;
using rltest::render_text;
using rltest::same_pixels;

namespace {

constexpr const char* kK =
    R"({"op":"add_layer","id":"k","fill":"gradient","from":"#2040C0FF","to":"#F0C020FF","dir":"h"})";

std::unique_ptr<Document> run(const std::string& ops, int w = 16, int h = 16) {
    return script::run_script_text(make_script(w, h, ops)).doc;
}

std::string run_error(const std::string& ops) {
    try {
        script::run_script_text(make_script(16, 16, ops));
    } catch (const ScriptError& e) {
        return e.what();
    }
    ADD_FAILURE() << "accepted: " << ops;
    return {};
}

// Ids of a container's children, bottom to top.
std::vector<std::string> ids(const Node& container) {
    std::vector<std::string> out;
    for (const Node& c : container.children) out.push_back(c.id);
    return out;
}

using V = std::vector<std::string>;

// One op on top of a one-layer document, built as a JSON value (for strings that JSON text cannot
// carry, such as ill-formed UTF-8).
bool set_name_accepted(const std::string& name) {
    script::Json s = {{"canvas", {{"w", 4}, {"h", 4}}},
                      {"ops", script::Json::array({{{"op", "add_layer"}, {"id", "u"}},
                                                   {{"op", "set_name"}, {"layer", "u"}, {"name", name}}})},
                      {"out", "png8"}};
    try {
        auto r = script::run_script(s);
        EXPECT_EQ(r.doc->find("u").node->name, name);
        return true;
    } catch (const ScriptError&) {
        return false;
    }
}

}  // namespace

// ---- registry ----------------------------------------------------------------------------------------
TEST(Editing, CoreRegistersTheSixOpsAndNotRenameLayer) {
    const auto& r = script::default_registry();
    for (const char* op : {"delete_layer", "set_name", "duplicate_layer", "set_adjustment", "set_group_mode",
                           "select_alpha"}) {
        const script::OpSpec* s = r.find(op);
        ASSERT_NE(s, nullptr) << op;
        EXPECT_TRUE(s->records_history) << op;
    }
    EXPECT_EQ(r.find("rename_layer"), nullptr);  // ids are immutable (§1.2)
}

TEST(Editing, EachOpPushesExactlyOneRecord) {
    const std::string setup = std::string(kK) +
                              R"(,{"op":"add_layer","id":"u","fill":"solid","color":"#10E080FF"})"
                              R"(,{"op":"add_group","id":"g"},{"op":"add_layer","id":"x","parent":"g"})"
                              R"(,{"op":"add_adjustment","id":"a","type":"posterize","params":{"levels":3}})";
    const auto base = run(setup);
    const size_t n0 = base->history_size();
    for (const char* op : {R"({"op":"set_name","layer":"u","name":"U"})",
                           R"({"op":"duplicate_layer","layer":"g","id":"g2"})",
                           R"({"op":"set_adjustment","layer":"a","params":{"levels":3}})",
                           R"({"op":"set_group_mode","layer":"g","mode":"pass"})",
                           R"({"op":"select_alpha","layer":"x"})", R"({"op":"delete_layer","layer":"u"})"}) {
        EXPECT_EQ(run(setup + "," + op)->history_size(), n0 + 1) << op;
    }
}

// ---- names (§1.1) --------------------------------------------------------------------------------------
TEST(EditingNames, Validity) {
    EXPECT_TRUE(edit::is_valid_name(""));
    EXPECT_TRUE(edit::is_valid_name("  Sky \xE2\x80\x93 warm  "));
    EXPECT_FALSE(edit::is_valid_name("a\nb"));
    EXPECT_FALSE(edit::is_valid_name(std::string("a\0b", 3)));
    EXPECT_FALSE(edit::is_valid_name("a\x7F"));
    EXPECT_FALSE(edit::is_valid_name("a\x1F"));
    EXPECT_TRUE(edit::is_valid_name("a\xC2\x80"));  // U+0080 is not in the forbidden set
    // Ill-formed UTF-8: stray continuation, truncated, overlong, encoded surrogate, > U+10FFFF.
    EXPECT_FALSE(edit::is_valid_name("\x80"));
    EXPECT_FALSE(edit::is_valid_name("\xE2\x80"));
    EXPECT_FALSE(edit::is_valid_name("\xC0\xAF"));
    EXPECT_FALSE(edit::is_valid_name("\xED\xA0\x80"));
    EXPECT_FALSE(edit::is_valid_name("\xF4\x90\x80\x80"));
    EXPECT_FALSE(edit::is_valid_name("\xFF"));
}

TEST(EditingNames, LengthCountsCodePoints) {
    std::string e254;
    for (int i = 0; i < 254; ++i) e254 += "\xC3\xA9";  // é
    const std::string clef = "\xF0\x9D\x84\x9E";        // U+1D11E
    EXPECT_EQ(edit::utf8_scalar_count(e254 + clef), 255u);
    EXPECT_TRUE(edit::is_valid_name(e254 + clef));      // 512 bytes, 256 UTF-16 units
    EXPECT_FALSE(edit::is_valid_name(e254 + clef + "x"));
    EXPECT_TRUE(edit::is_valid_name(std::string(255, 'a')));
    EXPECT_FALSE(edit::is_valid_name(std::string(256, 'a')));
}

TEST(EditingNames, SetNameThroughTheScriptEngine) {
    EXPECT_TRUE(set_name_accepted(""));
    EXPECT_TRUE(set_name_accepted("\xC3\x9C" "bermalung \xCE\xA9"));
    EXPECT_FALSE(set_name_accepted("tab\there"));
    EXPECT_FALSE(set_name_accepted("bad \xC3"));
    EXPECT_FALSE(set_name_accepted(std::string(256, 'n')));
    // A lone surrogate escape fails while parsing the script text.
    EXPECT_NE(run_error(R"({"op":"add_layer","id":"u"},{"op":"set_name","layer":"u","name":"\ud800x"})"), "");
}

TEST(EditingNames, NamesAreNotAddressesAndIdsNeverChange) {
    auto d = run(std::string(kK) + R"(,{"op":"add_layer","id":"u"},{"op":"set_name","layer":"u","name":"k"})"
                                   R"(,{"op":"set_opacity","layer":"k","value":0.5})");
    EXPECT_EQ(d->find("k").node->opacity, 0.5);
    EXPECT_EQ(d->find("u").node->opacity, 1.0);
    EXPECT_EQ(d->find("u").node->name, "k");
    EXPECT_NE(run_error(R"({"op":"add_layer","id":"u"},{"op":"set_name","layer":"u","name":"v"})"
                        R"(,{"op":"set_opacity","layer":"v","value":0.5})")
                  .find("unknown id 'v'"),
              std::string::npos);
    EXPECT_NE(run_error(R"({"op":"set_name","layer":"root","name":"x"})"), "");
    EXPECT_NE(run_error(R"({"op":"add_layer","id":"u"},{"op":"set_name","layer":"u","name":"x","id":"y"})"), "");
}

TEST(EditingNames, UndoRevertsOnlyTheName) {
    auto d = run(R"({"op":"add_layer","id":"u"},{"op":"set_name","layer":"u","name":"A"})"
                 R"(,{"op":"set_name","layer":"u","name":"B"},{"op":"undo"})");
    ASSERT_NE(d->find("u").node, nullptr);
    EXPECT_EQ(d->find("u").node->name, "A");
}

// ---- delete_layer (§2) -------------------------------------------------------------------------------
TEST(EditingDelete, RemovesTheSubtreeAndFreesItsIds) {
    auto d = run(R"({"op":"add_layer","id":"a"},{"op":"add_group","id":"g"},{"op":"add_layer","id":"c","parent":"g"})"
                 R"(,{"op":"add_layer","id":"b"},{"op":"delete_layer","layer":"g"})"
                 R"(,{"op":"add_layer","id":"c"},{"op":"add_group","id":"g"})");
    EXPECT_EQ(ids(d->root()), (V{"a", "b", "c", "g"}));
    EXPECT_TRUE(d->find("g").node->children.empty());
}

TEST(EditingDelete, KeepsClipFlagsSelectionAndSaved) {
    auto d = run(R"({"op":"add_layer","id":"b0"},{"op":"add_layer","id":"base"},{"op":"add_layer","id":"c"})"
                 R"(,{"op":"set_clip","layer":"c","value":true},{"op":"select_rect","x":0,"y":0,"w":4,"h":4})"
                 R"(,{"op":"deselect"},{"op":"select_rect","x":2,"y":2,"w":4,"h":4},{"op":"delete_layer","layer":"base"})");
    EXPECT_EQ(ids(d->root()), (V{"b0", "c"}));
    EXPECT_TRUE(d->find("c").node->clip);
    EXPECT_TRUE(d->selection().active());
    EXPECT_EQ(d->selection().mask.get(3, 3), 255);
    EXPECT_EQ(d->selection().mask.get(0, 0), 0);
    ASSERT_TRUE(d->selection().saved.has_value());
    EXPECT_EQ(d->selection().saved->get(0, 0), 255);
}

TEST(EditingDelete, DeletingEveryNodeRendersTheBackground) {
    const auto img = render_text(make_script(4, 4,
                                             R"({"op":"add_layer","id":"a","fill":"solid","color":"#FF0000FF"})"
                                             R"(,{"op":"add_group","id":"g"},{"op":"add_layer","id":"b","parent":"g"})"
                                             R"(,{"op":"delete_layer","layer":"a"},{"op":"delete_layer","layer":"g"})",
                                             "#808080FF"));
    ASSERT_FALSE(img.px.empty());
    for (const Rgba8& p : img.px) {
        EXPECT_EQ(p.r, 0x80);
        EXPECT_EQ(p.a, 0xFF);
    }
}

TEST(EditingDelete, Errors) {
    EXPECT_NE(run_error(R"({"op":"delete_layer","layer":"root"})"), "");
    EXPECT_NE(run_error(R"({"op":"delete_layer","layer":"nope"})").find("unknown id"), std::string::npos);
    EXPECT_NE(run_error(R"({"op":"delete_layer"})"), "");
    EXPECT_NE(run_error(R"({"op":"add_layer","id":"u"},{"op":"delete_layer","layer":"u","x":1})"), "");
    EXPECT_NE(run_error(R"({"op":"add_layer","id":"u"},{"op":"delete_layer","layer":7})"), "");
}

// ---- duplicate_layer (§4) ------------------------------------------------------------------------------
TEST(EditingDuplicate, PlacementDirectlyAboveTheSource) {
    auto d = run(R"({"op":"add_layer","id":"u"},{"op":"add_layer","id":"t"},{"op":"duplicate_layer","layer":"u","id":"u2"})");
    EXPECT_EQ(ids(d->root()), (V{"u", "u2", "t"}));
}

TEST(EditingDuplicate, ExplicitParentMeansTopEvenForTheOwnContainer) {
    auto d = run(R"({"op":"add_layer","id":"u"},{"op":"add_layer","id":"t"},{"op":"add_group","id":"g"})"
                 R"(,{"op":"add_layer","id":"x","parent":"g"})"
                 R"(,{"op":"duplicate_layer","layer":"u","id":"u2","parent":"root"})"
                 R"(,{"op":"duplicate_layer","layer":"u","id":"u3","parent":"g"})");
    EXPECT_EQ(ids(d->root()), (V{"u", "t", "g", "u2"}));
    EXPECT_EQ(ids(*d->find("g").node), (V{"x", "u3"}));
}

TEST(EditingDuplicate, Mutation40PutsTheCopyAtTheTop) {
    mut::ScopedMutations m({40});
    auto d = run(R"({"op":"add_layer","id":"u"},{"op":"add_layer","id":"t"},{"op":"duplicate_layer","layer":"u","id":"u2"})");
    EXPECT_EQ(ids(d->root()), (V{"u", "t", "u2"}));
}

TEST(EditingDuplicate, DeepCopyWithFlatDerivedIdsAndNames) {
    auto d = run(R"({"op":"add_group","id":"g","mode":"scrn"},{"op":"add_layer","id":"c1","parent":"g"})"
                 R"(,{"op":"add_group","id":"h","parent":"g"},{"op":"add_layer","id":"d","parent":"h"})"
                 R"(,{"op":"set_name","layer":"c1","name":"First"},{"op":"set_name","layer":"g","name":"Grp"})"
                 R"(,{"op":"duplicate_layer","layer":"g","id":"g2"})");
    EXPECT_EQ(ids(d->root()), (V{"g", "g2"}));
    const Node& g2 = *d->find("g2").node;
    EXPECT_EQ(g2.name, "Grp copy");
    EXPECT_EQ(g2.mode, BlendMode::Scrn);
    EXPECT_EQ(ids(g2), (V{"g2/c1", "g2/h"}));
    EXPECT_EQ(ids(g2.children[1]), (V{"g2/d"}));
    EXPECT_EQ(g2.children[0].name, "First");
    EXPECT_EQ(g2.children[1].name, "h");             // display_name of an unnamed child = its id
    EXPECT_EQ(g2.children[1].children[0].name, "d");
    // Second generation: §4.3's formula (top id + "/" + the descendant's own id, and g2's child's own
    // id is "g2/c1") gives "g3/g2/c1". §4.3's prose example says "g3/c1"; that disagreement is
    // reported as a doc dispute, and no golden pins it. This test pins the formula.
    auto d2 = run(R"({"op":"add_group","id":"g"},{"op":"add_layer","id":"c1","parent":"g"})"
                  R"(,{"op":"duplicate_layer","layer":"g","id":"g2"},{"op":"duplicate_layer","layer":"g2","id":"g3"})");
    EXPECT_EQ(ids(*d2->find("g3").node), (V{"g3/g2/c1"}));
}

TEST(EditingDuplicate, CopyNameRules) {
    auto d = run(R"({"op":"add_layer","id":"u"},{"op":"duplicate_layer","layer":"u","id":"u2"})"
                 R"(,{"op":"set_name","layer":"u","name":")" + std::string(250, 'n') + R"("})"
                 R"(,{"op":"duplicate_layer","layer":"u","id":"u3"})"
                 R"(,{"op":"set_name","layer":"u","name":")" + std::string(251, 'n') + R"("})"
                 R"(,{"op":"duplicate_layer","layer":"u","id":"u4"})"
                 R"(,{"op":"add_layer","id":"bad\nid"},{"op":"duplicate_layer","layer":"bad\nid","id":"b2"})");
    EXPECT_EQ(d->find("u2").node->name, "u copy");
    EXPECT_EQ(d->find("u3").node->name, std::string(250, 'n') + " copy");  // 255 code points
    EXPECT_EQ(d->find("u4").node->name, "");                                // 256 -> ""
    EXPECT_EQ(d->find("b2").node->name, "");                                // id with a control char
}

TEST(EditingDuplicate, EveryPropertyCopiedAndStorageIndependent) {
    auto d = run(R"({"op":"add_layer","id":"u","fill":"noise","seed":5,"alpha":"random"})"
                 R"(,{"op":"add_mask","layer":"u","fill":"gradient","outside":7},{"op":"set_mask_enabled","layer":"u","value":false})"
                 R"(,{"op":"set_blend","layer":"u","mode":"diss","seed":77},{"op":"set_fill","layer":"u","value":0.6})"
                 R"(,{"op":"set_opacity","layer":"u","value":0.7},{"op":"lock_transparency","layer":"u"})"
                 R"(,{"op":"set_visible","layer":"u","value":false},{"op":"set_clbl","layer":"u","value":false})"
                 R"(,{"op":"duplicate_layer","layer":"u","id":"u2"})"
                 R"(,{"op":"lock_transparency","layer":"u2","value":false})"
                 R"(,{"op":"select_rect","x":4,"y":4,"w":8,"h":8},{"op":"fill_selection","layer":"u2","color":"#FF00FFFF"})");
    const Node& u = *d->find("u").node;
    const Node& u2 = *d->find("u2").node;
    EXPECT_EQ(u2.mode, BlendMode::Diss);
    EXPECT_EQ(u2.seed, 77u);
    EXPECT_EQ(u2.fill, 0.6);
    EXPECT_EQ(u2.opacity, 0.7);
    EXPECT_FALSE(u2.visible);
    EXPECT_FALSE(u2.clbl);
    EXPECT_FALSE(u2.clip);
    ASSERT_TRUE(u2.mask.has_value());
    EXPECT_FALSE(u2.mask->enabled);
    EXPECT_EQ(u2.mask->plane.background(), 7);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) ASSERT_EQ(u.mask->plane.get(x, y), u2.mask->plane.get(x, y));
    EXPECT_TRUE(u.lock_alpha);
    EXPECT_FALSE(u2.lock_alpha);
    // The fill changed u2 only.
    int differ = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const Rgba8 a = u.pixels.get(x, y), b = u2.pixels.get(x, y);
            differ += (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) ? 1 : 0;
        }
    EXPECT_GT(differ, 0);
    auto fresh = run(R"({"op":"add_layer","id":"u","fill":"noise","seed":5,"alpha":"random"})");
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const Rgba8 a = u.pixels.get(x, y), b = fresh->find("u").node->pixels.get(x, y);
            ASSERT_TRUE(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a);
        }
}

TEST(EditingDuplicate, AdjustmentParamsIndependent) {
    auto d = run(R"({"op":"add_adjustment","id":"a","type":"posterize","params":{"levels":3}})"
                 R"(,{"op":"duplicate_layer","layer":"a","id":"a2"})"
                 R"(,{"op":"set_adjustment","layer":"a2","params":{"levels":6}})");
    EXPECT_EQ(std::get<adjust::PosterizeParams>(d->find("a").node->adjustment->params()).levels, 3);
    EXPECT_EQ(std::get<adjust::PosterizeParams>(d->find("a2").node->adjustment->params()).levels, 6);
}

TEST(EditingDuplicate, ErrorsLeaveTheTreeUntouched) {
    const std::string g = R"({"op":"add_group","id":"g"},{"op":"add_group","id":"h","parent":"g"})"
                          R"(,{"op":"add_layer","id":"c1","parent":"g"},{"op":"add_layer","id":"r"})";
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"g","id":"r"})").find("duplicate id"), std::string::npos);
    EXPECT_NE(run_error(g + R"(,{"op":"add_layer","id":"g2/c1"},{"op":"duplicate_layer","layer":"g","id":"g2"})")
                  .find("derived id 'g2/c1'"),
              std::string::npos);
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"g","id":"g2","parent":"g"})"), "");
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"g","id":"g2","parent":"h"})"), "");
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"g","id":"g2","parent":"r"})"), "");
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"g","id":"g2","parent":"zz"})"), "");
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"g","id":"g2","parent":3})"), "");
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"root","id":"g2"})"), "");
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"g","id":"root"})"), "");
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"g","id":""})"), "");
    EXPECT_NE(run_error(g + R"(,{"op":"duplicate_layer","layer":"g"})"), "");

    // The handler throws before changing anything: run it directly against a document.
    Document doc(8, 8, Rgba8{});
    doc.root().children.push_back(Node::make_group("g"));
    doc.root().children[0].children.push_back(Node::make_raster("c1", 8, 8));
    doc.root().children.push_back(Node::make_raster("g2/c1", 8, 8));
    const script::OpSpec* spec = script::default_registry().find("duplicate_layer");
    const script::Json op = {{"op", "duplicate_layer"}, {"layer", "g"}, {"id", "g2"}};
    script::OpContext ctx{doc, 0, "t"};
    script::Fields f(op, "t");
    f.consume("op");
    EXPECT_THROW(spec->run(ctx, f), ScriptError);
    EXPECT_EQ(ids(doc.root()), (V{"g", "g2/c1"}));
}

// ---- set_adjustment (§5) --------------------------------------------------------------------------------
TEST(EditingSetAdjustment, ReplacesWholesale) {
    const std::string created = R"({"op":"add_adjustment","id":"a","type":"levels","params":{"rgb":{"in_black":40,"gamma":1.6}}})";
    auto d = run(created + R"(,{"op":"set_adjustment","layer":"a","params":{"rgb":{"in_white":200}}})");
    const auto& p = std::get<adjust::LevelsParams>(d->find("a").node->adjustment->params());
    auto ref = run(R"({"op":"add_adjustment","id":"a","type":"levels","params":{"rgb":{"in_white":200}}})");
    const auto& q = std::get<adjust::LevelsParams>(ref->find("a").node->adjustment->params());
    EXPECT_EQ(adjust::params_to_json(p), adjust::params_to_json(q));
    EXPECT_EQ(d->find("a").node->adjust_params, R"({"rgb":{"in_white":200}})");

    auto reset = run(created + R"(,{"op":"set_adjustment","layer":"a","params":{}})");
    auto def = run(R"({"op":"add_adjustment","id":"a","type":"levels"})");
    EXPECT_EQ(adjust::params_to_json(reset->find("a").node->adjustment->params()),
              adjust::params_to_json(def->find("a").node->adjustment->params()));
}

TEST(EditingSetAdjustment, Mutation41MergesTheOldParams) {
    mut::ScopedMutations m({41});
    auto d = run(R"({"op":"add_adjustment","id":"a","type":"levels","params":{"rgb":{"in_black":40,"gamma":1.6}}})"
                 R"(,{"op":"set_adjustment","layer":"a","params":{"rgb":{"in_white":200}}})");
    const auto j = adjust::params_to_json(d->find("a").node->adjustment->params());
    EXPECT_EQ(j["rgb"]["in_black"], 40);
    EXPECT_EQ(j["rgb"]["in_white"], 200);
}

TEST(EditingSetAdjustment, KeepsEveryOtherProperty) {
    auto d = run(R"({"op":"add_layer","id":"x"},{"op":"add_adjustment","id":"a","type":"threshold","params":{"level":100}})"
                 R"(,{"op":"set_clip","layer":"a","value":true},{"op":"set_fill","layer":"a","value":0.5})"
                 R"(,{"op":"set_blend","layer":"a","mode":"over"},{"op":"set_opacity","layer":"a","value":0.8})"
                 R"(,{"op":"add_mask","layer":"a","value":9},{"op":"set_name","layer":"a","name":"T"})"
                 R"(,{"op":"set_adjustment","layer":"a","params":{"level":180}})");
    const Node& a = *d->find("a").node;
    EXPECT_TRUE(a.clip);
    EXPECT_EQ(a.fill, 0.5);
    EXPECT_EQ(a.mode, BlendMode::Over);
    EXPECT_EQ(a.opacity, 0.8);
    ASSERT_TRUE(a.mask.has_value());
    EXPECT_EQ(a.mask->plane.get(0, 0), 9);
    EXPECT_EQ(a.name, "T");
    EXPECT_EQ(ids(d->root()), (V{"x", "a"}));
    EXPECT_EQ(std::string(a.adjustment->type()), "threshold");
    EXPECT_EQ(std::get<adjust::ThresholdParams>(a.adjustment->params()).level, 180);
}

TEST(EditingSetAdjustment, Errors) {
    const std::string a = R"({"op":"add_layer","id":"r"},{"op":"add_group","id":"g"})"
                          R"(,{"op":"add_adjustment","id":"p","type":"posterize"},{"op":"add_adjustment","id":"t","type":"threshold"})";
    EXPECT_NE(run_error(a + R"(,{"op":"set_adjustment","layer":"r","params":{}})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"set_adjustment","layer":"g","params":{}})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"set_adjustment","layer":"root","params":{}})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"set_adjustment","layer":"p"})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"set_adjustment","layer":"p","params":[]})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"set_adjustment","layer":"p","params":null})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"set_adjustment","layer":"p","type":"posterize","params":{}})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"set_adjustment","layer":"p","params":{"levels":1}})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"set_adjustment","layer":"t","params":{"levels":4}})"), "");
}

// ---- set_group_mode (§6) ---------------------------------------------------------------------------------
TEST(EditingGroupMode, Semantics) {
    auto d = run(R"({"op":"add_group","id":"p"},{"op":"add_group","id":"s","mode":"scrn"})"
                 R"(,{"op":"set_group_mode","layer":"p","mode":"isolated"},{"op":"set_group_mode","layer":"s","mode":"isolated"})");
    EXPECT_EQ(d->find("p").node->mode, BlendMode::Norm);
    EXPECT_EQ(d->find("s").node->mode, BlendMode::Scrn);
    auto e = run(R"({"op":"add_group","id":"s","mode":"scrn"},{"op":"set_group_mode","layer":"s","mode":"pass"})"
                 R"(,{"op":"set_group_mode","layer":"s","mode":"isolated"})");
    EXPECT_EQ(e->find("s").node->mode, BlendMode::Norm);  // no memory of the earlier mode
    EXPECT_NE(run_error(R"({"op":"add_group","id":"g"},{"op":"set_group_mode","layer":"g","mode":"norm"})"), "");
    EXPECT_NE(run_error(R"({"op":"add_layer","id":"r"},{"op":"set_group_mode","layer":"r","mode":"pass"})"), "");
    EXPECT_NE(run_error(R"({"op":"set_group_mode","layer":"root","mode":"pass"})"), "");
    EXPECT_NE(run_error(R"({"op":"add_group","id":"g"},{"op":"set_group_mode","layer":"g"})"), "");
    EXPECT_NE(run_error(R"({"op":"add_group","id":"g"},{"op":"set_group_mode","layer":"g","mode":1})"), "");
}

// ---- select_alpha (§7) -----------------------------------------------------------------------------------
TEST(EditingSelectAlpha, OwnAlphaOnlyAndCombineModes) {
    // src: alpha ramp 0..255 across x (16 px), hidden, masked, low opacity; all ignored.
    const std::string src =
        R"({"op":"add_layer","id":"src","fill":"gradient","from":"#FF000000","to":"#FF0000FF"})"
        R"(,{"op":"add_mask","layer":"src","fill":"noise","seed":3},{"op":"set_opacity","layer":"src","value":0.3})"
        R"(,{"op":"set_visible","layer":"src","value":false})";
    auto d = run(src + R"(,{"op":"select_alpha","layer":"src"})");
    const Node& s = *d->find("src").node;
    for (int x = 0; x < 16; ++x) EXPECT_EQ(d->selection().mask.get(x, 5), s.pixels.get(x, 5).a) << x;

    const std::string rect = R"(,{"op":"select_rect","x":0,"y":0,"w":8,"h":16})";
    auto add = run(src + rect + R"(,{"op":"select_alpha","layer":"src","mode":"add"})");
    auto sub = run(src + rect + R"(,{"op":"select_alpha","layer":"src","mode":"subtract"})");
    auto isc = run(src + rect + R"(,{"op":"select_alpha","layer":"src","mode":"intersect"})");
    for (int x = 0; x < 16; ++x) {
        const int b = s.pixels.get(x, 5).a;
        const int r = x < 8 ? 255 : 0;
        EXPECT_EQ(add->selection().mask.get(x, 5), std::max(r, b));
        EXPECT_EQ(sub->selection().mask.get(x, 5), std::max(r - b, 0));
        EXPECT_EQ(isc->selection().mask.get(x, 5), std::min(r, b));
    }
}

TEST(EditingSelectAlpha, EmptyLayerMeansNoSelectionAndSavedIsKept) {
    auto d = run(R"({"op":"add_layer","id":"e"},{"op":"select_rect","x":0,"y":0,"w":4,"h":4},{"op":"deselect"})"
                 R"(,{"op":"select_rect","x":4,"y":4,"w":4,"h":4},{"op":"select_alpha","layer":"e"})");
    EXPECT_FALSE(d->selection().active());
    ASSERT_TRUE(d->selection().saved.has_value());
    EXPECT_EQ(d->selection().saved->get(1, 1), 255);
    EXPECT_EQ(d->selection().saved->get(5, 5), 0);
}

TEST(EditingSelectAlpha, Errors) {
    const std::string a = R"({"op":"add_layer","id":"r"},{"op":"add_group","id":"g"},{"op":"add_adjustment","id":"a","type":"invert"})";
    EXPECT_NE(run_error(a + R"(,{"op":"select_alpha","layer":"g"})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"select_alpha","layer":"a"})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"select_alpha","layer":"root"})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"select_alpha","layer":"r","mode":"xor"})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"select_alpha","layer":"r","mode":1})"), "");
    EXPECT_NE(run_error(a + R"(,{"op":"select_alpha","layer":"r","feather":1})"), "");
}

// ---- pixels: a duplicate of a clip base re-bases the clipped layers (§4.2) --------------------------------
TEST(EditingPixels, DuplicateOfUnnamedHiddenSourceRendersNothingNew) {
    const auto base = render_text(make_script(16, 16, kK));
    const auto dup = render_text(make_script(16, 16, std::string(kK) +
                                                         R"(,{"op":"add_layer","id":"u","fill":"solid","color":"#FF0000FF"})"
                                                         R"(,{"op":"set_visible","layer":"u","value":false})"
                                                         R"(,{"op":"duplicate_layer","layer":"u","id":"u2"})"));
    EXPECT_TRUE(same_pixels(base, dup));
}
