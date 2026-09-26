// SPDX-License-Identifier: GPL-3.0-or-later
//
// rasterloom: the Qt6 Widgets GUI. Contains no image math (BUILD-SPEC req 1).
//
//   rasterloom [FILE]                     open the editor (optionally with a PNG)
//   rasterloom --version [--verbose]      version; --verbose adds glibc, Qt, platform, bundled libs
//   rasterloom --license                  GPL-3.0-or-later notice (and LICENSE when shipped)
//   rasterloom --third-party-notices      bundled components and their licences
//   rasterloom --selftest [--selftest-mutate=N] [--junit-xml=F] [--selftest-dir=D]
//                                         run the core over the smoke scripts (no display needed)
//   rasterloom --smoke                    build the main window on the offscreen platform, exit 0
//
// Platform choice (BUILD-SPEC D6 + addendum, docs/research/qt-platform.md): made here before
// QApplication. RASTERLOOM_PLATFORM wins if set; then the Preferences choice; otherwise xcb when
// DISPLAY is set, ignoring an inherited QT_QPA_PLATFORM -- EXCEPT that an inherited "offscreen" or
// "minimal" is honoured, so tests and agents (QT_QPA_PLATFORM=offscreen) never open a visible window.
#include <QApplication>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>
#include <QtGlobal>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "core/base/version.hpp"
#include "core/composite/render.hpp"
#include "core/selftest/selftest.hpp"
#include "gui/app_info.hpp"
#include "gui/main_window.hpp"
#include "gui/theme.hpp"

namespace {

bool starts_with(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }

void choose_platform() {
    QByteArray forced = qgetenv("RASTERLOOM_PLATFORM");
    if (forced.isEmpty()) {
        const QByteArray inherited = qgetenv("QT_QPA_PLATFORM");
        if (inherited.startsWith("offscreen") || inherited.startsWith("minimal")) return;
        QSettings s(QStringLiteral("Rasterloom"), QStringLiteral("Rasterloom"));
        if (s.value(QStringLiteral("platform")).toString() == QLatin1String("wayland") && !qgetenv("WAYLAND_DISPLAY").isEmpty())
            forced = "wayland";
    }
    if (!forced.isEmpty()) {
        qputenv("QT_QPA_PLATFORM", forced);
        return;
    }
    if (!qgetenv("DISPLAY").isEmpty()) qputenv("QT_QPA_PLATFORM", "xcb");
}

void apply_canvas_preference() {
    if (qEnvironmentVariableIsSet("RASTERLOOM_CANVAS")) return;
    QSettings s(QStringLiteral("Rasterloom"), QStringLiteral("Rasterloom"));
    const QString b = s.value(QStringLiteral("canvas/backend")).toString();
    if (b == QLatin1String("gl") || b == QLatin1String("raster")) qputenv("RASTERLOOM_CANVAS", b.toLatin1());
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false, version = false, verbose = false, license = false, notices = false, selftest = false;
    rl::gui::SelftestOptions st;
    QString open_path;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--version") == 0) version = true;
        else if (std::strcmp(a, "--verbose") == 0) verbose = true;
        else if (std::strcmp(a, "--license") == 0) license = true;
        else if (std::strcmp(a, "--third-party-notices") == 0) notices = true;
        else if (std::strcmp(a, "--selftest") == 0) selftest = true;
        else if (starts_with(a, "--selftest-mutate=")) {
            selftest = true;
            st.mutate = std::atoi(a + std::strlen("--selftest-mutate="));
        } else if (starts_with(a, "--junit-xml=")) st.junit_xml = a + std::strlen("--junit-xml=");
        else if (starts_with(a, "--selftest-dir=")) st.dir = a + std::strlen("--selftest-dir=");
        else if (std::strcmp(a, "--smoke") == 0) smoke = true;
        else if (std::strcmp(a, "--deterministic") == 0) rl::composite::force_deterministic();
        else if (a[0] != '-') open_path = QString::fromLocal8Bit(a);
    }
    if (version) {
        std::cout << rl::gui::version_text(verbose);
        return 0;
    }
    if (license) {
        std::cout << rl::gui::license_text();
        return 0;
    }
    if (notices) {
        std::cout << rl::gui::third_party_text();
        return 0;
    }
    if (selftest) return rl::selftest::run_main(argc, argv, "rasterloom");

    QCoreApplication::setOrganizationName(QStringLiteral("Rasterloom"));
    QCoreApplication::setApplicationName(QStringLiteral("Rasterloom"));
    if (smoke) qputenv("QT_QPA_PLATFORM", "offscreen");  // a smoke run never opens a visible window
    else choose_platform();
    apply_canvas_preference();

    // GL 3.3 core for the canvas; set before QApplication.
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    QApplication::setApplicationVersion(QString::fromLatin1(rl::kVersion));
    // xcb turns event compression on in its integration constructor; turn it off afterwards so
    // mouse strokes keep every sample (tablet events are never compressed by default).
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    rl::gui::theme::apply(app);

    rl::gui::MainWindow window;
    window.show();
    if (!open_path.isEmpty()) window.open_file(open_path);
    if (smoke) {
        window.set_confirm_close(false);
        QTimer::singleShot(0, &app, [&app]() { app.exit(0); });
        const int rc = app.exec();
        std::cout << "rasterloom smoke: platform=" << QApplication::platformName().toStdString() << " window='"
                  << window.windowTitle().toStdString() << "' rc=" << rc << "\n";
        return rc;
    }
    return app.exec();
}
