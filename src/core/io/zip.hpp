// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal ZIP container for OpenRaster (.ora / .orp), written from the PKWARE APPNOTE 6.3.x.
//
// Writer: deterministic. Entries are written in the order added; every entry has DOS timestamp
// 1980-01-01 00:00, no extra fields, no data descriptors, no comments; DEFLATE uses zlib raw
// deflate at a fixed level. The first entry can be STORED so that `mimetype` sits at byte 30 with
// its content at byte 38, as shared-mime-info's magic requires. Zip64 is not written (entries and
// archives are limited to 4 GiB; exceeding it is an IoError).
//
// Reader: STORED and DEFLATE entries, Zip64 sizes/offsets, CRC-32 verified, sizes bounded.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rl::io {

class ZipWriter {
public:
    enum class Method { Store, Deflate };
    void add(const std::string& name, const std::vector<uint8_t>& data, Method m = Method::Deflate);
    // The finished archive bytes.
    std::vector<uint8_t> finish();

private:
    struct Entry {
        std::string name;
        uint16_t method = 0;
        uint32_t crc = 0;
        uint32_t csize = 0;
        uint32_t usize = 0;
        uint32_t offset = 0;
    };
    std::vector<uint8_t> out_;
    std::vector<Entry> entries_;
};

class ZipReader {
public:
    // Parses the central directory. Throws IoError on a malformed archive.
    // The archive must outlive the reader (it is referenced, not copied).
    explicit ZipReader(const std::vector<uint8_t>& archive);
    explicit ZipReader(std::vector<uint8_t>&&) = delete;

    // Entry names in central-directory order.
    const std::vector<std::string>& names() const { return names_; }
    bool has(const std::string& name) const { return entries_.count(name) != 0; }
    // Decompressed contents (CRC-checked). Throws IoError when absent or corrupt.
    std::vector<uint8_t> read(const std::string& name) const;
    // The first entry's name, its method and the offset of its data (for format sniffing tests).
    struct Raw {
        uint16_t method = 0;
        uint64_t csize = 0;
        uint64_t usize = 0;
        uint64_t local_offset = 0;
        uint32_t crc = 0;
    };
    const Raw& raw(const std::string& name) const;

private:
    const std::vector<uint8_t>& a_;
    std::vector<std::string> names_;
    std::map<std::string, Raw> entries_;
};

}  // namespace rl::io
