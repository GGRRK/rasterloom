# Qt platform research: tablet, canvas, docking, theme, packaging, headless tests

Status: research note, 2026-09-26. Scope is BUILD-SPEC `<stack>`, D6, req 13-14 and the packaging
parts of `<verification>`, read against the 2026-09-26 addendum (local Arch + Qt 6.11.2, CI/AppImage
Qt 6.9.3 + GCC 11).

**Evidence.** Findings marked **[measured]** come from probes built and run on this machine
(Qt 6.11.2, GCC 16, Mesa 26.2.3, NVIDIA driver, a wlroots-style Wayland compositor with Xwayland 24.1.13). Every
Qt probe ran under `QT_QPA_PLATFORM=offscreen` and opened no visible window. Findings marked
**[source]** come from reading the upstream code at the named tag. Scratch code is in
a temporary directory (`probe/`, `adsprobe/`, `AppDir/`, `*.log`).

---

## 0. Findings that change the spec or the plan

1. **Qt 6.10 moved the Wayland client into qtbase and renamed the platform plugin.** **[measured]**
   - **Qt 6.9.3** (aqt `qtwayland` archive) ships these plugins:
     - `platforms/libqwayland-generic.so` and `platforms/libqwayland-egl.so`
     - `wayland-graphics-integration-client/*`
     - `wayland-shell-integration/libxdg-shell.so` and the other shells
     - `wayland-decoration-client/{libadwaita,libbradient}.so`
   - **Qt 6.11.2** (Arch) ships a single `platforms/libqwayland.so`. It is owned by **qt6-base**, as
     are `libQt6WaylandClient.so.6`, `libxdg-shell.so` and `libqt-plugin-wayland-egl.so`. Only the
     decoration plugins remain in `qt6-wayland`.
   - Consequence: the spec's assertion `grep -q libqwayland-generic.so` fails on any 6.10+ bundle.
     Use `grep -qE 'platforms/libqwayland(-generic)?\.so'`.
2. **The spec's `unsquashfs -l Rasterloom-x86_64.AppImage` cannot work as written.** The squashfs
   image starts after the runtime (offset 1226208 bytes with uruntime 0.8.1 **[measured]**). Use
   `unsquashfs -o "$(./X.AppImage --appimage-offset)" -l X.AppImage`. The alternative is
   `./X.AppImage --appimage-extract` followed by `find squashfs-root`, which needs no squashfs-tools.
3. **linuxdeploy does not exclude `libglib-2.0.so.0`.** It bundled it **[measured]**. glib is not
   on the upstream excludelist. The spec forbids bundling it, so pass
   `--exclude-library 'libglib-2.0.so*'` explicitly.
4. **`linuxdeploy-plugin-checkrt` is archived.** The repo says "Deprecated; please use
   darealshinji/linuxdeploy-plugin-checkrt directly" (fork release `r4`, 2025-10-24). With the
   distro GCC 11 on ubuntu:22.04, libstdc++ is older than on every target distro, so checkrt does
   nothing useful. Drop it, or point at the fork.
5. **The commissioner's session exports `QT_QPA_PLATFORM=wayland;xcb` and
   `QT_QPA_PLATFORMTHEME=qt6ct`** **[measured]**.
   - D6 ("default xcb") is silently defeated if the app honours an inherited `QT_QPA_PLATFORM`.
   - qt6ct's palette leaked into offscreen tests: the window colour was `#1a1b25` when tests ran
     with the inherited environment.
   - Tests must run with `QT_QPA_PLATFORMTHEME` unset, and the app must set style and palette
     explicitly.
6. **ADS latest is v5.1.1 (2026-08-20), not 5.0.** It fixes a use-after-free and two
   undock/auto-hide crashes. It builds and runs against Qt 6.11.2 via FetchContent **[measured]**.
7. **Local AppImage builds on Arch need `NO_STRIP=1`.** linuxdeploy's bundled `strip` rejects Arch
   libraries that use DT_RELR (`unknown type [0x13] section '.relr.dyn'`). It then exits 1 **[measured]**.
8. **Qt ≥ 6.10 requires private modules to be found explicitly.**
   `target_link_libraries(... Qt6::GuiPrivate)` fails at configure time unless
   `find_package(Qt6 ... COMPONENTS GuiPrivate)` names it **[measured]**. ADS does this itself. The
   form is also valid on 6.9.

---

## 1. Tablet input (Qt 6.9-6.11)

### API

The signatures below are identical in qtbase `v6.9.3` and the local 6.11.2 headers **[source]**.

```cpp
// <QTabletEvent>  (QTabletEvent : QSinglePointEvent)
QTabletEvent(QEvent::Type t, const QPointingDevice *device,
             const QPointF &pos, const QPointF &globalPos,
             qreal pressure, float xTilt, float yTilt,
             float tangentialPressure, qreal rotation, float z,
             Qt::KeyboardModifiers keyState,
             Qt::MouseButton button, Qt::MouseButtons buttons);
// read side
QPointF position() const; QPointF globalPosition() const;   // logical px, sub-pixel
qreal pressure() const;           // 0..1
qreal xTilt() const, yTilt() const;   // degrees, -60..+60
qreal rotation() const, tangentialPressure() const, z() const;
QPointingDevice::PointerType pointerType() const;  // via QPointerEvent
QInputDevice::DeviceType deviceType() const;
const QPointingDevice *pointingDevice() const;
quint64 timestamp() const;        // ms; several samples may share one value

// <QPointingDevice>
QPointingDevice(const QString &name, qint64 systemId, QInputDevice::DeviceType devType,
                PointerType pType, Capabilities caps, int maxPoints, int buttonCount,
                const QString &seatName = QString(),
                QPointingDeviceUniqueId uniqueId = QPointingDeviceUniqueId(),
                QObject *parent = nullptr);
enum class PointerType { Unknown=0, Generic=1, Finger=2, Pen=4, Eraser=8, Cursor=0x10, AllPointerTypes=0x7FFF };
QPointingDeviceUniqueId uniqueId() const;   // .numericId() -> qint64, -1 if unknown
// QInputDevice::DeviceType { Unknown, Mouse, TouchScreen, TouchPad, Puck=0x8, Stylus=0x10, Airbrush=0x20, Keyboard }
// QInputDevice::Capability  { Position, Pressure=0x4, Rotation=0x400, XTilt=0x800, YTilt=0x1000,
//                              TangentialPressure=0x2000, ZPosition=0x4000, Hover=0x200, ... }
```

