// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/app_info.hpp"

#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QVBoxLayout>

#include <gnu/libc-version.h>

#include <cstdio>  // before jpeglib.h, which needs FILE
#include <jpeglib.h>  // jconfig.h: LIBJPEG_TURBO_VERSION_NUMBER
#include <lz4.h>
#include <png.h>
#include <tiffio.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/base/version.hpp"
#include "core/io/export_png.hpp"
#include "core/script/engine.hpp"
#include "gui/icons.hpp"

#ifndef RL_ADS_VERSION
#define RL_ADS_VERSION "unknown"
#endif
namespace rl::gui {

namespace fs = std::filesystem;

namespace {

// The running executable's directory from /proc/self/exe: works before QCoreApplication exists
// (--license, --third-party-notices) and embeds no build path in the binary.
fs::path exe_dir() {
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : exe.parent_path();
}

// Development tree fallback: the nearest ancestor of the executable's directory that looks like a
// Rasterloom checkout (LICENSE next to tests/scripts/smoke). Build trees inside the checkout find
// it; any other build sets RASTERLOOM_DOC_DIR / RASTERLOOM_SELFTEST_DIR (CTest does).
fs::path source_tree_root() {
    std::error_code ec;
    for (fs::path d = exe_dir(); !d.empty(); d = d.parent_path()) {
        if (fs::is_regular_file(d / "LICENSE", ec) && fs::is_directory(d / "tests" / "scripts" / "smoke", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return {};
}

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : std::string();
}

std::string tiff_version() {
    // "LIBTIFF, Version 4.7.0\nCopyright ..." -> "4.7.0"
    std::string v = TIFFGetVersion();
    v = v.substr(0, v.find('\n'));
    const std::string tag = "Version ";
    if (const size_t at = v.find(tag); at != std::string::npos) v = v.substr(at + tag.size());
    return v;
}

std::string jpeg_version() {
#ifdef LIBJPEG_TURBO_VERSION_NUMBER
    const int n = LIBJPEG_TURBO_VERSION_NUMBER;
    return "libjpeg-turbo " + std::to_string(n / 1000000) + "." + std::to_string(n / 1000 % 1000) + "." + std::to_string(n % 1000) +
           " (API " + std::to_string(JPEG_LIB_VERSION) + ", build-time)";
#else
    return "libjpeg API " + std::to_string(JPEG_LIB_VERSION) + " (build-time)";
#endif
}

}  // namespace

QString choose_platform_name() {
    const QByteArray forced = qgetenv("RASTERLOOM_PLATFORM");
    if (!forced.isEmpty()) return QString::fromLocal8Bit(forced);
    const QByteArray inherited = qgetenv("QT_QPA_PLATFORM");
    if (inherited.startsWith("offscreen") || inherited.startsWith("minimal")) return QString::fromLocal8Bit(inherited);
    if (!qgetenv("DISPLAY").isEmpty()) return QStringLiteral("xcb");
    return QString();
}

std::string version_text(bool verbose) {
    std::ostringstream o;
    o << "rasterloom " << rl::kVersion << " (Qt " << qVersion() << ")\n";
    if (!verbose) return o.str();
    const QString plat = choose_platform_name();
    o << "  glibc          " << gnu_get_libc_version() << "\n"
      << "  Qt runtime     " << qVersion() << " (built against " << QT_VERSION_STR << ")\n"
      << "  platform       " << (plat.isEmpty() ? std::string("Qt default (no DISPLAY; set RASTERLOOM_PLATFORM)") : plat.toStdString())
      << (qEnvironmentVariableIsSet("RASTERLOOM_PLATFORM") ? "  [RASTERLOOM_PLATFORM]" : "") << "\n"
      << "  canvas         " << (qEnvironmentVariableIsSet("RASTERLOOM_CANVAS") ? qgetenv("RASTERLOOM_CANVAS").toStdString() : std::string("auto (GL 3.3 probe, raster fallback)")) << "\n"
      << "  Qt-ADS         " << RL_ADS_VERSION << " (LGPL-2.1-or-later)\n"
      << "  libpng         " << png_get_libpng_ver(nullptr) << " (built against " << PNG_LIBPNG_VER_STRING << ")\n"
      << "  zlib           " << zlibVersion() << "\n"
      << "  liblz4         " << LZ4_versionString() << " (built against " << LZ4_VERSION_STRING << ")\n"
      << "  libjpeg        " << jpeg_version() << "\n"
      << "  libtiff        " << tiff_version() << "\n"
      << "  nlohmann-json  " << NLOHMANN_JSON_VERSION_MAJOR << "." << NLOHMANN_JSON_VERSION_MINOR << "." << NLOHMANN_JSON_VERSION_PATCH
      << " (header-only)\n"
      << "  compiler       "
#if defined(__clang__)
      << "clang " << __clang_version__
#elif defined(__GNUC__)
      << "GCC " << __VERSION__
#endif
      << "\n";
    return o.str();
}

namespace {

// Looks for a release-hygiene file: $RASTERLOOM_DOC_DIR, then <bindir>/../share/doc/rasterloom
// (install prefix and AppImage usr/), then the checkout the binary was built in (dev builds).
QString find_doc_file(const QString& name) {
    std::vector<fs::path> dirs;
    if (const std::string e = env_or_empty("RASTERLOOM_DOC_DIR"); !e.empty()) dirs.emplace_back(e);
    if (const fs::path b = exe_dir(); !b.empty()) dirs.push_back(b / ".." / "share" / "doc" / "rasterloom");
    if (const fs::path src = source_tree_root(); !src.empty()) dirs.push_back(src);
    std::error_code ec;
    for (const fs::path& d : dirs) {
        const fs::path p = d / name.toStdString();
        if (fs::is_regular_file(p, ec)) return QString::fromStdString(fs::weakly_canonical(p, ec).string());
    }
    return {};
}

QString read_file(const QString& p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

std::string license_text() {
    std::ostringstream o;
    o << "Rasterloom " << rl::kVersion << "\n"
      << "Copyright (C) the Rasterloom authors.\n\n"
      << "SPDX-License-Identifier: GPL-3.0-or-later\n\n"
      << "This program is free software: you can redistribute it and/or modify it under the terms of the\n"
         "GNU General Public License as published by the Free Software Foundation, either version 3 of the\n"
         "License, or (at your option) any later version.\n\n"
         "This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without\n"
         "even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General\n"
         "Public License for more details: https://www.gnu.org/licenses/gpl-3.0.html\n";
    const QString full = find_doc_file(QStringLiteral("LICENSE"));
    if (!full.isEmpty()) o << "\n---- LICENSE ----\n" << read_file(full).toStdString();
    else o << "\n(The full licence text ships as LICENSE in usr/share/doc/rasterloom/.)\n";
    return o.str();
}

std::string third_party_text() {
    std::ostringstream o;
    o << "Third-party components used by this build of Rasterloom " << rl::kVersion << ":\n\n"
      << "  Qt " << qVersion() << "               LGPL-3.0-only        https://www.qt.io/  (dynamically linked)\n"
      << "  Qt-Advanced-Docking-System " << RL_ADS_VERSION << "  LGPL-2.1-or-later  https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System  (shared library)\n"
      << "  libpng " << png_get_libpng_ver(nullptr) << "            libpng-2.0           http://www.libpng.org/pub/png/libpng.html\n"
      << "  zlib " << zlibVersion() << "              Zlib                 https://zlib.net/\n"
      << "  liblz4 " << LZ4_versionString() << "            BSD-2-Clause         https://lz4.org/\n"
      << "  " << jpeg_version().substr(0, jpeg_version().find(" (")) << "     IJG AND BSD-3-Clause AND Zlib  https://libjpeg-turbo.org/\n"
      << "  libtiff " << tiff_version() << "           libtiff              https://libtiff.gitlab.io/libtiff/\n"
      << "  nlohmann/json " << NLOHMANN_JSON_VERSION_MAJOR << "." << NLOHMANN_JSON_VERSION_MINOR << "."
      << NLOHMANN_JSON_VERSION_PATCH << "       MIT                  https://github.com/nlohmann/json\n"
      << "  glibc " << gnu_get_libc_version() << "               LGPL-2.1-or-later    (system, not bundled)\n\n"
      << "Icons are original Rasterloom artwork (GPL-3.0-or-later).\n";
    const QString full = find_doc_file(QStringLiteral("THIRD-PARTY-NOTICES.md"));
    if (!full.isEmpty()) o << "\n---- THIRD-PARTY-NOTICES.md ----\n" << read_file(full).toStdString();
    else o << "Full licence texts: THIRD-PARTY-NOTICES.md in usr/share/doc/rasterloom/ (release builds).\n";
    return o.str();
}

// ---- selftest ---------------------------------------------------------------------------------------------

namespace {

std::string find_selftest_dir(const std::string& given) {
    std::vector<std::string> cands;
    if (!given.empty()) cands.push_back(given);
    if (const std::string e = env_or_empty("RASTERLOOM_SELFTEST_DIR"); !e.empty()) cands.push_back(e);
    if (const fs::path b = exe_dir(); !b.empty()) cands.push_back((b / ".." / "share" / "rasterloom" / "selftest").string());
    if (const fs::path src = source_tree_root(); !src.empty()) cands.push_back((src / "tests" / "scripts" / "smoke").string());
    std::error_code ec;
    for (const std::string& c : cands)
        if (!c.empty() && fs::is_directory(c, ec)) return c;
    return {};
}

struct CaseResult {
    std::string name;
    bool pass = false;
    std::string detail;
    double ms = 0.0;
};

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

// Renders a script; returns false (with the message) on a script error.
bool render(const std::string& path, rl::io::RgbaBuffer& out, std::string& err) {
    try {
        rl::script::ScriptResult r = rl::script::run_script_file(path);
        out = rl::io::render_document(r.doc->state());
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool same(const rl::io::RgbaBuffer& a, const rl::io::RgbaBuffer& b) {
    return a.w == b.w && a.h == b.h && a.px == b.px;
}

}  // namespace

int run_selftest(const SelftestOptions& opt) {
    const std::string dir = find_selftest_dir(opt.dir);
    if (dir.empty()) {
        std::printf("selftest: no script directory found (set RASTERLOOM_SELFTEST_DIR)\n");
        return 2;
    }
    std::vector<fs::path> scripts;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".json") scripts.push_back(e.path());
    std::sort(scripts.begin(), scripts.end());
    std::printf("selftest: %zu scripts in %s\n", scripts.size(), dir.c_str());

    std::vector<CaseResult> results;
    int failed = 0;
    int caught = 0;
    for (const fs::path& p : scripts) {
        CaseResult cr;
        cr.name = p.stem().string();
        const auto t0 = std::chrono::steady_clock::now();
        const bool expect_error = cr.name.rfind("err_", 0) == 0;
        rl::io::RgbaBuffer a, b;
        std::string err;
        if (expect_error) {
            cr.pass = !render(p.string(), a, err);
            cr.detail = cr.pass ? "rejected: " + err : "accepted an invalid script";
        } else if (!render(p.string(), a, err)) {
            cr.detail = "script error: " + err;
        } else if (!render(p.string(), b, err) || !same(a, b)) {
            cr.detail = "two renders differ (non-deterministic)";
        } else {
            cr.pass = true;
            cr.detail = std::to_string(a.w) + "x" + std::to_string(a.h) + " deterministic";
            if (opt.mutate >= 0) {
                rl::mut::ScopedMutations m({opt.mutate});
                rl::io::RgbaBuffer c;
                std::string e2;
                if (!render(p.string(), c, e2) || !same(a, c)) {
                    ++caught;
                    cr.detail += ", mutation " + std::to_string(opt.mutate) + " changes the output";
                }
            }
        }
        cr.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!cr.pass) ++failed;
        std::printf("  %-4s %-40s %s\n", cr.pass ? "ok" : "FAIL", cr.name.c_str(), cr.detail.c_str());
        results.push_back(cr);
    }
    int rc = failed == 0 ? 0 : 1;
    if (opt.mutate >= 0) {
        std::printf("selftest: mutation %d (%s) caught by %d script(s)\n", opt.mutate, rl::mut::description(opt.mutate), caught);
        if (caught == 0) {
            std::printf("selftest: SUITE IS VACUOUS FOR MUTATION %d\n", opt.mutate);
            rc = 1;
        }
    }
    std::printf("selftest: %zu passed, %d failed\n", results.size() - static_cast<size_t>(failed), failed);
    if (!opt.junit_xml.empty()) {
        std::ofstream x(opt.junit_xml);
        x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"rasterloom-selftest\" tests=\"" << results.size()
          << "\" failures=\"" << failed << "\">\n";
        for (const CaseResult& r : results) {
            x << "  <testcase classname=\"selftest\" name=\"" << xml_escape(r.name) << "\" time=\"" << r.ms / 1000.0 << "\"";
            if (r.pass) x << "/>\n";
            else x << "><failure message=\"" << xml_escape(r.detail) << "\"/></testcase>\n";
        }
        x << "</testsuite>\n";
    }
    return rc;
}

// ---- About -----------------------------------------------------------------------------------------------------

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("AboutDialog"));
    setWindowTitle(tr("About Rasterloom"));
    auto* v = new QVBoxLayout(this);
    auto* tabs = new QTabWidget;
    auto* about = new QWidget;
    auto* av = new QVBoxLayout(about);
    auto* title = new QLabel(QStringLiteral("<span style='font-size:20px; font-weight:600;'>Rasterloom</span>"
                                            "&nbsp;&nbsp;<span style='color:#8d9199;'>%1</span>")
                                 .arg(QString::fromLatin1(rl::kVersion)));
    av->addWidget(title);
    auto* blurb = new QLabel(tr("A native Linux raster image editor: layers, masks, selections and a pressure-sensitive "
                                "brush, with every pixel operation proven byte-exact against an independent reference.<br><br>"
                                "Licensed under the GNU General Public License, version 3 or later. "
                                "Not affiliated with any other image-editor vendor."));
    blurb->setWordWrap(true);
    av->addWidget(blurb);
    auto* ver = new QPlainTextEdit(QString::fromStdString(version_text(true)));
    ver->setReadOnly(true);
    ver->setFont(QFont(QStringLiteral("monospace"), 9));
    av->addWidget(ver, 1);
    tabs->addTab(about, tr("About"));
    auto* lic = new QPlainTextEdit(QString::fromStdString(license_text()));
    lic->setReadOnly(true);
    lic->setObjectName(QStringLiteral("LicenseText"));
    tabs->addTab(lic, tr("Licenses"));
    auto* tp = new QPlainTextEdit(QString::fromStdString(third_party_text()));
    tp->setReadOnly(true);
    tp->setFont(QFont(QStringLiteral("monospace"), 9));
    tabs->addTab(tp, tr("Third-party"));
    v->addWidget(tabs, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
    resize(620, 460);
}

}  // namespace rl::gui
