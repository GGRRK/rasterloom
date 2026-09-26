// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/png.hpp"

#include <png.h>

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace rl::io {

namespace {

constexpr int kZlibLevel = 6;

struct ErrState {
    char msg[256] = {0};
};

void on_error(png_structp png, png_const_charp text) {
    auto* st = static_cast<ErrState*>(png_get_error_ptr(png));
    if (st) std::snprintf(st->msg, sizeof(st->msg), "%s", text ? text : "libpng error");
    png_longjmp(png, 1);
}
void on_warning(png_structp, png_const_charp) {}

struct MemSink {
    std::vector<uint8_t>* out = nullptr;
    std::FILE* fp = nullptr;
    bool io_failed = false;
};

void sink_write(png_structp png, png_bytep data, png_size_t len) {
    auto* s = static_cast<MemSink*>(png_get_io_ptr(png));
    if (s->out) {
        s->out->insert(s->out->end(), data, data + len);
    } else if (std::fwrite(data, 1, len, s->fp) != len) {
        s->io_failed = true;
        png_error(png, "write failed");
    }
}
void sink_flush(png_structp png) {
    auto* s = static_cast<MemSink*>(png_get_io_ptr(png));
    if (s->fp) std::fflush(s->fp);
}

// Returns an empty string on success, else the error message. All C++ objects with destructors are
// owned by the caller, so a longjmp back to setjmp skips nothing that needs destroying. The row
// provider is only called between libpng calls, never from inside libpng.
std::string encode_impl(MemSink& sink, int w, int h, const RowProvider& rows, std::exception_ptr& cb_error) {
    ErrState err;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &err, on_error, on_warning);
    if (!png) return "png_create_write_struct failed";
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        return "png_create_info_struct failed";
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return err.msg[0] ? err.msg : "libpng error";
    }
    png_set_write_fn(png, &sink, sink_write, sink_flush);
    png_set_IHDR(png, info, static_cast<png_uint_32>(w), static_cast<png_uint_32>(h), 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, kZlibLevel);
    png_write_info(png, info);
    for (int y = 0; y < h; ++y) {
        const Rgba8* row = nullptr;
        try {
            row = rows(y);
        } catch (...) {
            cb_error = std::current_exception();
            png_destroy_write_struct(&png, &info);
            return "row provider failed";
        }
        png_write_row(png, reinterpret_cast<png_const_bytep>(row));
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return {};
}

std::string decode_impl(png_structp png, png_infop info, RgbaBuffer& out, std::vector<png_bytep>& row_ptrs) {
    png_read_info(png, info);
    const png_uint_32 w = png_get_image_width(png, info);
    const png_uint_32 h = png_get_image_height(png, info);
    png_set_expand(png);  // palette -> RGB, grey < 8 bit -> 8 bit, tRNS -> alpha
    png_set_scale_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != static_cast<png_size_t>(w) * 4) return "unexpected row size after expansion";
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.px.assign(static_cast<size_t>(w) * h, Rgba8{});
    row_ptrs.resize(h);
    for (png_uint_32 y = 0; y < h; ++y)
        row_ptrs[y] = reinterpret_cast<png_bytep>(out.px.data() + (static_cast<size_t>(y) * w));
    png_read_image(png, row_ptrs.data());
    png_read_end(png, nullptr);
    return {};
}

struct MemSource {
    const std::vector<uint8_t>* in = nullptr;
    size_t pos = 0;
};

void source_read(png_structp png, png_bytep data, png_size_t len) {
    auto* s = static_cast<MemSource*>(png_get_io_ptr(png));
    if (s->pos + len > s->in->size()) png_error(png, "unexpected end of PNG data");
    std::memcpy(data, s->in->data() + s->pos, len);
    s->pos += len;
}

}  // namespace

void write_png_rows(const std::string& path, int w, int h, const RowProvider& rows) {
    if (w <= 0 || h <= 0) throw std::runtime_error("PNG: empty image");
    const std::string tmp = path + ".tmp";
    std::FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) throw std::runtime_error("cannot open '" + tmp + "' for writing");
    MemSink sink;
    sink.fp = fp;
    std::exception_ptr cb_error;
    std::string err = encode_impl(sink, w, h, rows, cb_error);
    if (std::fclose(fp) != 0 && err.empty()) err = "close failed";
    if (!err.empty()) {
        std::remove(tmp.c_str());
        if (cb_error) std::rethrow_exception(cb_error);
        throw std::runtime_error("PNG write '" + path + "': " + err);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        throw std::runtime_error("cannot rename '" + tmp + "' to '" + path + "'");
    }
}

void write_png(const std::string& path, const RgbaBuffer& img) {
    write_png_rows(path, img.w, img.h,
                   [&](int y) { return img.px.data() + (static_cast<size_t>(y) * static_cast<size_t>(img.w)); });
}

std::vector<uint8_t> encode_png_rows(int w, int h, const RowProvider& rows) {
    if (w <= 0 || h <= 0) throw std::runtime_error("PNG: empty image");
    std::vector<uint8_t> out;
    MemSink sink;
    sink.out = &out;
    std::exception_ptr cb_error;
    const std::string err = encode_impl(sink, w, h, rows, cb_error);
    if (cb_error) std::rethrow_exception(cb_error);
    if (!err.empty()) throw std::runtime_error("PNG encode: " + err);
    return out;
}

std::vector<uint8_t> encode_png(const RgbaBuffer& img) {
    std::vector<uint8_t> out;
    MemSink sink;
    sink.out = &out;
    std::exception_ptr cb_error;
    const std::string err = encode_impl(sink, img.w, img.h, [&](int y) {
        return img.px.data() + (static_cast<size_t>(y) * static_cast<size_t>(img.w));
    }, cb_error);
    if (cb_error) std::rethrow_exception(cb_error);
    if (!err.empty()) throw std::runtime_error("PNG encode: " + err);
    return out;
}

RgbaBuffer decode_png(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 8 || png_sig_cmp(bytes.data(), 0, 8) != 0) throw std::runtime_error("not a PNG file");
    RgbaBuffer out;
    std::vector<png_bytep> rows;
    MemSource src{&bytes, 0};
    ErrState err;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &err, on_error, on_warning);
    if (!png) throw std::runtime_error("png_create_read_struct failed");
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        throw std::runtime_error("png_create_info_struct failed");
    }
    std::string msg;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        throw std::runtime_error(std::string("PNG decode: ") + (err.msg[0] ? err.msg : "libpng error"));
    }
    png_set_read_fn(png, &src, source_read);
    msg = decode_impl(png, info, out, rows);
    png_destroy_read_struct(&png, &info, nullptr);
    if (!msg.empty()) throw std::runtime_error("PNG decode: " + msg);
    return out;
}

RgbaBuffer read_png(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open '" + path + "'");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decode_png(bytes);
}

}  // namespace rl::io
