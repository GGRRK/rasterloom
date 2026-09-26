// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/edit/png_payload.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "core/geometry/dense.hpp"

namespace rl::edit {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool is_letter(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("png payload: " + why); }

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

// §14.2 step 6: inflate `z` to exactly `need` bytes (end of stream reached, Adler-32 checked by
// zlib); bytes after the end of the stream are ignored.
std::vector<uint8_t> inflate_exact(const std::vector<uint8_t>& z, size_t need) {
    std::vector<uint8_t> out(need);
    z_stream s{};
    if (inflateInit(&s) != Z_OK) bad("zlib initialisation failed");
    struct Guard {
        z_stream* s;
        ~Guard() { inflateEnd(s); }
    } guard{&s};
    // zlib counts in uInt; feed in bounded slices.
    constexpr size_t kSlice = size_t{1} << 30;
    size_t in_pos = 0, out_pos = 0;
    bool ended = false;
    while (!ended) {
        if (s.avail_in == 0 && in_pos < z.size()) {
            const size_t n = std::min(kSlice, z.size() - in_pos);
            s.next_in = const_cast<Bytef*>(z.data() + in_pos);
            s.avail_in = static_cast<uInt>(n);
            in_pos += n;
        }
        if (out_pos == need) {
            // Output is full: the stream must end without producing another byte.
            uint8_t extra = 0;
            s.next_out = &extra;
            s.avail_out = 1;
            const int rc = inflate(&s, Z_NO_FLUSH);
            if (s.avail_out == 0) bad("image data is longer than height * (1 + 4 * width)");
            if (rc == Z_STREAM_END) {
                ended = true;
                break;
            }
            if (rc == Z_BUF_ERROR && s.avail_in == 0 && in_pos >= z.size()) bad("zlib stream is truncated");
            if (rc != Z_OK && rc != Z_BUF_ERROR) bad("zlib stream is corrupt");
            continue;
        }
        const size_t room = std::min(kSlice, need - out_pos);
        s.next_out = out.data() + out_pos;
        s.avail_out = static_cast<uInt>(room);
        const int rc = inflate(&s, Z_NO_FLUSH);
        out_pos += room - s.avail_out;
        if (rc == Z_STREAM_END) {
            ended = true;
            break;
        }
        if (rc == Z_BUF_ERROR) {
            if (s.avail_in == 0 && in_pos >= z.size()) bad("zlib stream is truncated");
            continue;
        }
        if (rc != Z_OK) bad("zlib stream is corrupt");
    }
    if (out_pos != need) bad("image data is shorter than height * (1 + 4 * width)");
    return out;
}

}  // namespace

bool decode_base64(const std::string& s, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (s.size() % 4 != 0) {
        err = "base64 length is not a multiple of 4";
        return false;
    }
    const size_t groups = s.size() / 4;
    out.reserve(groups * 3);
    for (size_t g = 0; g < groups; ++g) {
        const char* c = s.data() + (g * 4);
        const bool last = g + 1 == groups;
        int pad = 0;
        if (last && c[3] == '=') pad = (c[2] == '=') ? 2 : 1;
        int v[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4 - pad; ++k) {
            v[k] = b64_value(c[k]);
            if (v[k] < 0) {
                err = c[k] == '=' ? "base64 '=' is only allowed as final padding" : "base64 contains a character outside A-Z a-z 0-9 + /";
                return false;
            }
        }
        const uint32_t w = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                           (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<uint8_t>(w >> 16));
        if (pad < 2) out.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<uint8_t>(w & 0xFF));
    }
    return true;
}

std::string encode_base64(const uint8_t* data, size_t n) {
    std::string s;
    s.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t w = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        s.push_back(kAlphabet[(w >> 18) & 63]);
        s.push_back(kAlphabet[(w >> 12) & 63]);
        s.push_back(kAlphabet[(w >> 6) & 63]);
        s.push_back(kAlphabet[w & 63]);
    }
    if (n - i == 1) {
        const uint32_t w = static_cast<uint32_t>(data[i]) << 16;
        s.push_back(kAlphabet[(w >> 18) & 63]);
        s.push_back(kAlphabet[(w >> 12) & 63]);
        s += "==";
    } else if (n - i == 2) {
        const uint32_t w = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        s.push_back(kAlphabet[(w >> 18) & 63]);
        s.push_back(kAlphabet[(w >> 12) & 63]);
        s.push_back(kAlphabet[(w >> 6) & 63]);
        s.push_back('=');
    }
    return s;
}

