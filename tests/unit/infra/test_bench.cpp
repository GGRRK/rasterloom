// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bench cases measure what docs/PERF.md says they measure.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <vector>

#include "core/brush/brush.hpp"
#include "core/script/engine.hpp"
#include "core/selftest/bench.hpp"

using namespace rl;

TEST(Bench, CasesAreDefined) {
    const auto cs = bench::cases();
    ASSERT_GE(cs.size(), 8u);
    for (const auto& c : cs) {
        EXPECT_FALSE(bench::case_setup(c.name).empty()) << c.name;
        EXPECT_FALSE(bench::case_op(c.name).empty()) << c.name;
    }
    EXPECT_TRUE(bench::case_op("no-such-case").empty());
}

TEST(Bench, BrushCaseIsExactly1000DabsOfSize200) {
    const std::string name = "brush_1000dabs_size200";
    script::ScriptResult sr = script::run_script_text(bench::case_setup(name));
    const nlohmann::json op = nlohmann::json::parse(bench::case_op(name));
    ASSERT_EQ(op.at("op"), "brush_stroke");
    brush::StrokeParams p;
    p.size = op.at("size").get<double>();
    p.hardness = op.at("hardness").get<double>();
    EXPECT_EQ(p.size, 200.0);
    EXPECT_EQ(p.spacing, 0.25);  // default spacing: 50 px
    std::vector<brush::Sample> samples;
    for (const auto& s : op.at("samples"))
        samples.push_back({s.at("x").get<double>(), s.at("y").get<double>(), s.at("pressure").get<double>(), 0.0, 0.0,
                           s.at("t_ms").get<double>()});
    brush::StrokeSession ss;
    brush::SessionOptions o;
    o.preview = false;
    o.push_history = false;
    ss.begin(*sr.doc, "p", p, o);
    ss.add_samples(samples);
    EXPECT_EQ(ss.dab_count(), 1000u);
}

TEST(Bench, SmallCaseRuns) {
    const bench::Result r = bench::run_case("image_size_4000x3000_to_2000x1500", 1);
    EXPECT_TRUE(r.error.empty()) << r.error;
    ASSERT_EQ(r.ms.size(), 1u);
    EXPECT_GT(r.ms[0], 0.0);
}
