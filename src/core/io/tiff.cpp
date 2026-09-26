// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/tiff.hpp"

#include <tiffio.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/base/quant.hpp"
#include "core/io/io_error.hpp"

namespace rl::io {

namespace {

// In-memory stream for TIFFClientOpen.
struct MemStream {
    std::vector<uint8_t> buf;
    toff_t pos = 0;
};

tsize_t mem_read(thandle_t h, tdata_t p, tsize_t n) {
    auto* m = static_cast<MemStream*>(h);
    if (m->pos >= m->buf.size()) return 0;
    const size_t k = std::min<size_t>(static_cast<size_t>(n), m->buf.size() - static_cast<size_t>(m->pos));
    std::memcpy(p, m->buf.data() + m->pos, k);
    m->pos += k;
    return static_cast<tsize_t>(k);
}
tsize_t mem_write(thandle_t h, tdata_t p, tsize_t n) {
    auto* m = static_cast<MemStream*>(h);
    const size_t end = static_cast<size_t>(m->pos) + static_cast<size_t>(n);
    if (end > m->buf.size()) m->buf.resize(end);
    std::memcpy(m->buf.data() + m->pos, p, static_cast<size_t>(n));
    m->pos = end;
    return n;
}
toff_t mem_seek(thandle_t h, toff_t off, int whence) {
    auto* m = static_cast<MemStream*>(h);
    toff_t base = 0;
    if (whence == SEEK_CUR) base = m->pos;
    if (whence == SEEK_END) base = m->buf.size();
    m->pos = base + off;
    return m->pos;
}
int mem_close(thandle_t) { return 0; }
toff_t mem_size(thandle_t h) { return static_cast<MemStream*>(h)->buf.size(); }
int mem_map(thandle_t, tdata_t*, toff_t*) { return 0; }
void mem_unmap(thandle_t, tdata_t, toff_t) {}

thread_local std::string g_tiff_error;

void on_error(const char* module, const char* fmt, va_list ap) {
    char buf[512];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    g_tiff_error = std::string(module ? module : "libtiff") + ": " + buf;
}
void on_warning(const char*, const char*, va_list) {}

struct Handlers {
    TIFFErrorHandler e, w;
    Handlers() : e(TIFFSetErrorHandler(on_error)), w(TIFFSetWarningHandler(on_warning)) { g_tiff_error.clear(); }
    ~Handlers() {
        TIFFSetErrorHandler(e);
        TIFFSetWarningHandler(w);
    }
};

TIFF* open_mem(MemStream& m, const char* mode) {
    return TIFFClientOpen("memory", mode, &m, mem_read, mem_write, mem_seek, mem_close, mem_size, mem_map, mem_unmap);
}

}  // namespace

bool is_tiff(const std::vector<uint8_t>& b) {
    return b.size() >= 4 && ((b[0] == 'I' && b[1] == 'I' && (b[2] == 42 || b[2] == 43) && b[3] == 0) ||
                             (b[0] == 'M' && b[1] == 'M' && b[2] == 0 && (b[3] == 42 || b[3] == 43)));
}

std::vector<uint8_t> encode_tiff(const RgbaBuffer& img) {
    if (img.w <= 0 || img.h <= 0) throw IoError("TIFF: empty image");
    Handlers hs;
    bool opaque = true;
    for (const Rgba8& p : img.px) opaque = opaque && p.a == 255;
    const int spp = opaque ? 3 : 4;
    MemStream m;
    TIFF* t = open_mem(m, "w");
    if (!t) throw IoError("TIFF encode: " + g_tiff_error);
    constexpr uint32_t kRowsPerStrip = 64;
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(img.w));
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(img.h));
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, spp);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(t, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, kRowsPerStrip);
    if (!opaque) {
        uint16_t extra = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(t, TIFFTAG_EXTRASAMPLES, 1, &extra);
    }
    std::vector<uint8_t> row(static_cast<size_t>(img.w) * static_cast<size_t>(spp));
    bool ok = true;
    for (int y = 0; y < img.h && ok; ++y) {
        for (int x = 0; x < img.w; ++x) {
            const Rgba8 p = img.at(x, y);
            uint8_t* o = row.data() + (static_cast<size_t>(x) * static_cast<size_t>(spp));
            o[0] = p.r;
            o[1] = p.g;
            o[2] = p.b;
            if (spp == 4) o[3] = p.a;
        }
        ok = TIFFWriteScanline(t, row.data(), static_cast<uint32_t>(y), 0) >= 0;
    }
    ok = ok && TIFFWriteDirectory(t);
    TIFFClose(t);
    if (!ok) throw IoError("TIFF encode: " + g_tiff_error);
    return m.buf;
}

