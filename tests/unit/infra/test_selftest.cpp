// SPDX-License-Identifier: GPL-3.0-or-later
//
// The embedded self-test library: the corpus is present and complete, the unmutated run passes,
// and representative mutations (including 13, visible only through tile statistics) are caught.
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/base/mutation.hpp"
#include "core/selftest/embedded.hpp"
#include "core/selftest/selftest.hpp"

using namespace rl;

TEST(Selftest, CorpusIsEmbedded) {
    EXPECT_NE(selftest::find_embedded("manifest.json"), nullptr);
    EXPECT_GE(selftest::case_count(), 260u);  // BUILD-SPEC: 260 goldens minimum
    EXPECT_NE(selftest::find_embedded("compositing/blend_norm_opq.json"), nullptr);
    EXPECT_EQ(selftest::find_embedded("no/such/file.png"), nullptr);
}

TEST(Selftest, UnmutatedRunPasses) {
    selftest::Options o;
    const selftest::Report r = selftest::run(o);
    EXPECT_EQ(r.failed, 0);
    EXPECT_EQ(r.cases.size(), selftest::case_count());
    for (const auto& c : r.cases)
        if (!c.passed) ADD_FAILURE() << c.name << ": " << c.message;
}

TEST(Selftest, FilterSelectsAndKeepsEqualToTargets) {
    selftest::Options o;
    o.filter = "brush/B07";  // equal_to B05
    const selftest::Report r = selftest::run(o);
    EXPECT_EQ(r.failed, 0);
    ASSERT_EQ(r.cases.size(), 2u);
    EXPECT_EQ(r.cases[0].name, "brush/B05");
    EXPECT_EQ(r.cases[1].name, "brush/B07");
}

TEST(Selftest, MutationsAreCaughtAndRestored) {
    for (int id : {0, 7, 12, 13, 14, 39}) {
        selftest::Options o;
        o.mutations = {id};
        o.fail_fast = true;
        const selftest::Report r = selftest::run(o);
        EXPECT_GT(r.failed, 0) << "mutation " << id << " not caught";
        EXPECT_TRUE(mut::active_list().empty()) << "mutation " << id << " leaked";
    }
}

TEST(Selftest, ExpectErrorCasesMustFail) {
    selftest::Options o;
    o.filter = "*/err_*";
    const selftest::Report r = selftest::run(o);
    ASSERT_GT(r.cases.size(), 10u);
    for (const auto& c : r.cases) EXPECT_EQ(c.kind, "error") << c.name;
    EXPECT_EQ(r.failed, 0);
}

TEST(Selftest, JunitXml) {
    selftest::Options o;
    o.filter = "stats/*";
    selftest::Report r = selftest::run(o);
    r.cases.push_back({"x/fake", "render", false, "a <bad> & \"quoted\" message", 1.0});
    r.failed += 1;
    const std::string path = std::string(RL_TEST_OUT) + "/infra_junit.xml";
    ASSERT_TRUE(selftest::write_junit(r, path, "unit"));
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string x = ss.str();
    EXPECT_NE(x.find("<testsuite name=\"unit\" tests=\"" + std::to_string(r.cases.size()) + "\" failures=\"1\""),
              std::string::npos);
    EXPECT_NE(x.find("classname=\"selftest.stats\""), std::string::npos);
    EXPECT_NE(x.find("a &lt;bad&gt; &amp; &quot;quoted&quot; message"), std::string::npos);
}
