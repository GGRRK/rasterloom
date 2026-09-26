// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layer names read from files (docs/math/60-editing-ops.md §13 "Files"): the PSD/PSB and ORA/.orp
// readers sanitise a name that breaks §1.1 (U+FFFD for ill-formed input, C0 controls and DEL
// dropped, cut to 255 code points) and warn.
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "core/io/layer_name.hpp"
#include "core/io/ora.hpp"
#include "core/io/zip.hpp"
#include "core/psd/psd.hpp"
#include "io_test_util.hpp"

using namespace rl;
using rliotest::doc_from_ops;

namespace {

const std::string kFFFD = "\xEF\xBF\xBD";

std::string repeat(const std::string& s, int n) {
    std::string o;
    for (int i = 0; i < n; ++i) o += s;
    return o;
}

bool has_warning(const std::vector<std::string>& w, const std::string& needle) {
    for (const std::string& s : w)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

// Rewrites one entry of a zip (ORA) archive through `edit`.
std::vector<uint8_t> rezip(const std::vector<uint8_t>& bytes, const std::string& entry,
                           const std::function<void(std::string&)>& edit) {
    io::ZipReader z(bytes);
    io::ZipWriter zw;
    for (const std::string& n : z.names()) {
        auto d = z.read(n);
        if (n == entry) {
            std::string t(d.begin(), d.end());
            edit(t);
            d.assign(t.begin(), t.end());
        }
        zw.add(n, d, n == "mimetype" ? io::ZipWriter::Method::Store : io::ZipWriter::Method::Deflate);
    }
    return zw.finish();
}

DocState two_layers(const std::string& name_a, const std::string& name_b) {
    DocState s = doc_from_ops(8, 8, R"({"op":"add_layer","id":"a","fill":"solid","color":"#FF0000FF"},{"op":"add_layer","id":"b"})");
    s.root.children.at(0).name = name_a;
    s.root.children.at(1).name = name_b;
    return s;
}

}  // namespace

TEST(LayerNames, ValidNamesAreKeptExactly) {
    for (const std::string& ok : {std::string(""), std::string("  two  spaces "), std::string("caf\xC3\xA9"),
                                  std::string("\xF0\x9F\x98\x80 emoji"), std::string("C1 \xC2\x85 kept"),
                                  repeat("\xC3\xA9", 255)}) {
        const io::SanitizedName n = io::sanitize_layer_name(ok);
        EXPECT_EQ(n.name, ok);
        EXPECT_FALSE(n.changed()) << ok;
    }
}

TEST(LayerNames, ControlCodePointsAreDropped) {
    const io::SanitizedName n = io::sanitize_layer_name(std::string("a\x01" "b\tc\x7F\n\r", 8) + std::string(1, '\0') + "d");
    EXPECT_EQ(n.name, "abcd");
    EXPECT_TRUE(n.dropped);
    EXPECT_FALSE(n.replaced);
    EXPECT_FALSE(n.truncated);
}

TEST(LayerNames, IllFormedUtf8BecomesReplacementCharacters) {
    struct Case {
        std::string in, out;
    };
    const std::vector<Case> cases = {
        {"a\xFF" "b", "a" + kFFFD + "b"},                  // never-valid byte
        {"\xE2\x82" "x", kFFFD + "x"},                     // truncated 3-byte sequence: one maximal subpart
        {"\xF0\x9F\x98", kFFFD},                           // truncated 4-byte sequence at the end
        {"\xC0\xAF", kFFFD + kFFFD},                       // overlong: C0 and AF are each invalid
        {"\xE0\x80\x80", kFFFD + kFFFD + kFFFD},           // overlong 3-byte: E0 needs A0..BF
        {"\xED\xA0\x80" "z", kFFFD + "z"},                 // encoded lone high surrogate: one U+FFFD
        {"\xED\xB0\x80", kFFFD},                           // encoded lone low surrogate
        {"\xF4\x90\x80\x80", repeat(kFFFD, 4)},            // above U+10FFFF
        {"\x80\x80", kFFFD + kFFFD},                       // stray continuation bytes
    };
    for (const Case& c : cases) {
        const io::SanitizedName n = io::sanitize_layer_name(c.in);
        EXPECT_EQ(n.name, c.out) << testing::PrintToString(c.in);
        EXPECT_TRUE(n.replaced);
    }
}

TEST(LayerNames, Utf16LoneSurrogatesBecomeReplacementCharacters) {
    EXPECT_EQ(io::sanitize_layer_name_utf16({0x61, 0xD800, 0x62}).name, "a" + kFFFD + "b");
    EXPECT_EQ(io::sanitize_layer_name_utf16({0xDC00, 0xD800}).name, kFFFD + kFFFD);  // reversed pair
    const io::SanitizedName pair = io::sanitize_layer_name_utf16({0xD83D, 0xDE00});
    EXPECT_EQ(pair.name, "\xF0\x9F\x98\x80");
    EXPECT_FALSE(pair.changed());
    const io::SanitizedName ctl = io::sanitize_layer_name_utf16({0x41, 0x0007, 0x42});
    EXPECT_EQ(ctl.name, "AB");
    EXPECT_TRUE(ctl.dropped);
}

TEST(LayerNames, CutTo255CodePointsAfterDroppingControls) {
    const io::SanitizedName n = io::sanitize_layer_name(repeat("\xC3\xA9", 300));
    EXPECT_EQ(n.name, repeat("\xC3\xA9", 255));  // 255 code points, 510 bytes
    EXPECT_TRUE(n.truncated);
    // 255 visible code points plus controls: the controls go first, nothing is cut.
    const io::SanitizedName m = io::sanitize_layer_name(repeat("x\x01", 255));
    EXPECT_EQ(m.name, repeat("x", 255));
    EXPECT_TRUE(m.dropped);
    EXPECT_FALSE(m.truncated);
    // A replacement character counts as one code point.
    EXPECT_EQ(io::sanitize_layer_name(repeat("\xFF", 256)).name, repeat(kFFFD, 255));
}

TEST(LayerNames, WarningSaysWhatChanged) {
    const std::string w = io::sanitized_name_warning("PSD", io::sanitize_layer_name("a\x01\xFF"));
    EXPECT_EQ(w, "PSD: layer name 'a" + kFFFD +
                     "' was sanitised (invalid characters replaced with U+FFFD, control characters removed)");
}

TEST(LayerNames, JsonRepairReplacesOnlyIllFormedPieces) {
    bool changed = false;
    EXPECT_EQ(io::repair_json_text(R"({"n":"a\ud800b","p":"\ud83d\ude00"})", changed),
              R"({"n":"a\uFFFDb","p":"\ud83d\ude00"})");
    EXPECT_TRUE(changed);
    EXPECT_EQ(io::repair_json_text(R"({"n":"\\ud800","q":"\"\udc00"})", changed), R"({"n":"\\ud800","q":"\"\uFFFD"})");
    EXPECT_TRUE(changed);  // an escaped backslash is not an escape; \" does not end the string
    EXPECT_EQ(io::repair_json_text("{\"n\":\"x\xC3\"}", changed), "{\"n\":\"x" + kFFFD + "\"}");
    EXPECT_TRUE(changed);
    const std::string clean = R"({"n":"caf\u00e9 \u0001","m":[1,2]})";
    EXPECT_EQ(io::repair_json_text(clean, changed), clean);
    EXPECT_FALSE(changed);
}

TEST(LayerNames, PsdReaderSanitisesAndWarns) {
    // The writer turns the encoded surrogate into a lone UTF-16 unit in `luni` and keeps the control.
    const DocState s = two_layers("bad\x01name\xED\xA0\x80", repeat("n", 300));
    std::vector<std::string> w;
    const auto bytes = psd::write(s, {}, w);
    w.clear();
    const DocState r = psd::read(bytes, w);
    ASSERT_EQ(r.root.children.size(), 2u);
    EXPECT_EQ(r.root.children[0].name, "badname" + kFFFD);
    EXPECT_EQ(r.root.children[1].name, repeat("n", 255));
    EXPECT_TRUE(has_warning(w, "PSD: layer name 'badname" + kFFFD + "' was sanitised (invalid characters replaced "
                                "with U+FFFD, control characters removed)"));
    EXPECT_TRUE(has_warning(w, "(cut to 255 characters)"));
    // A clean file reads back without a name warning.
    std::vector<std::string> w2;
    psd::read(psd::write(two_layers("fine", ""), {}, w2), w2);
    EXPECT_FALSE(has_warning(w2, "layer name"));
}

TEST(LayerNames, OraStackXmlNamesAreSanitised) {
    std::vector<std::string> w;
    const auto bytes = ora::write(two_layers("AAA", "BBB"), w);
    // Another program rewrote stack.xml (hash mismatch -> the generic stack.xml path).
    const auto edited = rezip(bytes, "stack.xml", [](std::string& t) {
        t.replace(t.find("name=\"AAA\""), 10, "name=\"x&#1;y&#xD800;\xFFz\"");
        t.replace(t.find("name=\"BBB\""), 10, "name=\"" + repeat("b", 400) + "\"");
    });
    std::vector<std::string> w2;
    const DocState r = ora::read(edited, w2);
    ASSERT_EQ(r.root.children.size(), 2u);
    EXPECT_EQ(r.root.children[0].name, "xy" + kFFFD + kFFFD + "z");
    EXPECT_EQ(r.root.children[1].name, repeat("b", 255));
    EXPECT_TRUE(has_warning(w2, "ORA: layer name 'xy" + kFFFD + kFFFD + "z' was sanitised"));
    EXPECT_TRUE(has_warning(w2, "(cut to 255 characters)"));
}

TEST(LayerNames, OrpDocumentJsonNamesAreRepairedAndSanitised) {
    std::vector<std::string> w;
    const auto bytes = ora::write(two_layers("AAA", "BBB"), w);
    const auto edited = rezip(bytes, "document.json", [](std::string& t) {
        const size_t a = t.find("\"AAA\"");
        ASSERT_NE(a, std::string::npos);
        t.replace(a, 5, R"("o\u0001k\ud800")");
        const size_t b = t.find("\"BBB\"");
        ASSERT_NE(b, std::string::npos);
        t.replace(b, 5, "\"B\xC3\"");  // a truncated UTF-8 sequence
    });
    std::vector<std::string> w2;
    const DocState r = ora::read(edited, w2);  // a strict JSON parser would reject both
    ASSERT_EQ(r.root.children.size(), 2u);
    EXPECT_EQ(r.root.children[0].name, "ok" + kFFFD);
    EXPECT_EQ(r.root.children[1].name, "B" + kFFFD);
    EXPECT_TRUE(has_warning(w2, "document.json has invalid UTF-8 or lone UTF-16 surrogate escapes"));
    EXPECT_TRUE(has_warning(w2, "ORA: layer name 'ok" + kFFFD + "' was sanitised (control characters removed)"));
}