RgbaBuffer decode_tiff(const std::vector<uint8_t>& bytes) {
    if (!is_tiff(bytes)) throw IoError("not a TIFF file");
    Handlers hs;
    MemStream m;
    m.buf = bytes;
    TIFF* t = open_mem(m, "r");
    if (!t) throw IoError("TIFF decode: " + g_tiff_error);
    uint32_t w = 0, h = 0;
    uint16_t bps = 1, spp = 1, photo = PHOTOMETRIC_MINISBLACK, planar = PLANARCONFIG_CONTIG, fmt = SAMPLEFORMAT_UINT;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &fmt);
    TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &photo);
    uint16_t nextra = 0;
    uint16_t* extra = nullptr;
    TIFFGetFieldDefaulted(t, TIFFTAG_EXTRASAMPLES, &nextra, &extra);
    uint16_t orient = ORIENTATION_TOPLEFT;
    TIFFGetFieldDefaulted(t, TIFFTAG_ORIENTATION, &orient);
    if (w == 0 || h == 0 || static_cast<uint64_t>(w) * h > (uint64_t{1} << 30)) {
        TIFFClose(t);
        throw IoError("TIFF: bad image size");
    }
    RgbaBuffer out;
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.px.assign(static_cast<size_t>(w) * h, Rgba8{0, 0, 0, 255});

    const int colour = (photo == PHOTOMETRIC_RGB) ? 3 : 1;
    const bool direct = bps == 8 && fmt == SAMPLEFORMAT_UINT && !TIFFIsTiled(t) && orient == ORIENTATION_TOPLEFT &&
                        (photo == PHOTOMETRIC_RGB || photo == PHOTOMETRIC_MINISBLACK || photo == PHOTOMETRIC_MINISWHITE) &&
                        spp >= colour && spp <= colour + 1 + 4;
    if (direct) {
        const bool has_alpha = spp > colour && nextra > 0 &&
                               (extra[0] == EXTRASAMPLE_UNASSALPHA || extra[0] == EXTRASAMPLE_ASSOCALPHA);
        const bool assoc = has_alpha && extra[0] == EXTRASAMPLE_ASSOCALPHA;
        const tmsize_t sl = TIFFScanlineSize(t);
        std::vector<uint8_t> row(static_cast<size_t>(sl));
        bool ok = true;
        const int passes = planar == PLANARCONFIG_SEPARATE ? spp : 1;
        for (int s = 0; s < passes && ok; ++s) {
            for (uint32_t y = 0; y < h && ok; ++y) {
                if (TIFFReadScanline(t, row.data(), y, static_cast<uint16_t>(s)) < 0) {
                    ok = false;
                    break;
                }
                for (uint32_t x = 0; x < w; ++x) {
                    Rgba8& p = out.px[(static_cast<size_t>(y) * w) + x];
                    auto put = [&](int sample, uint8_t v) {
                        if (sample < colour) {
                            if (colour == 1) {
                                const uint8_t g = photo == PHOTOMETRIC_MINISWHITE ? static_cast<uint8_t>(255 - v) : v;
                                p.r = p.g = p.b = g;
                            } else if (sample == 0) {
                                p.r = v;
                            } else if (sample == 1) {
                                p.g = v;
                            } else {
                                p.b = v;
                            }
                        } else if (sample == colour && has_alpha) {
                            p.a = v;
                        }
                    };
                    if (planar == PLANARCONFIG_SEPARATE) {
                        put(s, row[x]);
                    } else {
                        for (int k = 0; k < spp; ++k) put(k, row[(static_cast<size_t>(x) * spp) + static_cast<size_t>(k)]);
                    }
                }
            }
        }
        TIFFClose(t);
        if (!ok) throw IoError("TIFF decode: " + g_tiff_error);
        for (Rgba8& p : out.px) {
            if (assoc && p.a > 0 && p.a < 255) {
                const double a = dec(p.a);
                p.r = q(dec(p.r) / a);
                p.g = q(dec(p.g) / a);
                p.b = q(dec(p.b) / a);
            }
        }
        return out;
    }
    // Everything else: libtiff's RGBA interface (premultiplied ABGR, top-left origin requested).
    std::vector<uint32_t> raster(static_cast<size_t>(w) * h);
    const int ok = TIFFReadRGBAImageOriented(t, w, h, raster.data(), ORIENTATION_TOPLEFT, 0);
    TIFFClose(t);
    if (!ok) throw IoError("TIFF decode: " + g_tiff_error);
    for (size_t i = 0; i < raster.size(); ++i) {
        const uint32_t v = raster[i];
        Rgba8 p{static_cast<uint8_t>(TIFFGetR(v)), static_cast<uint8_t>(TIFFGetG(v)), static_cast<uint8_t>(TIFFGetB(v)),
                static_cast<uint8_t>(TIFFGetA(v))};
        if (p.a > 0 && p.a < 255) {
            const double a = dec(p.a);
            p.r = q(dec(p.r) / a);
            p.g = q(dec(p.g) / a);
            p.b = q(dec(p.b) / a);
        }
        out.px[i] = p;
    }
    return out;
}

}  // namespace rl::io
