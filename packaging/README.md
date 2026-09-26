<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Packaging

| File | What it does |
|---|---|
| `build-appimage.sh` | CMake install (component `rasterloom`) into an AppDir, linuxdeploy + plugin-qt, AppRun, notices, uruntime + appimagetool, `.sha256`, then `check-bundle.sh` |
| `check-bundle.sh` | Hard assertions on a finished AppImage: Wayland/xcb/offscreen plugins, svg icon engine, the GIF/ICO/WebP/TGA image readers, no driver or session libraries, the six release-hygiene files, uruntime `URUNTIME_EXTRACT=2`, size, checksum |
| `test-artifact.sh` | Runs the AppImage itself: `--version --verbose`, a byte-exact `--cli` render, `--selftest`, licence flags, an 8 s GUI start |
| `AppRun.in` | Entry point template: glibc floor check with a plain-language message, `RASTERLOOM_PLATFORM`, `--cli` dispatch |
| `bundle_notices.py` | `THIRD-PARTY-NOTICES.md` for exactly the libraries in the AppDir (versions and licence texts read from the build host), through `tools/gen_third_party_notices.py` |
| `bundle-components.yaml` | What each bundleable library is (name, SPDX, URL). An unlisted bundled library stops the build. `inside_qt_plugins` lists code compiled into a Qt plugin (the aqtinstall Qt's static libwebp / libtiff in the qtimageformats plugins), versioned from the Qt SBOM |
| `written_offer.py` | `WRITTEN-OFFER.txt`: git commit, Qt source URLs and configure options, ADS source, relink recipe |
| `elfdeps.py` | Removes libraries nothing needs after the exclusions; lists what the host must provide |
| `render-icons.sh` | Re-renders `share/icons/hicolor/*` from `share/icons/rasterloom.svg` |
| `licenses/ICU-73.txt` | ICU 73.2 licence (ships inside the official Qt 6.9.3 binaries, which carry no licence file) |
| `licenses/libwebp-COPYING.txt`, `licenses/libtiff-LICENSE.md` | Licences of the libwebp 1.6.0 / libtiff 4.7.1 copies compiled into the official Qt 6.9.3 `libqwebp.so` / `libqtiff.so` |

Tools are downloaded into `_tools/` (gitignored) from pinned release URLs and checked against
SHA-256; output goes to `_out/` (gitignored).

## Local test build (Arch)

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DQT_NO_PRIVATE_MODULE_WARNING=ON && cmake --build build
RL_PERSONAL_BUILD=1 packaging/build-appimage.sh --no-build
build/src/rasterloom-cli --render-script tests/scripts/compositing/clip_adjust.json --out /tmp/expected.png
packaging/test-artifact.sh packaging/_out/Rasterloom-x86_64.AppImage \
    tests/scripts/compositing/clip_adjust.json /tmp/expected.png
```

A local build links against the host's glibc and libraries, so it only runs on systems at least
as new (`RL_PERSONAL_BUILD=1` makes AppRun say so). `NO_STRIP=1` is the default off Ubuntu/Debian
because linuxdeploy's `strip` rejects Arch's DT_RELR libraries. The AppImage that is handed to
anyone else comes only from the CI `package` job (ubuntu:22.04, GCC 11, Qt 6.9.3, glibc 2.35).
