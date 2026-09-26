// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/io/png.hpp"
#include "test_util.hpp"

using namespace rl;

namespace {
io::RgbaBuffer pattern(int w, int h) {
    io::RgbaBuffer b;
    b.w = w;
    b.h = h;
    b.px.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            b.at(x, y) = Rgba8{uint8_t(x * 3), uint8_t(y * 5), uint8_t(x ^ y), uint8_t((x + y) & 255)};
    return b;
}
std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST(Png, RoundTripIsByteExactIncludingAlphaZeroColour) {
    const auto img = pattern(70, 45);  // alpha 0 pixels keep their colour: the codec never alters bytes
    const auto bytes = io::encode_png(img);
    const auto back = io::decode_png(bytes);
    EXPECT_TRUE(rltest::same_pixels(img, back));
}

TEST(Png, EncodingIsDeterministicAndHasNoColourChunks) {
    const auto img = pattern(33, 17);
    const auto a = io::encode_png(img);
    const auto b = io::encode_png(img);
    EXPECT_EQ(a, b);
    // Walk the chunks: only IHDR, IDAT, IEND.
    size_t i = 8;
    while (i + 8 <= a.size()) {
        const uint32_t len = (uint32_t(a[i]) << 24) | (uint32_t(a[i + 1]) << 16) | (uint32_t(a[i + 2]) << 8) | a[i + 3];
        const std::string type(a.begin() + static_cast<long>(i) + 4, a.begin() + static_cast<long>(i) + 8);
        EXPECT_TRUE(type == "IHDR" || type == "IDAT" || type == "IEND") << type;
        i += 12 + len;
    }
    EXPECT_EQ(i, a.size());
}

TEST(Png, FileWriteMatchesMemoryEncodeAndDocumentExport) {
    const std::string path = std::string(RL_TEST_OUT) + "/unit_png_io.png";
    auto res = script::run_script_file(std::string(RL_SMOKE_DIR) + "/groups_clip_masks.json");
    io::write_document_png(res.doc->state(), path);
    const auto rendered = io::render_document(res.doc->state());
    EXPECT_EQ(slurp(path), io::encode_png(rendered));
    EXPECT_TRUE(rltest::same_pixels(io::read_png(path), rendered));
}

TEST(Png, FailedWriteLeavesNoFile) {
    const std::string path = std::string(RL_TEST_OUT) + "/unit_png_fail.png";
    std::remove(path.c_str());
    std::vector<Rgba8> row(8);
    EXPECT_THROW(io::write_png_rows(path, 8, 8,
                                    [&](int y) -> const Rgba8* {
                                        if (y == 5) throw std::runtime_error("boom");
                                        return row.data();
                                    }),
                 std::runtime_error);
    EXPECT_TRUE(slurp(path).empty());
    EXPECT_TRUE(slurp(path + ".tmp").empty());
    EXPECT_THROW(io::decode_png({1, 2, 3}), std::runtime_error);
}