io::RgbaBuffer decode_png_payload(const std::vector<uint8_t>& b) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() < 8 || std::memcmp(b.data(), kSig, 8) != 0) bad("not a PNG (bad signature)");
    size_t pos = 8;
    bool seen_ihdr = false, seen_plte = false, seen_iend = false;
    int idat_state = 0;  // 0 none yet, 1 inside the IDAT run, 2 run finished
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> z;
    while (!seen_iend) {
        if (b.size() - pos < 8) bad("chunk header runs past the end (no IEND)");
        const uint32_t len = be32(&b[pos]);
        const uint8_t* type = &b[pos + 4];
        if (len > 0x7FFFFFFFu) bad("chunk length above 2^31 - 1");
        for (int k = 0; k < 4; ++k)
            if (!is_letter(type[k])) bad("chunk type is not four ASCII letters");
        if (b.size() - pos - 8 < static_cast<size_t>(len) + 4) bad("chunk runs past the end");
        const uint8_t* data = &b[pos + 8];
        const bool critical = type[0] >= 'A' && type[0] <= 'Z';
        const std::string t(reinterpret_cast<const char*>(type), 4);
        const bool is_idat = t == "IDAT";
        if (!is_idat && idat_state == 1) idat_state = 2;
        if (critical) {
            uLong crc = crc32(0L, Z_NULL, 0);
            crc = crc32(crc, type, 4);
            // len <= 2^31 - 1 fits in uInt on every supported platform.
            crc = crc32(crc, data, static_cast<uInt>(len));
            if (static_cast<uint32_t>(crc) != be32(data + len)) bad("CRC mismatch in critical chunk " + t);
            if (!seen_ihdr && t != "IHDR") bad("the first chunk is not IHDR");
            if (t == "IHDR") {
                if (seen_ihdr) bad("second IHDR");
                seen_ihdr = true;
                if (len != 13) bad("IHDR length is not 13");
                w = be32(data);
                h = be32(data + 4);
                if (data[8] != 8) bad("bit depth is not 8");
                if (data[9] != 6) bad("colour type is not 6 (RGBA)");
                if (data[10] != 0 || data[11] != 0) bad("unknown compression or filter method");
                if (data[12] != 0) bad("interlaced (Adam7) images are not accepted");
                if (w < 1 || h < 1 || w > 16384 || h > 16384) bad("width and height must be 1..16384");
            } else if (t == "PLTE") {
                if (seen_plte) bad("second PLTE");
                if (idat_state != 0) bad("PLTE after IDAT");
                seen_plte = true;
            } else if (is_idat) {
                if (idat_state == 2) bad("IDAT chunks are not consecutive");
                idat_state = 1;
                z.insert(z.end(), data, data + len);
            } else if (t == "IEND") {
                seen_iend = true;
            } else {
                bad("unknown critical chunk " + t);
            }
        } else if (!seen_ihdr) {
            bad("the first chunk is not IHDR");
        }
        pos += 12 + static_cast<size_t>(len);
    }
    if (idat_state == 0) bad("no IDAT chunk");

    const size_t stride = static_cast<size_t>(w) * 4;
    const size_t need = static_cast<size_t>(h) * (1 + stride);
    const mem::Reservation hold = geom::reserve_dense(static_cast<uint64_t>(need) * 2, "place_image payload");
    const std::vector<uint8_t> raw = inflate_exact(z, need);

    io::RgbaBuffer img;
    img.w = static_cast<int>(w);
    img.h = static_cast<int>(h);
    img.px.resize(static_cast<size_t>(w) * h);
    auto* recon = reinterpret_cast<uint8_t*>(img.px.data());
    static_assert(sizeof(Rgba8) == 4, "Rgba8 must be 4 packed bytes");
    for (size_t y = 0; y < h; ++y) {
        const uint8_t ft = raw[y * (1 + stride)];
        const uint8_t* filt = &raw[(y * (1 + stride)) + 1];
        uint8_t* cur = recon + (y * stride);
        const uint8_t* prev = y > 0 ? recon + ((y - 1) * stride) : nullptr;
        if (ft > 4) bad("filter type above 4");
        for (size_t k = 0; k < stride; ++k) {
            const int a = k >= 4 ? cur[k - 4] : 0;
            const int bb = prev ? prev[k] : 0;
            const int c = (prev && k >= 4) ? prev[k - 4] : 0;
            int pred = 0;
            switch (ft) {
                case 0: pred = 0; break;
                case 1: pred = a; break;
                case 2: pred = bb; break;
                case 3: pred = (a + bb) / 2; break;
                default: pred = paeth(a, bb, c); break;
            }
            cur[k] = static_cast<uint8_t>((filt[k] + pred) & 0xFF);
        }
    }
    return img;
}

io::RgbaBuffer decode_payload(const std::string& b64) {
    std::vector<uint8_t> bytes;
    std::string err;
    if (!decode_base64(b64, bytes, err)) throw std::runtime_error("png payload: " + err);
    return decode_png_payload(bytes);
}

std::string encode_payload(const io::RgbaBuffer& img) {
    const std::vector<uint8_t> png = io::encode_png(img);
    return encode_base64(png.data(), png.size());
}

}  // namespace rl::edit
