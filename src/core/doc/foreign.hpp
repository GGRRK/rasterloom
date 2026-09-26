// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opaque "foreign data": bytes a file format carries that the document model does not interpret
// (PSD tagged blocks such as text, smart objects, layer styles and vector masks; PSD image
// resources and global blocks; codec-private bookkeeping). The file I/O layer stores them on read
// and writes them back unchanged on save, so a document opened from a file keeps everything the
// editor cannot represent (BUILD-SPEC addendum: "raw byte-preserving pass-through").
//
// Model code never looks inside. Copying a node or a DocState (history snapshots) copies only
// shared_ptr handles: the byte payloads are immutable and shared.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rl {

using ForeignBytes = std::shared_ptr<const std::vector<uint8_t>>;

struct ForeignBlock {
    // Namespace of the producer, e.g. "psd.tb" (a layer tagged block), "psd.gtb" (a global tagged
    // block), "psd.irb" (an image resource), "psd.own" (PSD codec bookkeeping that is not a block
    // of its own). The namespace decides who may interpret the payload; nobody else does.
    std::string ns;
    // Block key within the namespace (a 4CC, a resource id, ...). Keys may repeat.
    std::string key;
    ForeignBytes data;

    size_t size() const { return data ? data->size() : 0; }
    const std::vector<uint8_t>& bytes() const {
        static const std::vector<uint8_t> kEmpty;
        return data ? *data : kEmpty;
    }
};

struct ForeignData {
    // In the order the producer read them (file order for PSD).
    std::vector<ForeignBlock> blocks;

    bool empty() const { return blocks.empty(); }

    void add(std::string ns, std::string key, std::vector<uint8_t> bytes) {
        blocks.push_back(ForeignBlock{std::move(ns), std::move(key),
                                      std::make_shared<const std::vector<uint8_t>>(std::move(bytes))});
    }
    // First block with this namespace and key, or nullptr.
    const ForeignBlock* find(const std::string& ns, const std::string& key) const {
        for (const ForeignBlock& b : blocks)
            if (b.ns == ns && b.key == key) return &b;
        return nullptr;
    }
    // Removes every block of a namespace.
    void remove_ns(const std::string& ns) {
        std::vector<ForeignBlock> kept;
        for (ForeignBlock& b : blocks)
            if (b.ns != ns) kept.push_back(std::move(b));
        blocks = std::move(kept);
    }
};

}  // namespace rl
