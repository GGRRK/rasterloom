// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render-script parsing and validation (C9, doc 10 §11): every invalid input is a script error.
#include <gtest/gtest.h>

#include <algorithm>

#include "core/base/error.hpp"
#include "core/script/domains.hpp"
#include "test_util.hpp"

using namespace rl;
using rltest::make_script;

namespace {
void expect_error(const std::string& json, const char* needle = nullptr) {
    try {
        script::run_script_text(json);
        ADD_FAILURE() << "accepted: " << json;
    } catch (const ScriptError& e) {
        if (needle) {
            EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << e.what();
        }
    }
}
void expect_ops_error(const std::string& ops, const char* needle = nullptr) { expect_error(make_script(16, 16, ops), needle); }
}  // namespace

TEST(Script, BuildSpecExampleRuns) {
    auto res = script::run_script_file(std::string(RL_SMOKE_DIR) + "/buildspec_example.json");
    EXPECT_EQ(res.out, "png8");
    EXPECT_EQ(res.doc->width(), 256);
    ASSERT_EQ(res.doc->root().children.size(), 1u);
    EXPECT_EQ(res.doc->history_size(), 5u);  // one record per op
}

TEST(Script, TopLevelShape) {
    expect_error(R"({"canvas":{"w":4,"h":4},"ops":[]})", "missing required field 'out'");
    expect_error(R"({"canvas":{"w":4,"h":4},"ops":[],"out":"png16"})");
    expect_error(R"({"canvas":{"w":4,"h":4},"ops":[],"out":"png8","extra":1})", "unknown field 'extra'");
    expect_error(R"({"canvas":{"w":0,"h":4},"ops":[],"out":"png8"})");
    expect_error(R"({"canvas":{"w":16385,"h":4},"ops":[],"out":"png8"})");
    expect_error(R"({"canvas":{"w":4.0,"h":4},"ops":[],"out":"png8"})", "JSON integer");
    expect_error(R"({"canvas":{"w":4,"h":4,"bg":"#12345"},"ops":[],"out":"png8"})");
    expect_error(R"({"canvas":{"w":4,"h":4},"ops":{},"out":"png8"})");
    expect_error(R"({"canvas":{"w":4,"h":4},"ops":[],"out":"png8")", "invalid JSON");
    EXPECT_NO_THROW(script::run_script_text(R"({"canvas":{"w":16384,"h":1},"ops":[],"out":"png8"})"));
}

TEST(Script, OpLevelErrors) {
    expect_ops_error(R"({"op":"no_such_op"})", "unknown op");
    expect_ops_error(R"({"id":"x"})", "missing string field 'op'");
    expect_ops_error(R"({"op":"add_layer","id":"a","fill":"solid","from":"#000000"})", "unknown field 'from'");
    expect_ops_error(R"({"op":"add_layer","id":"a","fill":"checker","cell":8.0})", "JSON integer");
    expect_ops_error(R"({"op":"add_layer","id":"a","fill":"checker","cell":0})");
    expect_ops_error(R"({"op":"add_layer","id":"a","fill":"noise","alpha":"rand"})");
    expect_ops_error(R"({"op":"add_layer","id":"a","fill":"noise","seed":9007199254740992})");
    expect_ops_error(R"({"op":"add_layer","id":"a","rect":[0,0,0,4]})");
    expect_ops_error(R"({"op":"add_layer","id":"a","fill":"sold"})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"add_layer","id":"a"})", "duplicate id");
    expect_ops_error(R"({"op":"add_layer","id":"root"})", "reserved");
    expect_ops_error(R"({"op":"add_layer","id":""})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"add_layer","id":"b","parent":"a"})", "not a group");
    expect_ops_error(R"({"op":"add_layer","id":"b","parent":"nope"})", "unknown parent");
    expect_ops_error(R"({"op":"set_opacity","layer":"ghost","value":0.5})", "unknown id");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"set_opacity","layer":"a","value":-0.1})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"set_opacity","layer":"a","value":"0.5"})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"set_visible","layer":"a","value":1})");
    expect_ops_error(R"({"op":"add_group","id":"g"},{"op":"set_fill","layer":"g","value":0.5})");
    expect_ops_error(R"({"op":"add_group","id":"g"},{"op":"set_clip","layer":"g","value":true})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"set_blend","layer":"a","mode":"isolated"})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"set_blend","layer":"a","mode":"Norm"})");
    expect_ops_error(R"({"op":"add_group","id":"g","mode":"mul "})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"delete_mask","layer":"a"})", "no mask");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"set_mask_enabled","layer":"a","value":false})", "no mask");
    expect_ops_error(R"({"op":"add_group","id":"g"},{"op":"add_mask","layer":"g"},{"op":"apply_mask","layer":"g"})",
                     "not a raster layer");
    expect_ops_error(R"({"op":"add_group","id":"g"},{"op":"lock_transparency","layer":"g"})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"add_mask","layer":"a","value":256})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"add_mask","layer":"a","fill":"noise","value":3})",
                     "unknown field 'value'");
    expect_ops_error(R"({"op":"add_adjustment","id":"i","type":"invert","params":{"x":1}})", "unknown field 'x'");
    expect_ops_error(R"({"op":"add_adjustment","id":"i","type":"invert","params":[]})");
    expect_ops_error(R"({"op":"add_adjustment","id":"i","type":"curves","params":{"rgb":[[0,0]]}})", "2..16 points");
    expect_ops_error(R"({"op":"add_adjustment","id":"i","type":"sepia"})", "unknown adjustment type");
    expect_ops_error(R"({"op":"add_adjustment","id":"i","type":"invert","fill":"solid"})", "unknown field 'fill'");
    expect_ops_error(R"({"op":"undo","steps":0})");
    expect_ops_error(R"({"op":"undo","steps":1001})");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"merge_visible","id":"root"})", "reserved");
}

