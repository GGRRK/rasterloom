// SPDX-License-Identifier: GPL-3.0-or-later
//
// Written from the PKWARE .ZIP File Format Specification (APPNOTE.TXT 6.3.x), sections 4.3
// (local header, central directory, end of central directory) and 4.5.3 (Zip64 extra field).
#include "core/io/zip.hpp"

#include <zlib.h>

#include <limits>

#include "core/io/bytes.hpp"
#include "core/io/io_error.hpp"

namespace rl::io {

namespace {

constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kZip64EocdSig = 0x06064b50;
constexpr uint32_t kZip64LocatorSig = 0x07064b50;
constexpr uint16_t kDosTime = 0;       // 00:00:00
constexpr uint16_t kDosDate = 0x0021;  // 1980-01-01
constexpr int kDeflateLevel = 6;
constexpr uint64_t kMaxEntry = uint64_t{1} << 32;  // 4 GiB decompressed per entry

uint32_t crc_of(const std::vector<uint8_t>& d) {
    uLong c = crc32(0L, Z_NULL, 0);
    size_t off = 0;
    while (off < d.size()) {
        const size_t n = std::min<size_t>(d.size() - off, std::numeric_limits<uInt>::max());
        c = crc32(c, d.data() + off, static_cast<uInt>(n));
        off += n;
    }
    return static_cast<uint32_t>(c);
}

std::vector<uint8_t> deflate_raw(const std::vector<uint8_t>& in) {
    z_stream zs{};
    if (deflateInit2(&zs, kDeflateLevel, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw IoError("ZIP: deflateInit failed");
    std::vector<uint8_t> out(deflateBound(&zs, static_cast<uLong>(in.size())) + 16);
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    const size_t n = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) throw IoError("ZIP: deflate failed");
    out.resize(n);
    return out;
}

std::vector<uint8_t> inflate_raw(const uint8_t* p, uint64_t n, uint64_t usize, const std::string& name) {
    // One spare byte: zlib reports Z_BUF_ERROR for a zero-sized output buffer, and a stream that
    // is longer than declared must be detected rather than silently truncated.
    std::vector<uint8_t> out(static_cast<size_t>(usize) + 1);
    z_stream zs{};
    if (inflateInit2(&zs, -15) != Z_OK) throw IoError("ZIP: inflateInit failed");
    zs.next_in = const_cast<Bytef*>(p);
    zs.avail_in = static_cast<uInt>(n);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = inflate(&zs, Z_FINISH);
    const uint64_t got = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END || got != usize) throw IoError("ZIP: entry '" + name + "' is corrupt (inflate)");
    out.resize(static_cast<size_t>(usize));
    return out;
}

}  // namespace

void ZipWriter::add(const std::string& name, const std::vector<uint8_t>& data, Method m) {
    if (data.size() >= 0xFFFFFFFFull || out_.size() >= 0xFFFFFFFFull)
        throw IoError("ZIP: entry '" + name + "' too large (Zip64 is not written)");
    if (entries_.size() >= 0xFFFF) throw IoError("ZIP: too many entries");
    Entry e;
    e.name = name;
    e.crc = crc_of(data);
    e.usize = static_cast<uint32_t>(data.size());
    std::vector<uint8_t> packed;
    const std::vector<uint8_t>* payload = &data;
    if (m == Method::Deflate) {
        packed = deflate_raw(data);
        payload = &packed;
        e.method = 8;
    }
    e.csize = static_cast<uint32_t>(payload->size());
    e.offset = static_cast<uint32_t>(out_.size());

    bool ascii = true;
    for (unsigned char c : name) ascii = ascii && c < 0x80;
    ByteWriter w;
    w.le32(kLocalSig);
    w.le16(e.method == 8 ? 20 : 10);  // version needed to extract
    w.le16(ascii ? 0 : 0x0800);       // general purpose flags: bit 11 = UTF-8 name
    w.le16(e.method);
    w.le16(kDosTime);
    w.le16(kDosDate);
    w.le32(e.crc);
    w.le32(e.csize);
    w.le32(e.usize);
    w.le16(static_cast<uint16_t>(name.size()));
    w.le16(0);  // no extra field
    w.str(name);
    out_.insert(out_.end(), w.buf.begin(), w.buf.end());
    out_.insert(out_.end(), payload->begin(), payload->end());
    if (out_.size() >= 0xFFFFFFFFull) throw IoError("ZIP: archive too large (Zip64 is not written)");
    entries_.push_back(std::move(e));
}

std::vector<uint8_t> ZipWriter::finish() {
    const uint32_t cd_off = static_cast<uint32_t>(out_.size());
    ByteWriter w;
    for (const Entry& e : entries_) {
        bool ascii = true;
        for (unsigned char c : e.name) ascii = ascii && c < 0x80;
        w.le32(kCentralSig);
        w.le16(20);  // version made by: MS-DOS / FAT attribute compatibility, spec 2.0
        w.le16(e.method == 8 ? 20 : 10);
        w.le16(ascii ? 0 : 0x0800);
        w.le16(e.method);
        w.le16(kDosTime);
        w.le16(kDosDate);
        w.le32(e.crc);
        w.le32(e.csize);
        w.le32(e.usize);
        w.le16(static_cast<uint16_t>(e.name.size()));
        w.le16(0);  // extra
        w.le16(0);  // comment
        w.le16(0);  // disk number start
        w.le16(0);  // internal attributes
        w.le32(0);  // external attributes
        w.le32(e.offset);
        w.str(e.name);
    }
    const uint32_t cd_size = static_cast<uint32_t>(w.size());
    w.le32(kEocdSig);
    w.le16(0);
    w.le16(0);
    w.le16(static_cast<uint16_t>(entries_.size()));
    w.le16(static_cast<uint16_t>(entries_.size()));
    w.le32(cd_size);
    w.le32(cd_off);
    w.le16(0);  // comment length
    std::vector<uint8_t> out = std::move(out_);
    out.insert(out.end(), w.buf.begin(), w.buf.end());
    out_.clear();
    entries_.clear();
    return out;
}

ZipReader::ZipReader(const std::vector<uint8_t>& archive) : a_(archive) {
    const size_t n = a_.size();
    if (n < 22) throw IoError("ZIP: file too small");
    // End of central directory: the last signature within the final 64 KiB + 22 bytes.
    size_t eocd = std::string::npos;
    const size_t lo = n > 65557 ? n - 65557 : 0;
    for (size_t i = n - 22 + 1; i-- > lo;) {
        if (a_[i] == 0x50 && a_[i + 1] == 0x4b && a_[i + 2] == 0x05 && a_[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) throw IoError("ZIP: no end-of-central-directory record (not a ZIP file)");
    ByteReader r(a_, "ZIP");
    r.seek(eocd + 4);
    r.skip(4);  // disk numbers
    uint64_t count = r.le16();
    r.skip(2);
    uint64_t cd_size = r.le32();
    uint64_t cd_off = r.le32();
    if (count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF) {
        // Zip64 end of central directory locator sits right before the EOCD.
        if (eocd < 20) throw IoError("ZIP: bad Zip64 locator");
        r.seek(eocd - 20);
        if (r.le32() != kZip64LocatorSig) throw IoError("ZIP: missing Zip64 locator");
        r.skip(4);
        const uint64_t z64 = static_cast<uint64_t>(r.le32()) | (static_cast<uint64_t>(r.le32()) << 32);
        r.seek(z64);
        if (r.le32() != kZip64EocdSig) throw IoError("ZIP: bad Zip64 end record");
        r.skip(8 + 2 + 2 + 4 + 4 + 8);
        count = static_cast<uint64_t>(r.le32()) | (static_cast<uint64_t>(r.le32()) << 32);
        cd_size = static_cast<uint64_t>(r.le32()) | (static_cast<uint64_t>(r.le32()) << 32);
        cd_off = static_cast<uint64_t>(r.le32()) | (static_cast<uint64_t>(r.le32()) << 32);
    }
    if (cd_off > n || cd_size > n - cd_off) throw IoError("ZIP: central directory out of range");
    r.seek(cd_off);
    for (uint64_t i = 0; i < count; ++i) {
        if (r.le32() != kCentralSig) throw IoError("ZIP: corrupt central directory");
        r.skip(4);  // made by, needed
        const uint16_t flags = r.le16();
        Raw e;
        e.method = r.le16();
        r.skip(4);
        e.crc = r.le32();
        e.csize = r.le32();
        e.usize = r.le32();
        const uint16_t nlen = r.le16(), xlen = r.le16(), clen = r.le16();
        r.skip(8);
        e.local_offset = r.le32();
        std::string name = r.str(nlen);
        ByteReader x = r.sub(xlen, "ZIP extra");
        r.skip(clen);
        if (flags & 1) throw IoError("ZIP: encrypted entries are not supported ('" + name + "')");
        while (x.left() >= 4) {
            const uint16_t id = x.le16(), sz = x.le16();
            ByteReader f = x.sub(sz, "ZIP extra");
            if (id != 0x0001) continue;
            auto rd64 = [&f]() { return static_cast<uint64_t>(f.le32()) | (static_cast<uint64_t>(f.le32()) << 32); };
            if (e.usize == 0xFFFFFFFF) e.usize = rd64();
            if (e.csize == 0xFFFFFFFF) e.csize = rd64();
            if (e.local_offset == 0xFFFFFFFF) e.local_offset = rd64();
        }
        if (!entries_.count(name)) names_.push_back(name);
        entries_[name] = e;
    }
}

const ZipReader::Raw& ZipReader::raw(const std::string& name) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) throw IoError("ZIP: missing entry '" + name + "'");
    return it->second;
}

std::vector<uint8_t> ZipReader::read(const std::string& name) const {
    const Raw& e = raw(name);
    ByteReader r(a_, "ZIP");
    r.seek(e.local_offset);
    if (r.le32() != kLocalSig) throw IoError("ZIP: bad local header for '" + name + "'");
    r.skip(22);
    const uint16_t nlen = r.le16(), xlen = r.le16();
    r.skip(static_cast<uint64_t>(nlen) + xlen);
    r.need(e.csize);
    if (e.usize > kMaxEntry) throw IoError("ZIP: entry '" + name + "' is too large");
    std::vector<uint8_t> out;
    if (e.method == 0) {
        if (e.csize != e.usize) throw IoError("ZIP: stored entry '" + name + "' has inconsistent sizes");
        out.assign(r.here(), r.here() + e.csize);
    } else if (e.method == 8) {
        if (e.csize > std::numeric_limits<uInt>::max() || e.usize > std::numeric_limits<uInt>::max())
            throw IoError("ZIP: entry '" + name + "' is too large");
        out = inflate_raw(r.here(), e.csize, e.usize, name);
    } else {
        throw IoError("ZIP: entry '" + name + "' uses unsupported compression method " + std::to_string(e.method));
    }
    if (crc_of(out) != e.crc) throw IoError("ZIP: CRC mismatch in '" + name + "'");
    return out;
}

}  // namespace rl::io
