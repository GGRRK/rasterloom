// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bounds-checked big/little-endian byte readers and writers for the binary codecs (PSD, ZIP).
// Every read past the end throws IoError, so a truncated or hostile file can never read out of
// bounds.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/io/io_error.hpp"

namespace rl::io {

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size, std::string what = "file")
        : d_(data), n_(size), what_(std::move(what)) {}
    explicit ByteReader(const std::vector<uint8_t>& v, std::string what = "file")
        : ByteReader(v.data(), v.size(), std::move(what)) {}

    size_t pos() const { return p_; }
    size_t size() const { return n_; }
    size_t left() const { return n_ - p_; }
    const uint8_t* data() const { return d_; }
    const uint8_t* here() const { return d_ + p_; }

    void need(uint64_t k) const {
        if (k > left()) throw IoError(what_ + ": unexpected end of data (truncated or corrupt file)");
    }
    void seek(uint64_t p) {
        if (p > n_) throw IoError(what_ + ": offset out of range (corrupt file)");
        p_ = static_cast<size_t>(p);
    }
    void skip(uint64_t k) {
        need(k);
        p_ += static_cast<size_t>(k);
    }

    uint8_t u8() {
        need(1);
        return d_[p_++];
    }
    int8_t i8() { return static_cast<int8_t>(u8()); }
    uint16_t be16() {
        need(2);
        const uint16_t v = static_cast<uint16_t>((d_[p_] << 8) | d_[p_ + 1]);
        p_ += 2;
        return v;
    }
    int16_t bei16() { return static_cast<int16_t>(be16()); }
    uint32_t be32() {
        need(4);
        const uint32_t v = (static_cast<uint32_t>(d_[p_]) << 24) | (static_cast<uint32_t>(d_[p_ + 1]) << 16) |
                           (static_cast<uint32_t>(d_[p_ + 2]) << 8) | static_cast<uint32_t>(d_[p_ + 3]);
        p_ += 4;
        return v;
    }
    int32_t bei32() { return static_cast<int32_t>(be32()); }
    uint64_t be64() {
        const uint64_t hi = be32();
        return (hi << 32) | be32();
    }
    double bedouble() {
        const uint64_t b = be64();
        double v;
        std::memcpy(&v, &b, 8);
        return v;
    }
    uint16_t le16() {
        need(2);
        const uint16_t v = static_cast<uint16_t>(d_[p_] | (d_[p_ + 1] << 8));
        p_ += 2;
        return v;
    }
    uint32_t le32() {
        need(4);
        const uint32_t v = static_cast<uint32_t>(d_[p_]) | (static_cast<uint32_t>(d_[p_ + 1]) << 8) |
                           (static_cast<uint32_t>(d_[p_ + 2]) << 16) | (static_cast<uint32_t>(d_[p_ + 3]) << 24);
        p_ += 4;
        return v;
    }
    std::string str(size_t k) {
        need(k);
        std::string s(reinterpret_cast<const char*>(d_ + p_), k);
        p_ += k;
        return s;
    }
    std::vector<uint8_t> bytes(uint64_t k) {
        need(k);
        std::vector<uint8_t> v(d_ + p_, d_ + p_ + k);
        p_ += static_cast<size_t>(k);
        return v;
    }
    // A sub-reader over the next k bytes (advances this reader past them).
    ByteReader sub(uint64_t k, const std::string& what) {
        need(k);
        ByteReader r(d_ + p_, static_cast<size_t>(k), what);
        p_ += static_cast<size_t>(k);
        return r;
    }
    const std::string& what() const { return what_; }

private:
    const uint8_t* d_;
    size_t n_;
    size_t p_ = 0;
    std::string what_;
};

class ByteWriter {
public:
    std::vector<uint8_t> buf;

    size_t size() const { return buf.size(); }
    void u8(uint8_t v) { buf.push_back(v); }
    void i8(int8_t v) { buf.push_back(static_cast<uint8_t>(v)); }
    void be16(uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v));
    }
    void bei16(int16_t v) { be16(static_cast<uint16_t>(v)); }
    void be32(uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) buf.push_back(static_cast<uint8_t>(v >> s));
    }
    void bei32(int32_t v) { be32(static_cast<uint32_t>(v)); }
    void be64(uint64_t v) {
        be32(static_cast<uint32_t>(v >> 32));
        be32(static_cast<uint32_t>(v));
    }
    void bedouble(double d) {
        uint64_t b;
        std::memcpy(&b, &d, 8);
        be64(b);
    }
    void le16(uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
    }
    void le32(uint32_t v) {
        for (int s = 0; s <= 24; s += 8) buf.push_back(static_cast<uint8_t>(v >> s));
    }
    void raw(const void* p, size_t k) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + k);
    }
    void raw(const std::vector<uint8_t>& v) { buf.insert(buf.end(), v.begin(), v.end()); }
    void str(const std::string& s) { raw(s.data(), s.size()); }
    void zeros(size_t k) { buf.insert(buf.end(), k, 0); }
    void pad_to(size_t multiple) {
        while (buf.size() % multiple) buf.push_back(0);
    }

    // Placeholder for a length field patched later (4 or 8 bytes, big-endian).
    size_t reserve_len(int width) {
        const size_t at = buf.size();
        zeros(static_cast<size_t>(width));
        return at;
    }
    void patch_len(size_t at, int width, uint64_t v) {
        for (int i = 0; i < width; ++i)
            buf[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * (width - 1 - i)));
    }
    void patch_be32(size_t at, uint32_t v) { patch_len(at, 4, v); }
};

}  // namespace rl::io