TEST(Script, MoveLayer) {
    expect_ops_error(R"({"op":"add_group","id":"g"},{"op":"add_group","id":"h","parent":"g"},
        {"op":"move_layer","id":"g","parent":"h"})", "descendant");
    expect_ops_error(R"({"op":"add_group","id":"g"},{"op":"move_layer","id":"g","parent":"g"})", "descendant");
    expect_ops_error(R"({"op":"add_layer","id":"a"},{"op":"add_layer","id":"b"},{"op":"move_layer","id":"a","index":2})",
                     "out of range");
    expect_ops_error(R"({"op":"move_layer","id":"root"})");
    auto res = script::run_script_text(make_script(8, 8, R"({"op":"add_layer","id":"a"},{"op":"add_layer","id":"b"},
        {"op":"add_layer","id":"c"},{"op":"move_layer","id":"c","index":0},{"op":"add_group","id":"g"},
        {"op":"move_layer","id":"a","parent":"g"})"));
    const auto& ch = res.doc->root().children;
    ASSERT_EQ(ch.size(), 3u);
    EXPECT_EQ(ch[0].id, "c");
    EXPECT_EQ(ch[1].id, "b");
    EXPECT_EQ(ch[2].id, "g");
    ASSERT_EQ(ch[2].children.size(), 1u);
    EXPECT_EQ(ch[2].children[0].id, "a");
}

TEST(Script, PropertiesAndDefaults) {
    auto res = script::run_script_text(make_script(8, 8, R"({"op":"add_layer","id":"a"},
        {"op":"set_blend","layer":"a","mode":"diss","seed":9007199254740991},
        {"op":"set_opacity","layer":"a","value":0.1},
        {"op":"set_fill","layer":"a","value":1},
        {"op":"lock_transparency","layer":"a"},
        {"op":"set_clbl","layer":"a","value":false},
        {"op":"set_blend","layer":"a","mode":"lum"},
        {"op":"add_group","id":"g"},
        {"op":"set_blend","layer":"g","mode":"isolated"},
        {"op":"add_group","id":"p"})"));
    Document& d = *res.doc;
    const Node& a = *d.find("a").node;
    EXPECT_EQ(a.mode, BlendMode::Lum);
    EXPECT_EQ(a.seed, 9007199254740991ULL);  // seed unchanged by a set_blend without seed
    EXPECT_EQ(a.opacity, 0.1);               // nearest binary64 of the JSON text
    EXPECT_EQ(a.fill, 1.0);
    EXPECT_TRUE(a.lock_alpha);
    EXPECT_FALSE(a.clbl);
    EXPECT_EQ(d.find("g").node->mode, BlendMode::Norm);
    EXPECT_EQ(d.find("p").node->mode, BlendMode::Pass);
    EXPECT_EQ(d.history_size(), 10u);
}

TEST(Script, CentralListRegistersEveryDomain) {
    const auto& reg = script::default_registry();
    for (const char* op : {"add_layer", "add_group", "move_layer", "set_blend", "set_opacity", "set_fill",
                           "set_visible", "set_clip", "set_clbl", "add_mask", "set_mask_enabled", "delete_mask",
                           "apply_mask", "lock_transparency", "merge_down", "merge_visible", "flatten", "undo",
                           "add_adjustment"})
        EXPECT_NE(reg.find(op), nullptr) << op;
    EXPECT_FALSE(reg.find("undo")->records_history);
    EXPECT_TRUE(reg.find("add_layer")->records_history);
    script::OpRegistry r;
    script::registerAllOps(r);
    EXPECT_THROW(r.add("add_layer", [](script::OpContext&, script::Fields&) {}), std::logic_error);
}
