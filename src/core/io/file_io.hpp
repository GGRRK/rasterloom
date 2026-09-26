// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opening and saving documents in every supported format (BUILD-SPEC req 11). This is the API the
// GUI and the CLI call; it has no Qt dependency.
//
//   format   open                                    save
//   .orp/.ora whole document (document.json) or      whole document (lossless for .orp/.ora
//            generic OpenRaster                      written by Rasterloom)
//   .psd/.psb layer tree + foreign pass-through      layer tree + merged composite (PSB above
//                                                    30000 px or for a .psb name)
//   .png      one raster layer                       export: the composite over the background
//   .jpg/.jpeg one raster layer                      export: the composite, matted over white
//   .tif/.tiff one raster layer                      export: the composite (8-bit RGB / RGBA)
//
// Errors: every function throws io::IoError with a user-facing message. Saving is atomic
// (core/io/atomic_file.hpp): on any failure the existing file at `path` is left untouched.
// Non-fatal losses (a conversion, a dropped block, a property a format cannot hold) are returned as
// warnings.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/doc/document.hpp"
#include "core/io/png.hpp"

namespace rl::io {

enum class FileFormat { Unknown, Png, Jpeg, Tiff, Ora, Psd };

// By extension, case-insensitive (.orp and .ora are both Ora; .psb is Psd).
FileFormat format_for_path(const std::string& path);
// By content (magic bytes).
FileFormat sniff_format(const std::vector<uint8_t>& bytes);
const char* format_name(FileFormat f);

struct OpenResult {
    std::unique_ptr<Document> doc;
    FileFormat format = FileFormat::Unknown;
    std::vector<std::string> warnings;
};

// Opens any supported file (the content decides the format; the extension is only a fallback).
// The document gets the GUI default history depth and an empty history.
OpenResult open_document(const std::string& path);
// The same from bytes already in memory.
DocState decode_document(const std::vector<uint8_t>& bytes, FileFormat& format, std::vector<std::string>& warnings);

struct SaveOptions {
    int jpeg_quality = 92;   // 1..100
    bool force_psb = false;  // also implied by a ".psb" extension
};

// Saves or exports by the extension of `path`. Returns warnings. Throws IoError.
std::vector<std::string> save_document(const DocState& s, const std::string& path, const SaveOptions& opt = {});
// The bytes save_document would write (no file access).
std::vector<uint8_t> encode_document(const DocState& s, FileFormat f, const SaveOptions& opt,
                                     std::vector<std::string>& warnings);

// The layer tree as JSON (names, order, kinds, blend, opacity, fill, visibility, clip, clbl,
// lock, mask presence, group boundaries and each node's foreign block keys + sha256), for
// structural diffs (rasterloom-cli --dump-tree).
std::string dump_tree_json(const DocState& s);

// One raster layer (id "n1", name `name`) holding `img`, canonicalised: what opening a PNG, JPEG or
// TIFF produces. The GUI uses it for formats only Qt's image plugins read (GIF, BMP, WebP, ...).
DocState single_layer_document(const RgbaBuffer& img, const std::string& name);

}  // namespace rl::io
