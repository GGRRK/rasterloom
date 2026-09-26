// SPDX-License-Identifier: GPL-3.0-or-later
// Byte-level building blocks of the file I/O lane: PackBits, SHA-256, ZIP, XML, atomic save.
#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include "core/io/atomic_file.hpp"
#include "core/io/io_error.hpp"
#include "core/io/sha256.hpp"
#include "core/io/xml_mini.hpp"
#include "core/io/zip.hpp"
#include "core/psd/psd.hpp"
#include "io_test_util.hpp"

namespace fs = std::filesystem;
using namespace rl;

std::string rliotest::scratch_dir(const std::string& name) {
    const fs::path p = fs::path(RL_TEST_OUT) / "io" / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}

namespace {
std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST(PackBits, AppleTechNote1023Example) {
    // TN1023: unpacked AA AA AA 80 00 2A AA AA AA AA 80 00 2A 22 AA AA AA AA AA AA AA AA AA AA
    const std::vector<uint8_t> packed = {0xFE, 0xAA, 0x02, 0x80, 0x00, 0x2A, 0xFD, 0xAA, 0x03, 0x80, 0x00, 0x2A, 0x22, 0xF7, 0xAA};
    const std::vector<uint8_t> want = {0xAA, 0xAA, 0xAA, 0x80, 0x00, 0x2A, 0xAA, 0xAA, 0xAA, 0xAA, 0x80, 0x00,
                                       0x2A, 0x22, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    std::vector<uint8_t> out(want.size());
    ASSERT_TRUE(psd::packbits_decode(packed.data(), packed.size(), out.data(), out.size()));
    EXPECT_EQ(out, want);
    // Our encoder emits exactly TN1023's packing for this input.
    std::vector<uint8_t> enc;
    psd::packbits_encode(want.data(), want.size(), enc);
    EXPECT_EQ(enc, packed);
}

TEST(PackBits, RandomRoundTripAndMalformedInputRejected) {
    std::mt19937 rng(7);
    for (int t = 0; t < 300; ++t) {
        std::vector<uint8_t> v(static_cast<size_t>(rng() % 700));
        for (auto& b : v) b = static_cast<uint8_t>((rng() % 4 == 0) ? rng() : (rng() % 3));
        std::vector<uint8_t> enc;
        psd::packbits_encode(v.data(), v.size(), enc);
        EXPECT_LE(enc.size(), v.size() + (v.size() + 127) / 128);
        std::vector<uint8_t> dec(v.size());
        ASSERT_TRUE(psd::packbits_decode(enc.data(), enc.size(), dec.data(), dec.size()));
        EXPECT_EQ(dec, v);
    }
    const std::vector<uint8_t> overrun = {0x05, 1, 2};  // literal of 6, only 2 bytes present
    std::vector<uint8_t> o(6);
    EXPECT_FALSE(psd::packbits_decode(overrun.data(), overrun.size(), o.data(), o.size()));
    const std::vector<uint8_t> too_long = {0xF0, 7};  // run of 17 into 4 bytes
    std::vector<uint8_t> o4(4);
    EXPECT_FALSE(psd::packbits_decode(too_long.data(), too_long.size(), o4.data(), o4.size()));
}

TEST(Sha256, Fips180Vectors) {
    EXPECT_EQ(io::sha256_hex(std::string("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(io::sha256_hex(std::string("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(io::sha256_hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Zip, MimetypeFirstStoredAtShmOffsetsAndDeterministic) {
    auto build = [] {
        io::ZipWriter z;
        const std::string mime = "image/openraster";
        z.add("mimetype", std::vector<uint8_t>(mime.begin(), mime.end()), io::ZipWriter::Method::Store);
        std::vector<uint8_t> big(5000, 'x');
        z.add("stack.xml", big);
        z.add("empty.bin", {});
        z.add("d\xC3\xA9j\xC3\xA0.txt", {1, 2, 3});
        return z.finish();
    };
    const auto a = build();
    EXPECT_EQ(a, build());
    ASSERT_GT(a.size(), 60u);
    EXPECT_EQ(std::string(a.begin() + 30, a.begin() + 38), "mimetype");
    EXPECT_EQ(std::string(a.begin() + 38, a.begin() + 54), "image/openraster");
    EXPECT_EQ(a[8] | (a[9] << 8), 0);    // STORED
    EXPECT_EQ(a[28] | (a[29] << 8), 0);  // no extra field
    io::ZipReader r(a);
    ASSERT_EQ(r.names().size(), 4u);
    EXPECT_EQ(r.names()[0], "mimetype");
    EXPECT_EQ(r.read("stack.xml"), std::vector<uint8_t>(5000, 'x'));
    EXPECT_TRUE(r.read("empty.bin").empty());
    EXPECT_EQ(r.read("d\xC3\xA9j\xC3\xA0.txt"), (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_LT(r.raw("stack.xml").csize, 200u);  // deflated
}

TEST(Zip, CorruptArchivesThrowIoError) {
    io::ZipWriter z;
    z.add("a.txt", std::vector<uint8_t>(300, 'q'));
    auto good = z.finish();
    auto bad = good;
    bad[40] ^= 0xFF;  // inside the deflate stream / crc check
    io::ZipReader r(bad);
    EXPECT_THROW(r.read("a.txt"), io::IoError);
    for (size_t cut = 0; cut < good.size(); cut += 7) {
        std::vector<uint8_t> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        try {
            io::ZipReader rr(t);
            (void)rr.read("a.txt");
        } catch (const io::IoError&) {
        }
    }
    SUCCEED();
}

TEST(Xml, ParsesStackXmlSubset) {
    const std::string s =
        "\xEF\xBB\xBF<?xml version='1.0'?><!-- c --><!DOCTYPE image>\n<image w=\"4\" h='3' xmlns:rl=\"x\">"
        "<stack><layer name=\"a &amp; b &#x263A; &#65;\" rl:id=\"n1\" src=\"data/a.png\"/>text<![CDATA[<no>]]>"
        "<stack isolation=\"auto\"></stack></stack></image>";
    const io::XmlElement e = io::parse_xml(s);
    EXPECT_EQ(e.name, "image");
    ASSERT_EQ(e.children.size(), 1u);
    const auto& st = e.children[0];
    ASSERT_EQ(st.children.size(), 2u);
    EXPECT_EQ(*st.children[0].attr("name"), "a & b \xE2\x98\xBA A");
    EXPECT_EQ(*st.children[0].attr("rl:id"), "n1");
    EXPECT_EQ(*st.children[1].attr("isolation"), "auto");
    EXPECT_THROW(io::parse_xml("<a><b></a>"), io::IoError);
    EXPECT_THROW(io::parse_xml("<a x=\"1></a>"), io::IoError);
    EXPECT_EQ(io::xml_escape("<\"&'>"), "&lt;&quot;&amp;&apos;&gt;");
}

TEST(AtomicSave, ReplacesAtomicallyKeepsModeAndLeavesNoTemp) {
    const std::string dir = rliotest::scratch_dir("atomic");
    const std::string p = dir + "/f.bin";
    io::atomic_write_file(p, {1, 2, 3});
    ::chmod(p.c_str(), 0640);
    io::atomic_write_file(p, {9, 8});
    EXPECT_EQ(slurp(p), std::string("\x09\x08"));
    struct stat st {};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0640u);
    int n = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir)) ++n;
    EXPECT_EQ(n, 1);
}

TEST(AtomicSave, FailedSaveNeverTouchesTheExistingFile) {
    const std::string dir = rliotest::scratch_dir("atomic_ro");
    const std::string p = dir + "/keep.orp";
    io::atomic_write_file(p, {'o', 'l', 'd'});
    // The temp file is created but the write fails half way (file-size limit, EFBIG). This works
    // for every user, root included (CI containers run as root).
    {
        struct rlimit old {};
        ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &old), 0);
        struct rlimit lim = old;
        lim.rlim_cur = 500;
        const auto prev = std::signal(SIGXFSZ, SIG_IGN);
        ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &lim), 0);
        EXPECT_THROW(io::atomic_write_file(p, std::vector<uint8_t>(1000, 'n')), io::IoError);
        ::setrlimit(RLIMIT_FSIZE, &old);
        std::signal(SIGXFSZ, prev);
    }
    EXPECT_EQ(slurp(p), "old");
    // No temp file can be created at all (read-only directory). Root ignores directory
    // permissions, so this part only runs for other users.
    if (::geteuid() != 0) {
        ::chmod(dir.c_str(), 0555);
        EXPECT_THROW(io::atomic_write_file(p, std::vector<uint8_t>(1000, 'n')), io::IoError);
        ::chmod(dir.c_str(), 0755);
        EXPECT_EQ(slurp(p), "old");
    }
    // A target that is a directory: refused, directory untouched.
    fs::create_directories(dir + "/sub");
    EXPECT_THROW(io::atomic_write_file(dir + "/sub", {1}), io::IoError);
    EXPECT_TRUE(fs::is_directory(dir + "/sub"));
    int n = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir)) ++n;
    EXPECT_EQ(n, 2);  // keep.orp + sub: no temp left behind
}