Docs: https://doc.qt.io/qt-6/qtabletevent.html, https://doc.qt.io/qt-6/qpointingdevice.html,
https://doc.qt.io/qt-6/qinputdevice.html

### Delivery to QWidget or QOpenGLWidget

- A `QOpenGLWidget` is an ordinary `QWidget` for input. Reimplement
  `void tabletEvent(QTabletEvent *e)` on the canvas widget.
- `QWidget::tabletEvent`'s default implementation **ignores** the event. An ignored event
  propagates to the parent and then becomes eligible for mouse synthesis. Call `e->accept()`
  explicitly (https://doc.qt.io/qt-6/qwidget.html#tabletEvent).
- Hover samples need `setTabletTracking(true)`, for the brush cursor outline and for the
  Input Diagnostics rate counter. Without it, `TabletMove` arrives only while the pen is in
  contact or a barrel button is held.
- Mouse synthesis:
  - `Qt::AA_SynthesizeMouseForUnhandledTabletEvents` is **on** by default **[measured]**.
  - Qt synthesizes a mouse event only when the tablet event was **not** accepted. Synthesis
    happens in `QGuiApplicationPrivate::processTabletEvent`, and only for events from the
    window system.
  - Defensive rule for the canvas: while a pen stroke is active, drop mouse events where
    `e->pointingDevice()->type() != QInputDevice::DeviceType::Mouse` or
    `e->source() != Qt::MouseEventNotSynthesized`. The stroke then never gets two sample
    streams.
- Coordinates: `position()` is in logical (device-independent) pixels with sub-pixel precision.
  Multiply by `devicePixelRatioF()` only when mapping to framebuffer pixels. Map to document
  space with the view transform in `double`.
- Identity:
  - `pointingDevice()->uniqueId().numericId()` is the physical tool serial when the driver
    reports one (Wacom). Otherwise it is -1.
  - Use the (`systemId`, `pointerType`) pair as the per-tool preset key.
  - Under XWayland the eraser end is a **separate XI2 device** (see below), so it arrives with a
    different `QPointingDevice*`.

### Compression: getting every sample

- `Qt::AA_CompressHighFrequencyEvents`:
  - xcb **sets it to true** at startup (`qxcbintegration.cpp:116`, v6.9.3) **[source]**.
  - offscreen leaves it false **[measured]**.
  - On xcb it compresses core `MOTION_NOTIFY` and XI2 `XI_Motion`.
- The xcb `compressEvent()` (qxcbconnection.cpp ~L961-990, v6.9.3) has an explicit carve-out
  **[source]**:
  `if (!testAttribute(Qt::AA_CompressTabletEvents) && tabletDataForDevice(sourceid)) return false;`
  In other words, **XI2 motion from a recognised tablet device is never compressed unless
  `AA_CompressTabletEvents` is on**, and that attribute defaults to false
  (https://doc.qt.io/qt-6/qt.html#ApplicationAttribute-enum: "Input events from tablet devices are
  not compressed by default").
- Recommendation:
  - Leave `AA_CompressTabletEvents` false.
  - **Mouse** strokes are compressed on xcb. For mouse painting fidelity, call
    `QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false)` after the
    `QApplication` is constructed. xcb sets the attribute in the integration constructor, so
    setting it before is overwritten.
  - Cost: more `mouseMoveEvent`s. That is fine because the stroke engine consumes samples
    without repainting per sample; repaint is timer or `update()`-coalesced.
- Native Wayland does not compress tablet-v2 frames in the QPA. Each `zwp_tablet_tool_v2.frame`
  becomes one `QTabletEvent`.
- Input Diagnostics "event rate" should count `QTabletEvent`s per second in `tabletEvent()` itself.

### XWayland vs native Wayland on Hyprland

- **xcb over XWayland (D6 default):**
  - Hyprland implements `zwp_tablet_manager_v2`, found in `src/managers/ProtocolManager.cpp`.
  - Xwayland turns tablet-v2 tools into XI2 devices named `xwayland-tablet stylus:N`,
    `xwayland-tablet eraser:N` and `xwayland-tablet cursor:N`.
  - Qt's xcb heuristic (`qxcbconnection_xi2.cpp` ~L334-380, v6.9.3) **[source]**:
    - It first marks a device as a tablet if it has `Abs X`, `Abs Y` and `Abs Pressure` valuators.
    - Then name `contains("eraser")` gives `PointerType::Eraser`.
    - `contains("pen"|"stylus")` plus tablet valuators gives `Pen`.
    - `"cursor"` gives `Cursor`.
    - **Anything else gives *not a tablet*.**
  - So XWayland's naming maps cleanly to Pen/Eraser, and pressure and tilt come through as
    valuators.
  - Not verified on hardware, because no tablet is attached here. The Input Diagnostics panel
    (req 14) is the verification path. Put the raw `pointingDevice()->name()` in the panel,
    because the xcb heuristic keys off it.
- **Native Wayland (`QT_QPA_PLATFORM=wayland`, D6 opt-in):**
  - Qt's Wayland QPA binds tablet-v2 directly and reports Pen/Eraser tool types, pressure
    (normalised from 0..65535) and tilt.
  - D6's native-Wayland costs are Wayland-protocol facts and apply on Hyprland too:
    - window-local `QCursor::pos()`
    - no-op `QWindow::setPosition()`
  - Hyprland draws no server-side decorations for Qt by default, so Qt uses its own decoration
    plugin. `libadwaita.so` and `libbradient.so` are deployed by linuxdeploy **[measured]**.
- The platform choice has to override the commissioner's exported `QT_QPA_PLATFORM=wayland;xcb`
  (see §0.5). Recommended in `main()` before `QApplication`:
  - if `RASTERLOOM_PLATFORM` is set, `qputenv("QT_QPA_PLATFORM", it)`
  - otherwise force `xcb` when `DISPLAY` is set
  - otherwise leave the environment alone

  This works identically in the build tree and in the AppImage, so AppRun stays a plain symlink.

### Synthesizing tablet input in tests

**A. Handler-level tests** (unit-test the canvas' `tabletEvent` → stroke-sample path) **[measured]**:

```cpp
QPointingDevice pen("Test Pen", 4242, QInputDevice::DeviceType::Stylus,
    QPointingDevice::PointerType::Pen,
    QInputDevice::Capability::Position | QInputDevice::Capability::Pressure |
    QInputDevice::Capability::XTilt | QInputDevice::Capability::YTilt,
    /*maxPoints*/1, /*buttons*/3, QString(), QPointingDeviceUniqueId::fromNumericId(0xABCDEF));
QPointF lp(10.5, 20.25);
QTabletEvent press(QEvent::TabletPress, &pen, lp, w.mapToGlobal(lp),
                   0.5, 12.f, -7.f, 0.f, 0.0, 0.f, Qt::NoModifier, Qt::LeftButton, Qt::LeftButton);
QCoreApplication::sendEvent(&w, &press);
// then TabletMove (button=Qt::NoButton, buttons=Qt::LeftButton) ..., TabletRelease (buttons=Qt::NoButton)
```

- Every field arrived intact: sub-pixel position, pressure, tilt, `Eraser`/`Stylus` types,
  `uniqueId`, name.
- `sendEvent` never triggers mouse synthesis. Read `event.isAccepted()` afterwards rather than the
  `bool` return: `sendEvent` returned `true` for an ignored event **[measured]**.

**B. Delivery-path tests** (exercise the QGuiApplication → QWidgetWindow routing, grabs and
propagation) use the QPA injection API. It needs `Qt6::GuiPrivate` and `#include <qpa/qwindowsysteminterface.h>`:

```cpp
QWindowSystemInterface::registerInputDevice(pen);   // heap-allocated QPointingDevice*
QWindowSystemInterface::handleTabletEvent(QWindow *window, const QPointingDevice *device,
    const QPointF &local, const QPointF &global, Qt::MouseButtons buttons,
    qreal pressure, qreal xTilt, qreal yTilt, qreal tangentialPressure, qreal rotation, int z,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier);
QCoreApplication::processEvents();   // injection is queued
```

- 22 injected events produced 22 `tabletEvent` calls under offscreen **[measured]**.
- Ignored events did **not** produce synthesized mouse events, because the default
  `platformSynthesizesMouse` assumption applies to injected events.
- Therefore mouse-synthesis behaviour can only be observed on real xcb or Wayland.
- Recommendation: use A for the golden and mutation suite (mutation 8, "spacing accumulator reset
  per input event", needs deterministic multi-sample input), and B for one gui-smoke delivery test.

---

## 2. Canvas: QOpenGLWidget, GL 3.3 Core

### Context and functions [measured]

```cpp
QSurfaceFormat fmt; fmt.setVersion(3,3); fmt.setProfile(QSurfaceFormat::CoreProfile);
QSurfaceFormat::setDefaultFormat(fmt);            // before QApplication
// in initializeGL():
auto *gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context()); // <QOpenGLVersionFunctionsFactory>, Qt6::OpenGL
gl->initializeOpenGLFunctions();
```

- `QOpenGLContext::versionFunctions()` no longer exists in Qt 6. The replacement is
  `QOpenGLVersionFunctionsFactory`.
- Link `Qt6::OpenGL` and `Qt6::OpenGLWidgets`. The spec's module list omits `OpenGL`; it is pulled
  in transitively, but name it.

### Tile upload [measured]

- `glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 64, 64, N, …)` followed by
  `glTexSubImage3D(…, layer, 64, 64, 1, GL_RGBA, GL_UNSIGNED_BYTE, ptr)` ran with `glGetError()==0`
  on NVIDIA.
- Recommendation: **texture arrays or atlas pages** (e.g. 2048 layers or one 4096² page = 4096
  tiles) with a free-slot list, one draw call per page. Avoid one GL texture per tile: an 8k×8k
  document is 16384 tiles, which means 16384 binds and draws.
- `glTexSubImage2D` into an atlas is equivalent; the spec's wording holds.
- Rows are 64×4 = 256 bytes, so `GL_UNPACK_ALIGNMENT` 4 is fine. Set `GL_UNPACK_ROW_LENGTH` only
  when uploading a sub-rect of a larger buffer.

### Premultiplied upload

- Tiles are straight RGBA8, so upload **premultiplied** display copies (display-only, not subject
  to goldens) and blend with `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`.
- Straight-alpha textures with `GL_LINEAR` or minification bleed the RGB of transparent texels
  into edges. Premultiplying in the shader after sampling is too late.
- Magnification ≥ 100 %: use `GL_NEAREST`, the pixel-editor convention.
- Zoom-out: sample a CPU-built premultiplied mip pyramid of tiles rather than `glGenerateMipmap`
  per upload.

### Checkerboard

- Draw it in the fragment shader from `gl_FragCoord.xy / (cell * dpr)` so it stays fixed in
  screen space and does not swim with pan.
- Then composite the premultiplied tile over it.
- No texture is needed.

### HiDPI [measured]

- With `QT_SCALE_FACTOR=2`: `resizeGL(w,h)` receives **logical** sizes (100×50) while the
  framebuffer is 200×100 (`grabFramebuffer()` returns 200×100, dpr 2).
- Qt sets `glViewport` itself before `paintGL`. Compute projection from
  `width()*devicePixelRatioF()`.

### Reparenting

- Reparenting a `QOpenGLWidget` to another top-level destroys its context
  (https://doc.qt.io/qt-6/qopenglwidget.html, "Resource Initialization and Cleanup"). Connect
  `context()->aboutToBeDestroyed` to a cleanup that calls `makeCurrent()`, and re-upload
  everything in `initializeGL()`.
- With ADS, make the canvas the **central widget** (`CDockManager::setCentralWidget`), which is
  not floatable, so it is never reparented.
- Keep the Navigator as a raster widget fed from the cached tiles, not a second `QOpenGLWidget`.

### Does QOpenGLWidget work under `QT_QPA_PLATFORM=offscreen`? [measured]

The offscreen plugin's GL path is **GLX over `$DISPLAY`**: it links `libGLX.so.0` and calls
`glXCreateNewContext`/`XCreateWindow` with no map.

| Environment | Standalone `QOpenGLContext::create()` | `QOpenGLWidget::initializeGL` | `grabFramebuffer()` |
|---|---|---|---|
| offscreen, `DISPLAY` unset (CI without xvfb, sandboxed tests) | **false** | never called | null image |
| offscreen, `DISPLAY` set (the commissioner's desktop) | true: NVIDIA, 3.3 core | called | real pixels |
| offscreen + `LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa` | true: `llvmpipe (LLVM 22.1.8, 256 bits)` 3.3 core | called | real pixels |

- `QWidget::grab()` on the GL widget returned a non-null pixmap even with no context. It is blank,
  and silently so.
- **So offscreen tests are machine-dependent unless the canvas backend is forced.**

Recommendation:
- **Detection (production).** Before constructing the canvas widget:
  1. Build a standalone `QOpenGLContext` with the 3.3 core format and a `QOffscreenSurface`.
  2. Require `create() && makeCurrent(&surf)`, `format().majorVersion()*10+minor >= 33` and
     `profile()==CoreProfile`.
  3. Read `GL_VENDOR`/`GL_RENDERER`/`GL_VERSION` and log them for Diagnostics.
  4. If the context fails, use the raster fallback.
  5. If `GL_RENDERER` matches `QRegularExpression("llvmpipe|softpipe|swiftshader", CaseInsensitive)`,
     run the spec's upload-throughput check (e.g. time 256 `glTexSubImage2D` 64² uploads plus a
     `glFinish`) and fall back if it is below threshold.
  6. Also handle a late failure: if `initializeGL` is never called by first show, or
     `context()->isValid()` is false, swap in the raster widget.
  7. Add an override: `RASTERLOOM_CANVAS=gl|raster`.
- **Tests.** Every offscreen GUI test sets `RASTERLOOM_CANVAS=raster` and unsets `DISPLAY` (CTest
  `ENVIRONMENT` / `ENVIRONMENT_MODIFICATION`: `DISPLAY=unset:`, `QT_QPA_PLATFORMTHEME=unset:`,
  `WAYLAND_DISPLAY=unset:`). The raster `QPainter::drawImage` path is the one offscreen tests
  exercise and the one the contact sheet shows.
- The GL path gets one explicit test that is skipped (`QSKIP`) when context creation fails. It
  runs in CI under `xvfb-run` with Mesa llvmpipe and compares `grabFramebuffer()` of a known tile
  against the raster path within a tolerance, not byte-exact.

---

## 3. Qt-Advanced-Docking-System

- **Latest release: `v5.1.1` (2026-08-20).** Previous releases: `5.0.0` (2026-06-26), `4.5.0`
  (2026-01-11). https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/releases
- 5.0.0 added "full Wayland support for Linux (#844, #837)", dark-mode stylesheets and a
  `DisableStylesheet` flag.
- 5.1.1 fixed "use-after-free in updateTabs queued call (#860)" and "2 crashes in singleShot timers
  while undocking tabs (especially in auto hide mode) (#792)". It also removed qmake, and CI now
  covers Ubuntu 20.04, 22.04, 24.04 and 26.04.
- **Pin v5.1.1**, not "5.0".

**Qt compatibility:**
- `src/CMakeLists.txt` requests `GuiPrivate` for Qt ≥ 6.9 and links `Qt6::GuiPrivate` (it includes
  `qpa/qplatformnativeinterface.h`) plus `xcb` directly on Linux.
- Because it uses private Qt API, the ADS `.so` must be built against the exact Qt it ships with.
  That is automatic with FetchContent.
- Built and ran against Qt 6.11.2 **[measured]**, including the perspective save/load round-trip
  and `restoreState()==true`.
- Qt 6.9 is the upstream CI baseline (issue #738, "Qt 6.9.0 + master result in broken windows on
  Ubuntu 22.04", was fixed by the Wayland work).
- Build deps on ubuntu:22.04: `libxcb1-dev` (ADS links `xcb`).

**FetchContent recipe [measured]:**
```cmake
find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Widgets GuiPrivate)
set(QT_VERSION_MAJOR 6)                                  # ADS reads this
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)              # ADS option (generic name!)
set(BUILD_STATIC   OFF CACHE BOOL "" FORCE)              # ADS option (generic name!)
set(QT_NO_PRIVATE_MODULE_WARNING ON)                     # silences Qt 6.10+ private-module warning
include(FetchContent)
FetchContent_Declare(qtads
  GIT_REPOSITORY https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System.git
  GIT_TAG v5.1.1  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(qtads)
target_link_libraries(rasterloom_app PRIVATE ads::qtadvanceddocking-qt6)
```

- Targets: `qtadvanceddocking-qt6`, with aliases `ads::qtadvanceddocking-qt6` and
  `ads::qtadvanceddocking`.
- Output: `libqtadvanceddocking-qt6.so.5.1.1` under `<build>/x64/lib/`.
- `BUILD_EXAMPLES` and `BUILD_STATIC` are un-prefixed options. Do not reuse those names for our own
  options.
- For reproducible CI, prefer `URL …/archive/refs/tags/v5.1.1.tar.gz URL_HASH SHA256=…` over git,
  or vendor it.

**Perspectives API** (DockManager.h, v5.1.1):
```cpp
void addPerspective(const QString& UniquePrespectiveName);
void removePerspective(const QString& Name);
void removePerspectives(const QStringList& Names);
QStringList perspectiveNames() const;
void savePerspectives(QSettings& Settings) const;
void loadPerspectives(QSettings& Settings);
void openPerspective(const QString& PerspectiveName);     // slot; emits openingPerspective/perspectiveOpened
QByteArray saveState(int version = 0) const;
bool restoreState(const QByteArray &state, int version = 0);
static void setConfigFlag(eConfigFlag, bool on = true);   // e.g. FocusHighlighting, DisableStylesheet
static void setAutoHideConfigFlags(AutoHideFlags);        // DefaultAutoHideConfig
void setColorSchemeMode(ColorSchemeMode);                 // Light | Dark | FollowPalette (default)
```

- Config flags must be set **before** constructing `CDockManager`.
- `FollowPalette` switches between `default_linux.css` and `default_linux_dark.css` using
  `isApplicationPaletteDark()`. It therefore follows our explicit palette (§4), not the
  platform's colour scheme.

**Licence.** Headers say LGPL-2.1 "or (at your option) any later version", and `LICENSE` is
LGPL-2.1.
- LGPL-2.1-or-later code may be combined into a GPL-3.0 work: LGPL-2.1 §3 allows applying the GPL
  instead.
- Static linking is therefore licence-legal for us. A GPL-3 app must ship complete corresponding
  source anyway, which satisfies LGPL §6's relink requirement.
- Recommendation: keep it **shared** (the default). It costs nothing, keeps the ADS notice cleanly
  separable and matches how Qt is shipped.
- Obligations:
  - ADS licence text plus copyright in `THIRD-PARTY-NOTICES.md`
  - ADS tag and URL in `WRITTEN-OFFER.txt`
  - the "or later" wording recorded as `LGPL-2.1-or-later`

**Wayland issues.**
- #839 (Qt 6.11, Ubuntu 26.04 Wayland, 4.5.0): drag-and-drop docking broken, with the upstream
  reply "Ubuntu + Wayland is not a supported platform". It is addressed by #837/#844 in 5.0.
- #714 (floating docks on Wayland) was closed in 2026-06.
- Floating containers on Wayland cannot be positioned by the client. 5.x uses a Wayland-specific
  path; `ads::internal::isWayland()` keys off `platformName().startsWith("wayland")`.
- Under the D6 default (xcb) none of this applies.
- For the Wayland opt-in, expect floating-panel placement to be compositor-chosen. Test snapping on
  Hyprland before 0.6.

**QDockWidget as fallback.** It is adequate for v0.1's panel set: dock, tabify, float, and
`QMainWindow::saveState/restoreState` for simple "perspectives". It lacks auto-hide sidebars,
float-in-float and dock-inside-floating-window, and it has the same Wayland floating-placement
limits. Keep the panel code independent of ADS types (`QWidget* makeLayersPanel()`) so the dock
host can swap if ADS breaks.

---

## 4. Dark Fusion palette and icons

- `QStyleHints::setColorScheme(Qt::ColorScheme)` and `unsetColorScheme()` are **since Qt 6.8**, so
  they are available on 6.9.3 (https://doc.qt.io/qt-6/qstylehints.html#colorScheme-prop).
- Docs: "overriding the color scheme is not supported on all platforms".
- **[measured]** Under offscreen, `setColorScheme(Dark)` left `colorScheme()` at `Unknown`.
- Do not rely on it for the look. Recommended startup, before any widget:
  ```cpp
  QApplication::setStyle(QStyleFactory::create("Fusion"));
  QPalette p;  // build every role explicitly, all three groups (Active/Inactive/Disabled)
  p.setColor(QPalette::Window, …); p.setColor(QPalette::WindowText, …); p.setColor(QPalette::Base, …);
  p.setColor(QPalette::AlternateBase, …); p.setColor(QPalette::Text, …); p.setColor(QPalette::Button, …);
  p.setColor(QPalette::ButtonText, …); p.setColor(QPalette::Highlight, …); p.setColor(QPalette::HighlightedText, …);
  p.setColor(QPalette::ToolTipBase, …); p.setColor(QPalette::ToolTipText, …); p.setColor(QPalette::PlaceholderText, …);
  p.setColor(QPalette::Link, …); p.setColor(QPalette::Light/Midlight/Mid/Dark/Shadow, …);   // Fusion bevels use these
  p.setColor(QPalette::Disabled, QPalette::Text / ButtonText / WindowText, dimmed);
  QApplication::setPalette(p);
  QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);  // hint only
  ```
- Keep the targeted `.qss` minimal. A stylesheet on a widget turns off Fusion's palette-driven
  painting for it (QStyleSheetStyle), so style only what the palette cannot express, such as
  canvas-adjacent chrome and the tool-options bar.
- Put the palette's colour values in one header so Diagnostics, ADS (`FollowPalette`) and the icon
  tinting all read the same tokens.

**Icons.**
- QtSvg handles `QIcon(":/icons/x.svg")` through the `iconengines/libqsvgicon.so` plugin, and
  `imageformats/libqsvg.so` covers `QImage`.
- Both were deployed with `EXTRA_QT_MODULES=svg` **[measured]**. Assert them in the bundle check,
  because a missing plugin gives **blank icons with no error**.
- Tinting: author single-colour glyphs (fill or stroke `#000`), then render through `QSvgRenderer`
  into a `QImage` at `size*dpr`. Recolour with `CompositionMode_SourceIn` from the palette, and
  generate Normal/Disabled/Active states in a small `QIconEngine`.
- Originality:
  - Draw our own glyphs on a 24-px grid with 1.5-px strokes and rounded joins.
  - Do not trace or evoke Photoshop's tool glyphs. Examples to avoid: its feather-quill pen, its
    specific lasso loop, its "marching-ants square with arrow" move tool.
  - If a base set is wanted for speed, Lucide (ISC) or Tabler (MIT) are licence-compatible. Record
    them in THIRD-PARTY-NOTICES.
  - A fully original set avoids the question.

---

## 5. Packaging

### linuxdeploy-plugin-qt and the Wayland plugins

- **Source [source]** (`src/deployers/PlatformPluginsDeployer.cpp`, master 2026-08):
  - The deployer always deploys only `platforms/libqxcb.so`.
  - Anything else must be named in `EXTRA_PLATFORM_PLUGINS` (`;`-separated filenames).
  - For names starting with `libqwayland`, it also deploys `wayland-decoration-client` and
    `wayland-shell-integration`, but **not** `wayland-graphics-integration-client`.
  - That comes from `EXTRA_QT_MODULES=waylandcompositor` (`WaylandcompositorPluginsDeployer`
    deploys all three dirs). It is a misleading name, but it is the documented workaround.
- **"Silently X11-only"** is the default behaviour when `EXTRA_PLATFORM_PLUGINS` is unset. It is
  not a bug that got fixed.
- **Status with Qt 6.11, linuxdeploy continuous (2026-09-01) + plugin-qt continuous (2026-08-22)
  [measured]:**
  - `EXTRA_PLATFORM_PLUGINS=libqwayland.so EXTRA_QT_MODULES="waylandcompositor;svg"` deploys
    `platforms/libqwayland.so`, `libQt6WaylandClient.so.6`, all shell, decoration and
    graphics-integration plugins, and `xcbglintegrations`.
  - From the AppDir, `QT_QPA_PLATFORM=wayland` loaded the bundled `libqwayland.so` (LD_DEBUG
    confirmed) and reported `platform=wayland`. `xcb` also worked.
  - With the **old** name `libqwayland-generic.so` on 6.11, it fails loudly: "Cannot deploy
    non-existing library file", exit 1.
  - There is no upstream release that "fixes" the rename. The filename is simply per-Qt-version.
    The last tagged release is `1-alpha-20250213-1`; use `continuous`, pinned by SHA256.
- Related open issues:
  - linuxdeploy-plugin-qt #160 (export Wayland plugins), #200 (platform plugins deployed
    incorrectly, Qt 6.8), #213 (broken AppImage after skipping AppRun hook on Qt6)
  - qgroundcontrol #13855 (Qt 6.10 rename dropped Wayland plugins)
  - QtPass PR #1806 (working 6.x recipe with Weston smoke test)
- CI (Qt 6.9.3) recipe:
  ```bash
  export QMAKE=$QT_ROOT/bin/qmake
  export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
  export EXTRA_QT_MODULES="waylandcompositor;svg"
  linuxdeploy-x86_64.AppImage --appdir AppDir -e AppDir/usr/bin/rasterloom \
     -d rasterloom.desktop -i rasterloom.png --plugin qt \
     --exclude-library 'libglib-2.0.so*'
  ```
  For a local 6.11 build: `EXTRA_PLATFORM_PLUGINS=libqwayland.so` plus `NO_STRIP=1`.
- The plugin writes `usr/bin/qt.conf` (`Prefix=../`, `Plugins=plugins`), and `AppRun` is a symlink
  to the binary (no Qt6 hook) **[measured]**.

### Excludelist

- linuxdeploy compiles the AppImage excludelist in at build time
  (https://raw.githubusercontent.com/AppImageCommunity/pkg2appimage/master/excludelist).
- It already contains `libGL.so.1 libEGL.so.1 libGLdispatch.so.0 libGLX.so.0 libdrm.so.2
  libgbm.so.1 libxcb.so.1 libX11.so.6 libX11-xcb.so.1 libwayland-client.so.0 libfontconfig.so.1
  libfreetype.so.6 libharfbuzz.so.0`.
- **`libglib-2.0` is not on it** and got bundled **[measured]**. Add `--exclude-library`.
- `libsystemd.so.0` and `libdbus-1.so.3` were also bundled. That is harmless, but consider
  excluding both too: every desktop has them, and a stale `libsystemd` inside the bundle is a known
  source of odd issues.
- Keep the spec's post-build assertion as the enforcement point.

### Runtime and appimagetool

- **uruntime:**
  - Latest `v0.8.1` (2026-09-24); the spec pins `v0.6.1` (2026-08-22).
  - 0.8.0 is described as "a major runtime-safety and compatibility release" (authenticated
    reusable mounts, fd-bound re-exec, subreaper cleanup), and 0.8.1 fixes startup regressions
    from it.
  - Variant `uruntime-appimage-squashfs-lite-x86_64` is "mount/extract only", a static musl
    binary of about 1.2 MB.
  - Embedded config is patched in place with `sed`, and each value must stay one digit:
    ```
    sed -i 's|URUNTIME_EXTRACT=[0-9]|URUNTIME_EXTRACT=2|' uruntime-appimage-squashfs-lite-x86_64
    ```
  - **Default `URUNTIME_EXTRACT=3` extracts (when FUSE is unavailable) only if the file is ≤ 350
    MiB**, which is exactly the spec's size ceiling. Set it to `2` (FUSE first, else always
    extract). `APPIMAGE_EXTRACT_AND_RUN=1` and `--appimage-extract-and-run` are still honoured
    (docs/CONFIGURATION.md).
  - Both 0.6.1 and 0.8.1 packed the test AppDir with `appimagetool 1.9.1 --runtime-file` and ran it
    via FUSE and via extract-and-run on this machine **[measured]**. `--appimage-version` prints the
    runtime version.
  - Recommendation: keep the 0.6.1 pin for the first friend delivery (less churn), and plan a
    deliberate bump to 0.8.x behind the distro matrix.
- **appimagetool:** `AppImage/appimagetool` release `1.9.1` (2025-11-18, plus `continuous`):
  `ARCH=x86_64 appimagetool --no-appstream --runtime-file ./uruntime-… AppDir Out.AppImage`. It
  needs `mksquashfs`, which is bundled inside the tool.
- **Alternatives:**
  - **go-appimage** (`probonopd/go-appimage`, `continuous` 2026-09-03): `appimagetool -s deploy`
    bundles glibc too. It is the "any distro" path, but has its own Qt plugin handling and weaker
    control. Not recommended over linuxdeploy for Qt.
  - **sharun / quick-sharun** (`VHSgunzo/sharun` v0.8.1; `pkgforge-dev/Anylinux-AppImages`):
    - lib4bin collects the binary plus all libraries **including the dynamic linker**, and sharun
      launches through the bundled `ld-linux`. This removes the glibc floor entirely, and they
      claim musl/non-FHS hosts work.
    - They typically bundle Mesa too and carry NVIDIA-EGL special cases
      (`SHARUN_NO_NVIDIA_EGL_PRIME`).
    - This **directly conflicts** with the spec's "never bundle libGL/EGL/drm/gbm" rule and
      carries GPU-driver risk.
    - Worth revisiting only if the 7-distro matrix shows glibc-floor pain.
  - **appimage-builder** is apt-based, slow and largely unmaintained. Skip it.

### Building locally on Arch with system Qt 6.11

- Sane only as a **personal test build for this machine**. Measured on the probe AppDir:
  - highest required symbol `GLIBC_2.43` (host glibc 2.44)
  - bundled Arch ICU 78, libtiff 6 and so on
  - 91 MB AppDir for a trivial app
- It will not start on Ubuntu 22.04/24.04 or Debian 12, and it is not a friend deliverable.
- Needs:
  - `NO_STRIP=1` (DT_RELR, §0.7)
  - `EXTRA_PLATFORM_PLUGINS=libqwayland.so`
  - `--exclude-library 'libglib-2.0.so*'`
  - `QMAKE=/usr/bin/qmake6`
- linuxdeploy and plugin-qt run fine as downloaded AppImages with `APPIMAGE_EXTRACT_AND_RUN=1`, so
  nothing needs installing.
- The friend AppImage must come from the ubuntu:22.04 CI job with Qt 6.9.3.

### CI: ubuntu:22.04 container, Qt 6.9.3 via aqtinstall, GCC 11

- aqtinstall `3.3.0` is current.
- For `linux desktop 6.9.3 linux_gcc_64`, the default archives are `icu qtbase qtdeclarative qtsvg
  qttools qttranslations qtwayland`, so **qtsvg and qtwayland are base archives, not
  `-m` modules**. `qtwaylandcompositor` is the only wayland add-on module and is not needed
  **[measured]**.
- The Qt binaries are built on RHEL 8.10 against glibc 2.28, which is below 22.04's 2.35
  (https://doc.qt.io/qt-6.9/supported-platforms.html lists "Ubuntu 22.04 … GCC 11.x").
- A 6.9.3 `qtwayland`+`qtsvg` download was 7.4 MB and contained exactly the plugin set in §0.1
  **[measured]**.

```bash
apt-get update && apt-get install -y --no-install-recommends \
  build-essential g++-11 cmake ninja-build git curl ca-certificates python3-pip file patchelf \
  libgl-dev libegl-dev libglvnd-dev libfontconfig1-dev libfreetype-dev libxkbcommon-dev libxkbcommon-x11-0 \
  libxcb1-dev libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 \
  libxcb-shape0 libxcb-sync1 libxcb-xfixes0 libxcb-xkb1 libxcb-shm0 libxcb-util1 libx11-xcb1 libsm6 libice6 \
  libdbus-1-3 libwayland-client0 libwayland-cursor0 libwayland-egl1 \
  xvfb xauth squashfs-tools \
  libpng-dev libjpeg-turbo8-dev libtiff-dev zlib1g-dev
# cmake 3.22.1 from jammy satisfies ">= 3.21"
pip3 install aqtinstall==3.3.0
aqt install-qt linux desktop 6.9.3 linux_gcc_64 -O /opt/Qt \
    --archives qtbase qtsvg qtwayland icu qttranslations
export QT_ROOT=/opt/Qt/6.9.3/gcc_64 CMAKE_PREFIX_PATH=/opt/Qt/6.9.3/gcc_64
```

- `libxcb-cursor0` is mandatory for the xcb plugin since Qt 6.5
  (https://doc.qt.io/qt-6.9/linux-requirements.html). It must be installed on the build host so
  linuxdeploy bundles it; it is not on the excludelist.
- The whole `libxcb-*` extension family plus `libxkbcommon-x11` must be present at deploy time for
  the same reason.
- `--archives` restricts to the listed set. Omit it to get the full default set, including
  `qtdeclarative`, which we do not need.
- Cache `/opt/Qt` keyed on the version.
- **GCC 11 (jammy: 11.4) C++20 gaps** to police locally, where GCC 16 accepts them:
  - no `<format>` / `std::format` (GCC 13), so use `fmt` (already in the toolchain list)
  - no `std::expected` / `<print>` (C++23, banned anyway)
  - no `constexpr std::string` or `std::vector` (GCC 12)
  - early `<ranges>` with several later-fixed defects: keep ranges usage simple
  - partial `<chrono>` calendar and time zones
  - Available: `<span>`, `<bit>`/`std::bit_cast`, concepts, `<source_location>`, `std::jthread`,
    designated initialisers, `using enum`
- Local enforcement: `-std=c++20` plus a CI GCC 11 build that is authoritative.
- Qt API floor:
  - Build with `-DQT_DISABLE_DEPRECATED_UP_TO=0x060900`.
  - `QT_WARN_DEPRECATED_UP_TO` does **not** catch *newer* API.
  - The only real guard against 6.10+ API is the CI build against 6.9.3.

---

## 6. QtTest headless GUI

Verified under offscreen with `DISPLAY` and `QT_QPA_PLATFORMTHEME` unset **[measured]**:

- `QTEST_MAIN(T)` with `QT_QPA_PLATFORM=offscreen` (or `-platform offscreen`). CTest:
  ```cmake
  add_test(NAME gui-smoke COMMAND gui_smoke)
  set_tests_properties(gui-smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;RASTERLOOM_CANVAS=raster"
    ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:;QT_QPA_PLATFORMTHEME=unset:;QT_SCALE_FACTOR=unset:")
  ```
  Unsetting `DISPLAY` is what forces GL off under offscreen (§2 table).
- `QTest::qWaitForWindowExposed(&w)` returned true.
- `QTest::mouseClick(QWidget *w, Qt::MouseButton b, Qt::KeyboardModifiers m = {}, QPoint pos = QPoint(), int delay = -1)`
  and `QTest::keyClick(QWidget *w, Qt::Key key, Qt::KeyboardModifiers m = Qt::NoModifier, int delay = -1)`
  both fired `clicked` (`QSignalSpy` count checked).
- For strokes, use `QTest::mousePress/mouseMove/mouseRelease` for the mouse path, and §1A
  `QTabletEvent`s via `sendEvent` for the pen path.
- `QWidget::grab()` returns correct pixmaps. With `QT_SCALE_FACTOR=2` it returns 2× pixels with
  `devicePixelRatio()==2`. Divide by dpr when laying out.
- Minimum contact-sheet gui-smoke (ran green in 13 ms; `probe/smoke.cpp`):
  1. Build the real `MainWindow` with ADS.
  2. Load a fixture `.orp`, show, then `qWaitForWindowExposed`.
  3. Drive four steps: select brush (keyClick `B`), paint a stroke, open Image Size (then close),
     open Input Diagnostics.
  4. After each step, `window.grab()` plus grabs of the Layers and History panels.
  5. `QPainter` them into one `QImage` grid with a caption strip (step name, `platformName()`,
     canvas backend).
  6. Save `gui-smoke-contact-sheet.png` as a CI artifact.
  7. Assertions must be semantic, not "did not crash": layer count, history entry count, a canvas
     pixel under the stroke changed, and the Diagnostics labels are non-empty.
- The sheet is for human eyes and is **not** a byte-exact golden. Fonts come from fontconfig and
  differ by host.
- Theme determinism depends on the explicit palette (§4) and `QT_QPA_PLATFORMTHEME` being unset.
  With qt6ct leaking in, colours changed **[measured]**.

---

## Sources

- Qt docs: https://doc.qt.io/qt-6/qtabletevent.html ·
  https://doc.qt.io/qt-6/qpointingdevice.html · https://doc.qt.io/qt-6/qwidget.html#tabletEvent ·
  https://doc.qt.io/qt-6/qt.html#ApplicationAttribute-enum · https://doc.qt.io/qt-6/qopenglwidget.html ·
  https://doc.qt.io/qt-6/qstylehints.html#colorScheme-prop · https://doc.qt.io/qt-6/newclasses68.html ·
  https://doc.qt.io/qt-6.9/supported-platforms.html · https://doc.qt.io/qt-6.9/linux-requirements.html
- qtbase v6.9.3: `src/gui/kernel/qevent.h`, `qpointingdevice.h`, `qstylehints.h`;
  `src/plugins/platforms/xcb/qxcbconnection_xi2.cpp`, `qxcbconnection.cpp`, `qxcbintegration.cpp`
  (https://github.com/qt/qtbase/tree/v6.9.3)
- ADS: https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/releases/tag/v5.1.1 ·
  …/releases/tag/5.0.0 · issues #738, #839, #714 · PRs #837, #844
- linuxdeploy-plugin-qt: https://github.com/linuxdeploy/linuxdeploy-plugin-qt
  (`src/deployers/PlatformPluginsDeployer.cpp`, `WaylandcompositorPluginsDeployer.cpp`; issues
  #160, #200, #213) · https://github.com/mavlink/qgroundcontrol/issues/13855 ·
  https://github.com/IJHack/QtPass/pull/1806 · https://github.com/MMetze/DMHelper/pull/210
- Excludelist: https://raw.githubusercontent.com/AppImageCommunity/pkg2appimage/master/excludelist
- checkrt: https://github.com/linuxdeploy/linuxdeploy-plugin-checkrt (archived) ·
  https://github.com/darealshinji/linuxdeploy-plugin-checkrt
- uruntime: https://github.com/VHSgunzo/uruntime/releases (v0.6.1, v0.8.0, v0.8.1) ·
  https://github.com/VHSgunzo/uruntime/blob/main/docs/CONFIGURATION.md
- appimagetool: https://github.com/AppImage/appimagetool/releases/tag/1.9.1 ·
  go-appimage https://github.com/probonopd/go-appimage · sharun https://github.com/VHSgunzo/sharun ·
  https://github.com/pkgforge-dev/Anylinux-AppImages
- aqtinstall 3.3.0: https://github.com/miurahr/aqtinstall
