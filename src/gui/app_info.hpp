// SPDX-License-Identifier: GPL-3.0-or-later
//
// Version, licence and self-test text for `rasterloom --version [--verbose]`, `--license`,
// `--third-party-notices`, `--selftest` and Help -> About. None of these needs a QApplication.
#pragma once

#include <QDialog>
#include <QString>

#include <string>

namespace rl::gui {

// The QPA platform main() will request (D6 + addendum): RASTERLOOM_PLATFORM, else an inherited
// offscreen/minimal, else xcb when DISPLAY is set, else Qt's own choice ("" = default).
QString choose_platform_name();

std::string version_text(bool verbose);
std::string license_text();
std::string third_party_text();

struct SelftestOptions {
    std::string dir;          // empty = search (RASTERLOOM_SELFTEST_DIR, bundle, source tree)
    std::string junit_xml;    // write JUnit XML here when non-empty
    int mutate = -1;          // >= 0: activate this mutation and require the suite to catch it
};
// Runs every script in the smoke directory through the core twice (determinism, byte equality),
// error scripts must be rejected. Returns the process exit code (0 = pass).
int run_selftest(const SelftestOptions& o);

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};

}  // namespace rl::gui
