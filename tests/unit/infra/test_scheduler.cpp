// SPDX-License-Identifier: GPL-3.0-or-later
//
// ICompositeScheduler: row-major delivery, de-duplication, the render API going through the
// installed scheduler, and --deterministic pinning the single-threaded path.
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <vector>

#include "core/composite/render.hpp"
#include "core/script/engine.hpp"

using namespace rl;

namespace {

// A scheduler that computes tiles in REVERSE order but still delivers them row-major - what a
// threaded 0.2 implementation must look like from the outside.
class ReverseScheduler final : public composite::ICompositeScheduler {
public:
    explicit ReverseScheduler(std::atomic<int>* calls) : calls_(calls) {}
    const char* name() const override { return "reverse-test"; }
    int concurrency() const override { return 1; }
    void render(const DocState& s, const std::vector<TileKey>& keys, bool onto_bg,
                const composite::TileSink& sink) override {
        calls_->fetch_add(1);
        std::vector<RgbaTile> out(keys.size());
        for (size_t i = keys.size(); i-- > 0;) composite::render_tile_kernel(s, keys[i].tx, keys[i].ty, onto_bg, out[i]);
        for (size_t i = 0; i < keys.size(); ++i) sink(keys[i].tx, keys[i].ty, out[i]);
    }

private:
    std::atomic<int>* calls_;
};

std::unique_ptr<Document> sample_doc() {
    return script::run_script_text(R"({"canvas":{"w":300,"h":200,"bg":"#102030ff"},"ops":[
        {"op":"add_layer","id":"a","fill":"gradient","from":"#ff0000ff","to":"#0000ff80","dir":"h"},
        {"op":"add_layer","id":"b","fill":"solid","color":"#00ff0080","rect":[40,30,150,100]},
        {"op":"set_blend","layer":"b","mode":"mul"}],"out":"png8"})")
        .doc;
}

}  // namespace

TEST(Scheduler, DefaultIsDeterministicRowMajor) {
    EXPECT_STREQ(composite::scheduler().name(), "deterministic");
    EXPECT_EQ(composite::scheduler().concurrency(), 1);
    auto doc = sample_doc();
    std::vector<TileKey> order;
    composite::render_tiles(doc->state(), {{4, 3}, {0, 0}, {2, 1}, {0, 0}, {9, 9}, {-1, 0}, {1, 1}}, true,
                            [&](int tx, int ty, const RgbaTile&) { order.push_back({tx, ty}); });
    // Sorted row-major, duplicates and off-canvas keys (300x200 = 5x4 tiles) dropped.
    const std::vector<TileKey> want = {{0, 0}, {1, 1}, {2, 1}, {4, 3}};
    ASSERT_EQ(order.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) EXPECT_TRUE(order[i] == want[i]) << i;
}

TEST(Scheduler, BatchEqualsSingleTileRenders) {
    auto doc = sample_doc();
    std::vector<TileKey> all;
    for (int ty = 0; ty < 4; ++ty)
        for (int tx = 0; tx < 5; ++tx) all.push_back({tx, ty});
    composite::render_tiles(doc->state(), all, true, [&](int tx, int ty, const RgbaTile& t) {
        RgbaTile one;
        composite::render_tile(doc->state(), tx, ty, true, one);
        EXPECT_EQ(0, std::memcmp(one.px.data(), t.px.data(), sizeof(one.px))) << tx << "," << ty;
    });
}

TEST(Scheduler, InstalledSchedulerIsUsedAndOutputIdentical) {
    auto doc = sample_doc();
    const RgbaImage ref = composite::render_image(doc->state(), true);
    std::atomic<int> calls{0};
    composite::set_scheduler(std::make_unique<ReverseScheduler>(&calls));
    EXPECT_STREQ(composite::scheduler().name(), "reverse-test");
    const RgbaImage got = composite::render_image(doc->state(), true);
    RgbaTile t;
    composite::render_tile(doc->state(), 2, 2, true, t);  // the GUI's dirty-tile path
    composite::set_scheduler(nullptr);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_TRUE(ref.pixels_equal(got));
    EXPECT_STREQ(composite::scheduler().name(), "deterministic");
}

// Last in this file: force_deterministic() is process-wide and permanent by design.
TEST(Scheduler, ForceDeterministicPinsTheSingleThreadedPath) {
    std::atomic<int> calls{0};
    composite::force_deterministic();
    EXPECT_TRUE(composite::deterministic_forced());
    composite::set_scheduler(std::make_unique<ReverseScheduler>(&calls));  // ignored
    EXPECT_STREQ(composite::scheduler().name(), "deterministic");
    auto doc = sample_doc();
    (void)composite::render_image(doc->state(), true);
    EXPECT_EQ(calls.load(), 0);
}
