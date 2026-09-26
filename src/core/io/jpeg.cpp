// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/jpeg.hpp"

// jpeglib.h needs size_t and FILE declared first.
#include <cstddef>
#include <cstdio>

#include <jpeglib.h>

#include <csetjmp>
#include <string>

#include "core/base/quant.hpp"
#include "core/io/io_error.hpp"

namespace rl::io {

namespace {

struct ErrMgr {
    jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];
};

void on_error(j_common_ptr cinfo) {
    auto* e = reinterpret_cast<ErrMgr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, e->msg);
    std::longjmp(e->jb, 1);
}
void on_output(j_common_ptr) {}

}  // namespace

bool is_jpeg(const std::vector<uint8_t>& b) { return b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF; }

std::vector<uint8_t> encode_jpeg(const RgbaBuffer& img, int quality) {
    if (img.w <= 0 || img.h <= 0) throw IoError("JPEG: empty image");
    if (img.w > 65500 || img.h > 65500) throw IoError("JPEG: image too large for JPEG (max 65500 px per side)");
    std::vector<uint8_t> row(static_cast<size_t>(img.w) * 3);
    unsigned char* mem = nullptr;
    unsigned long mem_size = 0;
    jpeg_compress_struct c{};
    ErrMgr err{};
    c.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    err.pub.output_message = on_output;
    if (setjmp(err.jb)) {
        jpeg_destroy_compress(&c);
        std::free(mem);
        throw IoError(std::string("JPEG encode: ") + err.msg);
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &mem, &mem_size);
    c.image_width = static_cast<JDIMENSION>(img.w);
    c.image_height = static_cast<JDIMENSION>(img.h);
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    c.dct_method = JDCT_ISLOW;
    c.optimize_coding = FALSE;
    c.write_JFIF_header = TRUE;
    c.density_unit = 0;
    c.X_density = 1;
    c.Y_density = 1;
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        const Rgba8* src = img.px.data() + (static_cast<size_t>(c.next_scanline) * static_cast<size_t>(img.w));
        for (int x = 0; x < img.w; ++x) {
            row[static_cast<size_t>(x) * 3] = src[x].r;
            row[(static_cast<size_t>(x) * 3) + 1] = src[x].g;
            row[(static_cast<size_t>(x) * 3) + 2] = src[x].b;
        }
        JSAMPROW rp = row.data();
        jpeg_write_scanlines(&c, &rp, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    std::vector<uint8_t> out(mem, mem + mem_size);
    std::free(mem);
    return out;
}

RgbaBuffer decode_jpeg(const std::vector<uint8_t>& bytes) {
    if (!is_jpeg(bytes)) throw IoError("not a JPEG file");
    RgbaBuffer out;
    std::vector<uint8_t> row;
    jpeg_decompress_struct d{};
    ErrMgr err{};
    d.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    err.pub.output_message = on_output;
    if (setjmp(err.jb)) {
        jpeg_destroy_decompress(&d);
        throw IoError(std::string("JPEG decode: ") + err.msg);
    }
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, bytes.data(), static_cast<unsigned long>(bytes.size()));
    jpeg_read_header(&d, TRUE);
    const bool cmyk = d.jpeg_color_space == JCS_CMYK || d.jpeg_color_space == JCS_YCCK;
    const bool inverted = cmyk && d.saw_Adobe_marker;
    d.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    jpeg_start_decompress(&d);
    const int w = static_cast<int>(d.output_width), h = static_cast<int>(d.output_height);
    const int nc = d.output_components;
    if (static_cast<int64_t>(w) * h > (int64_t{1} << 30)) {
        jpeg_destroy_decompress(&d);
        throw IoError("JPEG: image too large");
    }
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    row.resize(static_cast<size_t>(w) * static_cast<size_t>(nc));
    while (d.output_scanline < d.output_height) {
        const int y = static_cast<int>(d.output_scanline);
        JSAMPROW rp = row.data();
        jpeg_read_scanlines(&d, &rp, 1);
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row.data() + (static_cast<size_t>(x) * static_cast<size_t>(nc));
            Rgba8 o{0, 0, 0, 255};
            if (cmyk) {
                // Adobe CMYK JPEGs store 255 - ink; plain ones store ink. RGB = (1-C)(1-K).
                auto keep = [inverted](uint8_t v) { return dec(inverted ? v : 255 - v); };
                const double k = keep(p[3]);
                o.r = q(keep(p[0]) * k);
                o.g = q(keep(p[1]) * k);
                o.b = q(keep(p[2]) * k);
            } else {
                o.r = p[0];
                o.g = p[1];
                o.b = p[2];
            }
            out.at(x, y) = o;
        }
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    return out;
}

}  // namespace rl::io
