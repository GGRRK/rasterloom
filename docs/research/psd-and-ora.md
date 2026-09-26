# Research: PSD I/O (PhotoshopAPI) and the `.orp` / OpenRaster format

Date: 2026-09-26. Scope: `docs/BUILD-SPEC.md` `<stack>` (PSD, native format), the addendum
(GCC 16/CMake 4.4 locally, GCC 11 floor), and the PSD round-trip corpus in `<verification>`.
Everything marked **measured** was built and run on this machine (GCC 16.2.1, CMake 4.4.3,
Ninja 1.13.2, psd-tools 1.20.0 in `.venv`). Scratch work: a temporary directory
(clones `pa-v091/`, `pa-main/`, probe program `probe/probe.cpp`, block-diff script
`probe/tbdiff.py`; Krita and OpenRaster sources under `krita/` and `ora/`).

Line numbers below refer to the `v0.9.1` tag of `EmilDohne/PhotoshopAPI` (commit `055cad5`).

---

## 0. Findings that change the plan (read these first)

1. **PhotoshopAPI v0.9.1 hard-requires OpenImageIO.** The spec says "No OpenImageIO in v1".
   `CMakeLists.txt:47` runs `find_package(OpenImageIO CONFIG REQUIRED)`,
   `PhotoshopAPI/CMakeLists.txt` links `OpenImageIO::OpenImageIO` PUBLIC, and so does the
   `compressed-image` submodule (`compressed_image/CMakeLists.txt`). The OIIO headers are pulled in
   by `LayeredFile.h -> LinkedData/LinkedLayerData.h`, so every consumer compiles against OIIO.
   **Measured:** a read/write probe binary imports only **10 OIIO symbols**, all `ImageInput` /
   `IOProxy` / `ParamValue` (decoding smart-object linked files in `LinkedLayerData`). Linking the
   system OIIO 3.1 pulled 50 shared libraries (x264, x265, aom, OpenColorIO, OpenEXR, libraw...),
   which blows the AppImage size budget. Options: (a) fork patch `PSAPI_WITH_OIIO=OFF` that compiles
   out `Core/Warp/SmartObjectWarp.cpp`, the `ImageInput` decode in `LinkedLayerData.h`, and the
   `ImageBuffer.h`/`Render.h`/`Composite.h` templates, keeping the smart-object bytes as opaque
   pass-through data. (b) Accept OIIO, built minimal. Decision needed (see decisions).
2. **v0.9.1 does not build with GCC 11 (and not with GCC 12).** It uses `<format>` / `std::format`,
   which first shipped in libstdc++ with GCC 13: 6 call sites in PhotoshopAPI (`Util/Logger.h:139`
   formats a `std::chrono` time point, `Core/Struct/ImageChannel.h:269`,
   `TaggedBlocks/BlendFillTaggedBlock.h`, `SheetColorTaggedBlock.h`, `Warp/SmartObjectWarp.cpp`) and
   **71 in the `compressed-image` submodule** (`channel.h`, `image.h`, `util.h`, `blosc2/*.h`,
   `iterators/iterator.h`). `Logger.h` is included everywhere, so no translation unit compiles.
   Either the fork swaps `std::format` for `fmt::format` (fmt is already a dependency;
   `fmt/chrono.h` covers the Logger case; the change is mechanical across 77 sites), or the CI
   compiler floor moves to GCC 13 on ubuntu:22.04 (ubuntu-toolchain-r PPA; linuxdeploy-plugin-checkrt
   then ships the newer libstdc++). Decision needed.
3. **v0.9.1 writes every new layer with fill opacity 0 (invisible).** `Layer.h:424` declares
   `uint8_t m_Fill{};`. `Layer<T>::generate_tagged_blocks()` (`Layer.h:507`) always emits `iOpa`,
   so every layer authored through the API, and every layer read from a file that had no `iOpa`
   block, is written with `iOpa = 0`. Photoshop only writes `iOpa` when fill is not 100 %, so reading
   an ordinary Photoshop, GIMP or Krita PSD and saving it again **hides every layer's pixels**.
   **Measured:** `psd.composite(force=True)` of a probe-written file was fully transparent
   (`mean [255,255,255,0]`). After changing the line to `m_Fill{255u}` it rendered correctly.
   This is a third mandatory fork patch, and it is on the same code path as the #179 black-merge
   trap.
4. **Unparsed tagged blocks do not round-trip byte-identically in v0.9.1.** The corpus requires
   they do. **Measured** (injected blocks, then read and wrote through PhotoshopAPI): `vmsk`
   (vector mask) and `brst` were **dropped**. `lyid`, `shmd`, `lnsr`, `knko`, `clbl` and `infx`
   survived byte-identically, but the block order changed. Causes and patch in §1.6.
5. **PSB is selected by file extension only.** `FileHeader::write` (`PhotoshopFile/FileHeader.cpp:221-240`)
   picks version 2 only if the path ends in exactly `.psb`, and version 1 only for exactly `.psd`.
   Any other extension, including `.PSD` and a `.tmp` atomic-save temp name, logs an error that
   throws. `LayeredFile/Util/GenerateHeader.h` hard-codes `Enum::Version::Psd`.
   **Measured:** a 30001-px document written to `wide.psd` produced an invalid version-1 file that
   PhotoshopAPI then refused to read ("Width is not between 1 and 30,000"). Written to `wide.psb`,
   it round-tripped. The atomic-save temp file must end in `.psd`/`.psb`, or the fork decides the
   version from `m_Version` (with auto-promotion above 30000 px) and ignores the extension.
6. **The upstream build leaks `-O3 -mavx2` to consumers.** `PhotoshopAPI/CMakeLists.txt` uses
   `target_compile_options(PhotoshopAPI PUBLIC -O3 ... -mavx2)`. PUBLIC propagates both flags to
   every target that links PhotoshopAPI, which breaks the spec's "never a global -mavx2" rule
   (SIGILL on pre-AVX2 CPUs) and overrides our `-O2`. All AVX2 code paths are guarded by
   `#ifdef __AVX2__` (`Decompress_RLE.h:21`, `InterleavedToPlanar.h`, `EndianByteSwapArr.h`), so
   deleting the flag gives the scalar fallback. Patch the flags to PRIVATE, or remove them.
7. **The GIMP-black check in the spec can pass on a broken file.** psd-tools `composite()` returns
   the merged image only when `has_preview()` is true, and that is read from IRB 1057
   (`api/psd_image.py:466-475`). With no 1057 it assumes the merged data is valid and returns it
   (black today). With 1057 set to `hasRealMergedData=0`, it silently re-composites from the
   layers, so the check passes while GIMP and thumbnailers still see black. Strengthen the check
   (§2.3).

---

## 1. EmilDohne/PhotoshopAPI

### 1.1 Versions

- Tags: v0.1.0 … v0.9.0 (2026-04-06), **v0.9.1 (2026-05-01, latest release)**. There is no newer
  release (`gh release list`, `git ls-remote --tags`).
- `master` is 3 commits past v0.9.1 (checked 2026-09-26):
  - `51bb741` (#221, fixes issue #222): `ResourceBlock.cpp` no longer throws on ResolutionInfo
    `widthUnit=0`, which non-Adobe exporters write. **Cherry-pick this**: reading third-party PSDs
    is exactly our import path.
  - `45a1615`: simdutf submodule bump (`c00580b` = 5.3.0 → `2d4e249`, "potentially fix unicode
    corruption"). Cherry-pick it.
  - `a0fd85b`: CI only (fmt 12.2 pin, `FMT_USE_CONSTEVAL=0` for AppleClang). Not needed.
- **Recommendation:** stay on **v0.9.1 plus the 2 cherry-picks**. Do not wait for a newer
  release. Issue #179 is unfixed on master and has no PR.

### 1.2 Dependencies and how the build finds them

| Dependency | How it gets there | Required? | Used for |
|---|---|---|---|
| OpenImageIO (vcpkg pin 3.0.9.1) | `find_package(OpenImageIO CONFIG REQUIRED)` (top-level `CMakeLists.txt:47`) + `compressed-image` | **Required** | smart-object linked-file decode (`LinkedLayerData.h`), warp/render (`Core/Render/*`, `Core/Warp/SmartObjectWarp.cpp`), `compressed::image` file reading |
| Eigen3 (3.4.0) | `find_package(Eigen3 CONFIG REQUIRED)` | Required | homographies (`Core/Geometry`, warp) |
| fmt (vcpkg 11.0.2; 12.2 works) | `find_package(fmt CONFIG REQUIRED)` | Required | logging |
| libdeflate (vcpkg 1.22) | `find_package(libdeflate CONFIG REQUIRED)` | Required | ZIP / ZIP-prediction channels |
| stduuid (1.2.3) | `find_package(stduuid CONFIG REQUIRED)`; code includes `<stduuid/uuid.h>` (`Util/StringUtil.h:10`) | Required | smart-object UUIDs |
| simdutf 5.3.0 | git submodule, `add_subdirectory(thirdparty/simdutf)` | Required | UTF-8/16 |
| mio | submodule | Required | mmap reads |
| compressed-image (EmilDohne) | submodule; brings **c-blosc2** (EmilDohne fork, submodule, built static with `DEACTIVATE_ZLIB ON`) and **nlohmann_json** (submodule) | Required | in-memory blosc2-compressed channels |
| TBB (implicit) | *not declared at all* | Required at link time if `<tbb/tbb.h>` is visible | libstdc++ implements `std::execution::par*` (38 uses) with TBB when its headers are installed |
| doctest, pybind11, vcpkg | submodules | only for tests/python/vcpkg | — |

- vcpkg is on by default (`PSAPI_USE_VCPKG=ON`, which includes the submodule toolchain file and
  would build OIIO from source). Turn it off and provide system or FetchContent packages.
- `compressed-image/CMakeLists.txt` has a bug: the `else()` branch of the OIIO check also sets
  `COMPRESSED_IMAGE_HAVE_OIIO TRUE`, so "optional" OIIO is not optional there either.
- **TBB (measured):** with Arch's `onetbb` installed, the link failed with ~800 undefined
  `tbb::detail::r1::*` symbols until `TBB::tbb` was added. Pick one: link `TBB::tbb` (parallel
  RLE/ZIP, one more .so in the AppImage) or define `_GLIBCXX_USE_TBB_PAR_BACKEND=0` for the
  PhotoshopAPI target (serial, deterministic, no extra dependency). The ubuntu:22.04 container has
  no TBB headers unless `libtbb-dev` is installed, so CI would silently go serial. Pin the choice
  explicitly so local and CI behave the same.
- ubuntu:22.04 availability (for the CI container): `libdeflate-dev` 1.10 ships **no CMake config**
  (config files arrived in 1.15), so `find_package(libdeflate CONFIG)` fails. There is no stduuid
  package. `libfmt-dev` 8.1 is older than the pinned 11. `libopenimageio-dev` is 2.2.18 against a
  3.0 pin, untested. Plan on FetchContent for libdeflate ≥ 1.15, fmt 11, and the header-only
  stduuid (it must be reachable as `stduuid/uuid.h`: the upstream install puts `uuid.h` at the
  include root, which I had to work around locally). Eigen 3.4.0 from apt is fine.

CMake options: `PSAPI_BUILD_STATIC` (ON, the only target), `PSAPI_USE_VCPKG`,
`PSAPI_BUILD_TESTS`, `PSAPI_BUILD_EXAMPLES`, `PSAPI_BUILD_BENCHMARKS`, `PSAPI_BUILD_DOCS`
(default OFF), `PSAPI_BUILD_PYTHON`. All of them except STATIC default ON. Recipe used here
(**measured, builds**):

```cmake
set(PSAPI_USE_VCPKG OFF CACHE BOOL "" FORCE)
foreach(o TESTS EXAMPLES BENCHMARKS DOCS PYTHON)
  set(PSAPI_BUILD_${o} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(thirdparty/PhotoshopAPI)            # needs Eigen3, stduuid, fmt, libdeflate, OIIO findable
target_link_libraries(rasterloom_psd PRIVATE PhotoshopAPI TBB::tbb)
```

C++ standard: `set(CMAKE_CXX_STANDARD 20)` (top level, and in compressed-image). It really needs
C++20 plus `<format>`, i.e. libstdc++ 13+. Nothing C++23 was found (grep for `std::expected`,
`std::print`, `ranges::to`, `views::zip/enumerate`: none; `.contains(` is C++20).

### 1.3 GCC 16 + CMake 4.4 + Ninja (measured)

- Configure: OK. The only warning is a deprecation notice about `mio`'s `cmake_minimum_required`
  (CMake 4 removed < 3.5 compatibility; mio's minimum is above that, so it only warns).
- Build: **one error**, in `LayerInstantiations.cpp` (and every TU that instantiates `ImageLayer`):
  `/usr/include/c++/16/bits/hashtable.h:210: static assertion failed: hash function must be copy
  constructible`, raised from `ImageLayer.h:144` → `ImageDataMixins.h:709`, where
  `impl_set_image_data(std::unordered_map<Enum::ChannelIDInfo, compressed::channel<T>> data, ...)`
  omits the `Enum::ChannelIDInfoHasher` that every other map uses. Overload resolution
  instantiates the parameter type, and GCC 15+ makes a disabled `std::hash` a hard error. This is
  the same bug as upstream issue **#172** ("gcc compiler error", GCC 15 on macOS, still open).
  **Minimal patch (verified; the library then builds with zero warnings):** append to
  `PhotoshopAPI/src/Util/Enum.h`, after `PSAPI_NAMESPACE_END`:

  ```cpp
  template <>
  struct std::hash<NAMESPACE_PSAPI::Enum::ChannelIDInfo>
  {
      std::size_t operator()(const NAMESPACE_PSAPI::Enum::ChannelIDInfo& k) const noexcept
      { return NAMESPACE_PSAPI::Enum::ChannelIDInfoHasher{}(k); }
  };
  ```
  (Alternative: add the hasher template argument at `ImageDataMixins.h:709`, plus lines
  190/202/473/480/677 for consistency. Those are public signatures, so the specialisation is the
  smaller change.)
- Link of a consumer: needs `TBB::tbb` (see above). After that, the nested-tree probe below runs.
- GCC 11: **fails** (see §0.2). There is no local GCC 11 to show the exact error, but `<format>`
  is missing from libstdc++ before GCC 13.

### 1.4 API: reading and walking the tree

```cpp
#include "PhotoshopAPI.h"                                  // pulls LayeredFile, Image/Group/SmartObject
#include "LayeredFile/LayerTypes/AdjustmentLayer.h"        // not included by the umbrella header
#include "LayeredFile/LayerTypes/TextLayer/TextLayer.h"
using namespace NAMESPACE_PSAPI;                           // namespace PhotoshopAPI
auto file = LayeredFile<bpp8_t>::read("in.psd");           // bpp16_t, bpp32_t also exist
for (std::shared_ptr<Layer<bpp8_t>>& l : file.layers())    // index 0 = TOP of the stack
    if (auto g = std::dynamic_pointer_cast<GroupLayer<bpp8_t>>(l)) walk(g->layers());
LayeredFile<bpp8_t>::write(std::move(file), "out.psd");    // consumes the file
```

- **Order:** `layers()` / `GroupLayer::layers()` are **top-first**. `add_layer()` appends
  *below* the existing ones. **Measured:** the first layer added ("Background") ends up on top;
  the validator warns that a clipped layer added last "is the last layer within its scope".
- Layer types from `identify_layer_type` (`Impl/LayeredFileImpl.cpp:23-98`): no additional info
  → `ImageLayer`. `lsct` with type 1/2 → `GroupLayer` (or `ArtboardLayer` when `artb/artd/abdd` is
  present). `lsct` type 3 → `SectionDividerLayer` (internal). `TySh`+`Txt2` → `TextLayer`.
  `PlLd/plLd`+`SoLd/SoLE` → `SmartObjectLayer`. Any of the 20 adjustment keys (`SoCo GdFl PtFl brit
  levl curv expA vibA hue  hue2 blnc blwh phfl mixr clrL nvrt post thrs grdm selc`) →
  `AdjustmentLayer`. `vogk/vsms/vstk/vscg` → `ShapeLayer`. Anything else → `ImageLayer`.
- `Layer<T>` accessors (`LayerTypes/Layer.h:75-181`): `name()` (UTF-8, read from `luni` when
  present), `blendmode()` → `Enum::BlendMode`, `visible()`, `locked()`, `clipping_mask()`,
  `opacity()` / `fill()` as floats (stored as uint8; setters truncate `v*255`, which round-trips
  exactly for all k/255 under `-O2` without fast-math, but add uint8 accessors in the fork for
  safety), `width()`, `height()`, `center_x()`, `center_y()` (bounding-box centre, not the
  top-left). `GroupLayer::collapsed()`.
- Masks (`MaskDataMixin.h`): `has_mask()`, `get_mask()`, `mask_disabled()`,
  `mask_default_color()`, `mask_density()`, `mask_feather()`, `mask_relative_to_layer()`,
  `mask_position()`. **Pixel mask only** (channel −2). The "real user mask" (−3, present when a
  layer has both a vector and a pixel mask) is not modelled. Vector masks are not modelled either
  (see `vmsk` below).
- Channels: `ImageLayer::get_image_data()` → `unordered_map<int, vector<T>>` keyed by channel id
  (0..n colour, −1 alpha, −2 mask). `get_channel(id)`.
- **Blend-mode enum ↔ 4CC** (`Util/Enum.h:654-718`), exactly our 27 plus pass-through:
  `Passthrough pass`, `Normal norm`, `Dissolve diss`, `Darken dark`, `Multiply "mul "`,
  `ColorBurn idiv`, `LinearBurn lbrn`, `DarkerColor dkCl`, `Lighten lite`, `Screen scrn`,
  `ColorDodge "div "`, `LinearDodge lddg`, `LighterColor lgCl`, `Overlay over`, `SoftLight sLit`,
  `HardLight hLit`, `VividLight vLit`, `LinearLight lLit`, `PinLight pLit`, `HardMix hMix`,
  `Difference diff`, `Exclusion smud`, `Subtract fsub`, `Divide fdiv`, `Hue "hue "`,
  `Saturation "sat "`, `Color colr`, `Luminosity "lum "`.
  Convert with `Enum::getBlendMode<Enum::BlendMode, std::string>(m)`.
- **Pass-through vs normal group:** `GroupLayer` blend mode `Passthrough` vs any other mode. On
  read, the `lsct` block's blend mode overrides the record's (`Layer.h:240-247`). **Measured:**
  both round-trip.
- **Lock flags: lossy.** `ProtectedSettingTaggedBlock::read` reads only bit 31 ("lock all") of
  `lspf` (`flags & 128` on the first big-endian byte). Transparency (bit 0), composite (bit 1) and
  position (bit 2) locks are dropped. On write, `BitFlags(m_IsLocked, …)` sets the record's
  "transparency protected" bit only when the layer is fully locked. The spec's
  **lock-transparency cannot round-trip** without a fork patch: carry the full `uint32` `lspf`
  flags and write record flag bit 0 from the transparency lock.
- **Blending ranges ("Blend If") are not preserved:** `generate_blending_ranges()` always writes
  the defaults (`Layer.h:516`).
- Fill opacity: `iOpa` is parsed by `BlendFillTaggedBlock` (added for #214 in v0.9.1). Note the
  `m_Fill{}` = 0 default bug (§0.3). The block's `read()` also sets
  `m_Key = lrSheetColorSetting` (copy-paste bug; harmless, because parsed subclasses never reach the
  pass-through list).
- Names: `luni` is written with a trailing UTF-16 NUL counted in its length (deliberate,
  `UnicodeString.cpp` ~line 204). psd-tools reports names as `'Background\x00'`, so the
  structural diff must `rstrip('\0')`.
- Not written: `lyid` (layer IDs) for new layers. Kept when read (raw pass-through).
- **Adjustment layers:** not parsed. `AdjustmentLayer<T>` only exists "to allow clean
  round-tripping" (`AdjustmentLayer.h`): the adjustment key blocks travel as unparsed raw blocks
  and any pixel channels as `m_UnparsedImageData`. We can preserve them but not render or edit
  them. v1 needs its own parser for the adjustments it renders (read the raw `m_Data` of the
  `levl`/`curv`/… block).
- **Text layers:** parsed (`TySh` + `Txt2`, `LayerTypes/TextLayer/*`, new in 0.9). Round-trip
  fidelity of `EngineData` is not verified here; the corpus file will show.
- **Smart objects:** parsed (`PlLd`/`SoLd` + global `lnk2/lnkD/lnk3`). Linked data is re-emitted by
  `linked_layers()->to_photoshop()` in `GenerateLayerMaskInfo.cpp`. Decoding the embedded image
  needs OIIO. Upstream issues #220/#169: reading production PSDs with smart objects inside groups
  fails (`DecompressRLE` size mismatch, suspected OCIO-related blocks), so expect corpus failures
  there.
- Global additional layer info: unknown global blocks are kept (`layeredFile.unparsed_blocks()`,
  `GenerateLayerMaskInfo.cpp:29-99`). The GlobalLayerMaskInfo section is always written empty.
- **Image resources are not preserved:** `generate_imageresources`
  (`LayeredFile/Util/GenerateImageResources.h`) writes only ICC (1039) and ResolutionInfo (1005).
  Guides, slices, XMP, layer comps, 1024/1026 and so on are dropped on save. That is acceptable for
  v1 because the corpus only checks layer-level blocks, but document it.
- `num_channels()` bug (`LayeredFile.h:486-491`): `bool hasAlpha = false; hasAlpha &= …` is
  always false, so the header and merged image always get 3 channels (RGB) and never carry the
  composite's transparency. **Measured:** header `channels=3`. Fix to `|=` together with the #179
  patch.

### 1.5 Probe: nested tree written and read back (measured)

`probe/probe.cpp` authors Background · G1 (pass-through) › G2 (multiply, opacity 200) ›
G3 (normal) › [clip base (fill 0.5), clipped screen (clipping)], plus a hidden, locked
`masked softlight` layer with a gradient pixel mask. It writes the file, reads it back and walks
the tree. After the read, PhotoshopAPI printed the identical tree (names, 4CCs, opacity, fill,
visibility, lock, clip, mask, bounds). psd-tools read the same structure: `lsct` present on the 3
groups, `PASS_THROUGH` / `MULTIPLY` / `NORMAL`, `clipping=True`, `has_mask=True`. It also showed:
`iOpa=0` on every layer that did not set fill, the merged image entirely 0, and `IRB ids:
[1005]` only.

### 1.6 Unknown and unparsed tagged blocks (byte-identity)

Mechanism (`Core/TaggedBlocks/TaggedBlockStorage.cpp:136-265`, `TaggedBlock.cpp`):
- The 4CC is mapped to `Enum::TaggedBlockKey` via `taggedBlockMap` (`Enum.h:830-905`, ~75 keys).
  Keys **not in the map** become `TaggedBlockKey::Unknown`. They are read (the length is
  consumed), and then `readTaggedBlock` **returns nullptr without storing them**: the block is
  dropped with the log line "Unknown tagged block key … skipping". Missing from the map, and so
  dropped: **`vmsk`** (vector mask; the comment at `Enum.h:823` says "We dont support the legacy
  'vmsk'", but Photoshop still writes `vmsk` for vector masks on pixel layers), `brst`, `lmfx`,
  `tsly`, `vmgm`, `sn2P`, `lfxs`, `extn`, `CAI `, `OCIO`, `GenI`, and every future key.
- Known keys without a specialised class are kept as a raw `TaggedBlock` with `m_Data` bytes,
  passed through via `Layer::m_UnparsedBlocks` (`Layer.h:238`, exact-typeid filter in
  `TaggedBlockStorage::get_base_tagged_blocks`), e.g. `lfx2`, `lyid`, `shmd`, `clbl`, `infx`,
  `knko`, `lnsr`, `cinf`, `vogk`, all adjustment keys.
- The raw block does **not keep its 4CC**. `TaggedBlock::write` looks the enum up again and writes
  `keyStr.value()[0]`, the first of `findMultipleByValue` over an **unordered_map**. For aliased
  keys, the written 4CC is therefore arbitrary: `lsct/lsdk`, `artb/artd/abdd`, `Patt/Pat2/Pat3`,
  `lnkD/lnkE/lnk3`, `SoLd/SoLE`, `PlLd/plLd`, `Mtrn/Mt16/Mt32`, `FXid/FEid`. That changes the
  meaning of the data (e.g. `Mt16` written as `Mtrn`, `FEid` as `FXid`).
- Order: `generate_tagged_blocks()` emits the unparsed blocks first, then regenerated `luni`,
  `lspf`, `lclr`, `iOpa`. **Measured:** "ORDER DIFFERS" on the layer with injected blocks.
- Length: `read()` rounds the length up to the padding and stores the pad bytes in `m_Data`, so a
  block whose declared length was not already padded is rewritten with a larger length field.

**Required fork patch (small):** add `std::array<char,4> m_RawKey` to `TaggedBlock`, set it in
`TaggedBlockStorage::readTaggedBlock` before the key lookup, and store Unknown-key blocks as
plain `TaggedBlock` instead of returning nullptr. In `TaggedBlock::write`, emit `m_RawKey` when it
is set, keep the declared (unpadded) length, and write the padding separately. For PSB, keys in
the spec's 8-byte-length list are already handled by `isTaggedBlockSizeUint64` (`Enum.h:940`);
unknown keys use 4-byte lengths, which is correct. **Corpus comparison:** per layer, compare the
multiset of `(4CC, bytes)` for every block the app does not own. Do not compare positions, because
the regenerated blocks move. `probe/tbdiff.py` (appendix A) already does this with a raw parser
that does not depend on psd-tools decoding.

### 1.7 Issue #179 and the merged-composite patch

Status (checked 2026-09-26): **open**, no linked PR. The maintainer agrees to an opt-in parameter
(comments 2025-10-22/23). Nothing on master touches `ImageData.h`. Reproduced here: the merged
image is all zero, and psd-tools `composite()` returns black.

Write path, and exactly where the patch goes:
1. `LayeredFile<T>::write(...)` (`LayeredFile/LayeredFile.h:602-619`) →
   `layered_to_photoshop(std::move(layeredFile), filePath)` (`LayeredFile.h:765-779`), which
   builds `ImageData imageData = ImageData(layered_file.num_channels());` at **line 776**.
   Add a way to pass the composite: a `LayeredFile<T>::merged_image(std::vector<std::vector<T>>
   planes)` member consumed here, or a `write(..., const MergedImage<T>&)` overload.
2. `struct ImageData` (`PhotoshopFile/ImageData.h:71-107`): `ImageData::write(File&, const
   FileHeader&)` (**line 75**) writes compression = 1 (RLE) and then zeros. Replace it with
   per-channel data: PSD = `height×channels` `uint16` byte counts for all channels first, then
   each channel's PackBits rows. PSB = `uint32` counts. **Caution:**
   `ImageDataImpl::writeCompressedData` (lines 19-57) compresses one plane and writes the same
   bytes `numChannels` times, so it cannot be reused for real data. Call
   `CompressRLEImageDataPsd` / `CompressRLEImageDataPsb` (`Core/Compression/Compress_RLE.h`) once
   per plane. Channel order is the document order (R,G,B[,A] / C,M,Y,K[,A] / Gray[,A]).
3. `PhotoshopFile::write` (`PhotoshopFile/PhotoshopFile.cpp:49`) calls `m_ImageData.write` last.
   No change needed.
4. `num_channels()` (`LayeredFile.h:486-491`): fix `&=` → `|=`, or pass the channel count from
   the composite. `generate_header` (`LayeredFile/Util/GenerateHeader.h`) must agree with it.
5. IRBs: extend `generate_imageresources` (`LayeredFile/Util/GenerateImageResources.h`) and add
   two `ResourceBlock` subclasses next to `ResolutionInfoBlock`/`ICCProfileBlock`
   (`Core/Struct/ResourceBlock.h:39-67`, `.cpp`). The IDs are already mapped (`Enum.h:224`
   `1036 ThumbnailResource`, `Enum.h:235` `1057 VersionInfo`).
   - **1057 VersionInfo:** `uint32 version=1`, `uint8 hasRealMergedData=1`, Unicode string writer
     name, Unicode string reader name, `uint32 fileVersion=1`.
   - **1036 Thumbnail:** `uint32 format=1` (kJpegRGB), `uint32 width`, `uint32 height`,
     `uint32 widthbytes = (width*24+31)/32*4`, `uint32 totalsize = widthbytes*height`,
     `uint32 compressedsize`, `uint16 bpp=24`, `uint16 planes=1`, then JFIF bytes (≤ 160 px long
     side by convention). Encode with our libjpeg-turbo in Rasterloom code and pass the bytes in,
     so PhotoshopAPI gains no JPEG dependency.
   Reference: Adobe PSD spec, "Image Resource IDs" and "Image Data Section",
   https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/

### 1.8 CMYK inversion (where it belongs)

PhotoshopAPI does no inversion anywhere (grep: no CMYK handling outside validation). Put it at
the codec boundary, where the channel id is known, applying only to colour planes
(`channel.m_ChannelID.index` 0..3), never to −1 alpha or −2/−3 masks: 8-bit `v = 255 - v`,
16-bit `v = 65535 - v` (32-bit CMYK is rejected at `LayeredFile.h:240`).
- **Read:** `ChannelImageData::read` (`PhotoshopFile/LayerAndMaskInformation.cpp:798-930`),
  immediately after each `DecompressData<T>(...)` (lines **889 / 903 / 917**) and before the
  `channel_wrapper` is built. Condition: `header.m_ColorMode == Enum::ColorMode::CMYK`.
- **Write:** `ChannelImageData::compressData` (same file, line 678), after
  `imageChannelPtr->get_data<T>(channelDataSpan)` (~line 780) and before `CompressData(...)` at
  **line 781**. Invert a copy (the source channel stays un-inverted in memory).
- **Merged image:** in the new `ImageData::write` (§1.7), and on read in
  `LinkedData/PsdPsbReader.h:71-78` (the merged-data reader used for embedded PSD smart objects).
- Test: a CMYK document with C=0 everywhere must store `0xFF` bytes on disk, and read back as 0.

### 1.9 PSB

Reading PSB works (`8B64` blocks, 8-byte lengths, `PsdPsbReader`). Writing works when the path
ends in `.psb` (**measured**: 30001×8 written as `.psb`, header version 2, read back by
PhotoshopAPI and by psd-tools). There is no auto-promotion (§0.5). Fork patch: in
`generate_header`, choose `Psb` when `width>30000 || height>30000`, and make
`FileHeader::write` use `m_Version` instead of the extension. Our UI can still offer a `.psb`
name. PSB limit: 300000 px per axis (`FileHeader.cpp:55-78`).

### 1.10 Fork patch list (for `thirdparty/PhotoshopAPI`)

1. `Util/Enum.h`: `std::hash<ChannelIDInfo>` (GCC ≥ 15 build fix, upstream #172).
2. `LayerTypes/Layer.h:424`: `m_Fill{255u}` (fill-0 bug). Add uint8 opacity/fill accessors.
3. #179: merged composite in `ImageData`, `num_channels` fix, IRB 1036 + 1057 (§1.7).
4. CMYK negate on read/write/merged (§1.8).
5. Tagged-block pass-through: keep unknown keys and raw 4CCs (§1.6).
6. PSB promotion by size, version from `m_Version` rather than the extension (§1.9).
7. `lspf` full flags, so lock-transparency survives (§1.4).
8. CMake: `-O3 -mavx2` PUBLIC → removed or PRIVATE. Explicit TBB decision. `stduuid` include
   path. Optional `PSAPI_WITH_OIIO` (§0.1).
9. If the GCC 11 floor stays: `std::format` → `fmt::format` (77 sites, including
   compressed-image).
10. Cherry-picks `51bb741`, `45a1615`.

---

## 2. psd-tools 1.20.0 as the verifier

### 2.1 What `composite()` supports (read in `.venv/.../psd_tools/composite/*.py`)

- **Blend modes:** all 27 separable and non-separable modes are in the dispatch table
  (`composite/blend.py:~650-700`). **Dissolve is not implemented** (`blend.py:476`,
  `# TODO: Implement me!` → falls back to normal). `soft_light` is the W3C formula with
  `D(Cb)` keyed on Cb (`blend.py:169-186`, the same as our `docs/math`). Non-separable luma is
  `0.3R + 0.59G + 0.11B` (`blend.py:484`, the same as ours).
- **Groups:** pass-through vs isolated: `isolated = group.blend_mode != BlendMode.PASS_THROUGH`
  (`composite.py:615`). There is a separate pass-through source path (`_apply_passthrough_source`,
  ~`composite.py:1130`).
- **Clipping:** yes (`_apply_clip_layers`, `composite.py:1604`). `clbl` (blend clipped as group)
  is **not honoured** (`# TODO: Consider Tag.BLEND_CLIPPING_ELEMENTS`, line 1607). `infx` is not
  honoured either (line 1111).
- **Masks:** pixel mask with `disabled`, user density, and vector-mask density; vector masks are
  rendered (`_get_mask`, `composite.py:1624-1665`; `composite/vector.py`).
- **Fill opacity:** yes, as a multiplier on shape and alpha (`composite.py:1125-1150`). It is
  applied uniformly to every mode: Photoshop's special fill behaviour for its eight "special"
  modes is not modelled, so psd-tools cannot catch mutation 6 on those modes.
- **Knockout:** partially (`_read_knockout`).
- **Adjustments:** only 8 (`composite/adjustments.py:712-721`): brightness/contrast, levels,
  curves, exposure, hue/saturation, invert, posterize, threshold. It does **not** do colour
  balance, black & white, photo filter, channel mixer, gradient map, selective color, vibrance or
  color lookup.
- **Layer effects:** colour/gradient/pattern overlays and stroke only. There are no shadows,
  glows or bevels, so the `lfx2` corpus file will not match Photoshop.
- **Blending ranges:** ignored.
- Arithmetic is float32, so it is **not** a byte-exact oracle for our binary64/8-bit rules. Use a
  tolerance (e.g. max |Δ| ≤ 2/255) when comparing against it, and keep `refcomp.py` as the exact
  reference.
- **Size guard:** `api/utils.py` `check_pixel_size` raises for **any** axis > 30000 px
  (`MAX_DIMENSION_PSD`), even for PSB. **Measured:** `topil()`/`composite()` on a 30001-px PSB
  raise `ValueError`. In the PSB corpus test, set `psd_tools.api.utils.MAX_DIMENSION_PSD = 300000`
  before opening (verified to work).

### 2.2 Useful for structural diffs

`layer.kind`, `blend_mode`, `opacity`, `clipping` (`clipping_layer` is deprecated),
`has_mask()`, `has_vector_mask()`, `is_visible()`, `layer._record.tagged_blocks` (keys and
parsed data), `psd.image_resources.get_data(Resource.VERSION_INFO)`. Names need `rstrip('\0')`.
Injecting raw blocks for tests: `TaggedBlock.frombytes(b'8BIM'+key+len+data)` and `psd.save()`
(used for the §1.6 measurement).

### 2.3 Strengthen the black-image check

The spec's one-liner passes when IRB 1057 says `hasRealMergedData=0` (psd-tools then re-renders
from the layers). Use:

```python
psd = PSDImage.open(p)
vi = psd.image_resources.get_data(Resource.VERSION_INFO)
assert vi is not None and vi.has_composite                 # IRB 1057 present and true
merged = np.asarray(psd.topil())                           # the Image Data section itself
assert merged.max() > 0                                    # not black
assert np.abs(merged.astype(int) - our_render).max() <= 0  # our own composite, byte-exact
```

---

## 3. OpenRaster 0.0.6 and `.orp`

Sources: https://www.openraster.org/baseline/file-layout-spec.html ,
https://www.openraster.org/baseline/layer-stack-spec.html , the extensions pages
(`extensions/layer-selection-status.html`, `extensions/layer-edit-locking-status.html`),
`proposals/layer-alpha-preserve.html`, `proposals/png-data-requirements.html`. The version is
0.0.6 (`conf.py` in https://invent.kde.org/documentation/openraster-org). RelaxNG:
https://invent.kde.org/documentation/openraster-org/-/blob/master/openraster-standard/schema.rnc .
Krita: `plugins/impex/ora/kis_open_raster_stack_{save,load}_visitor.cpp`, `ora_converter.cpp`
(invent.kde.org/graphics/krita, master 6bc8339). GIMP: `plug-ins/python/file-openraster.py`
(gitlab.gnome.org/GNOME/gimp, master).

### 3.1 ZIP layout rules

- The ZIP may use only DEFLATED or STORED. Names are case-sensitive UTF-8 (set the UTF-8 flag for
  non-ASCII). Use Zip64 for files over 4 GB.
- **`mimetype` must be the first entry, STORED, content exactly `image/openraster`** (no newline).
  Detection magic in shared-mime-info is `PK\003\004` at 0, `mimetype` at **30** and
  `image/openraster` at **38** (`/usr/share/mime/packages/freedesktop.org.xml`). So the local
  header of `mimetype` must have **zero extra-field bytes** (no Zip64/UT extra, no data
  descriptor). With libzip, add it first with `ZIP_CM_STORE` and check the bytes in a test.
- Required files: `mimetype`, `stack.xml` (UTF-8), **`mergedimage.png`** (mandatory since 0.0.2:
  the full-canvas final render, 8 or 16 bits per channel, no frame), and
  **`Thumbnails/thumbnail.png`** (mandatory: non-interlaced, 8 bits per channel, **≤ 256×256**, as
  large as possible without upscaling, aspect preserved, no decoration, must **not** be referenced
  from any XML; the proposal says write sRGB chunks).
- `data/`: files referenced from `stack.xml` by their full path (`data/layer2.png`). "All files
  inside this directory should be referenced from somewhere." Lower-case extensions. Layer PNGs
  should be non-interlaced and should carry colour-management chunks.

### 3.2 `stack.xml` schema

- `<image version="0.0.6" w h [xres yres] [name]>`. `w`/`h` are required positive ints and the
  display is cropped to (0,0,w,h). `xres`/`yres` are optional ints ≥ 1 in PPI, default 72, and
  must come as a pair. (Krita writes `version="0.0.1"` and `xres = round(xRes*72)`.)
- Exactly one root `<stack>`, which must not carry name/opacity/visibility/composite-op/isolation.
  **The first child is the uppermost**, the same as PhotoshopAPI's top-first order.
- `<stack>` (non-root): `name`, `opacity` (float 0..1, default 1), `visibility`
  (`visible`|`hidden`), `composite-op` (only meaningful when isolated), `isolation`
  (`isolate`|`auto`, **default `isolate`**). Since 0.0.6, `x`/`y` on stacks are not allowed
  (readers ignore them).
- `<layer src=… >`: `name`, `x`, `y` (signed int offsets, default 0), `opacity`, `visibility`,
  `composite-op`. Extensions: `selected="true"`, `edit-locked="true"`, and the proposal
  `alpha-preserve="true"` (Krita, Drawpile). Krita writes `selected`/`edit-locked` on stacks too.
- The RNC schema is stricter than the prose (it has no extension attributes, and `isolation` is
  not marked optional), so no real file validates against it. Readers ignore unknown attributes
  (Krita via `QDomElement::attribute`, GIMP via ElementTree `attrib.get`).
- Rendering: W3C Compositing-1. The root stack composites isolated over an app-chosen background.
  Isolated stacks start from transparent black. For **`isolation="auto"`** the stack's
  composite-op is ignored and its opacity/visibility are "combined … e.g. by multiplication" into
  each child.

### 3.3 `composite-op` values, and mapping our 27 modes

Standard values: `svg:src-over, multiply, screen, overlay, darken, lighten, color-dodge,
color-burn, hard-light, soft-light, difference, color, luminosity, hue, saturation` (blend
functions + source-over) and `svg:plus, dst-in, dst-out, src-atop, dst-atop` (Porter-Duff
operators; dst-in/out since 0.0.4, src-atop/dst-atop since 0.0.5).

What to **write** (Krita reads `krita:<id>` as its composite-op id verbatim,
`kis_open_raster_stack_load_visitor.cpp:160-163`; IDs from `libs/pigment/KoCompositeOpRegistry.h`,
cross-checked with Krita's own PSD mapping `libs/psdutils/psd.cpp:62-140`):

| Our mode (PSD 4CC) | ORA `composite-op` | Krita | GIMP 3 import |
|---|---|---|---|
| Normal `norm` | `svg:src-over` | normal | NORMAL |
| Dissolve `diss` | `krita:dissolve` | dissolve | → Normal (lossy) |
| Darken `dark` | `svg:darken` | darken | DARKEN_ONLY |
| Multiply `mul ` | `svg:multiply` | multiply | MULTIPLY |
| Color Burn `idiv` | `svg:color-burn` | burn | BURN |
| Linear Burn `lbrn` | `krita:linear_burn` | linear_burn | → Normal |
| Darker Color `dkCl` | `krita:darker color` (the id contains a space) | darker color | → Normal |
| Lighten `lite` | `svg:lighten` | lighten | LIGHTEN_ONLY |
| Screen `scrn` | `svg:screen` | screen | SCREEN |
| Color Dodge `div ` | `svg:color-dodge` | dodge | DODGE |
| Linear Dodge `lddg` | `krita:linear_dodge` (not `svg:plus`: that is the Porter-Duff *plus* operator and differs at partial alpha) | linear_dodge | → Normal |
| Lighter Color `lgCl` | `krita:lighter color` | lighter color | → Normal |
| Overlay `over` | `svg:overlay` | overlay | OVERLAY |
| Soft Light `sLit` | `svg:soft-light` (W3C = our math; Krita maps it to `soft_light_svg`, while Krita's PSD `sLit` is the Photoshop variant `soft_light`) | soft_light_svg | SOFTLIGHT |
| Hard Light `hLit` | `svg:hard-light` | hard_light | HARDLIGHT |
| Vivid Light `vLit` | `krita:vivid_light` | vivid_light | → Normal |
| Linear Light `lLit` | `krita:linear light` | linear light | → Normal |
| Pin Light `pLit` | `krita:pin_light` | pin_light | → Normal |
| Hard Mix `hMix` | `krita:hard_mix_photoshop` | hard_mix_photoshop | → Normal |
| Difference `diff` | `svg:difference` | diff | DIFFERENCE |
| Exclusion `smud` | `krita:exclusion` (Krita comments out `svg:exclusion` as "not in the official list") | exclusion | → Normal |
| Subtract `fsub` | `krita:subtract` | subtract | → Normal |
| Divide `fdiv` | `krita:divide` | divide | → Normal |
| Hue `hue ` | `svg:hue` | hue | HSV_HUE (not W3C) |
| Saturation `sat ` | `svg:saturation` | saturation | HSV_SATURATION |
| Color `colr` | `svg:color` | color | HSL_COLOR |
| Luminosity `lum ` | `svg:luminosity` | luminize | HSV_VALUE |
| Pass Through (groups) | `<stack isolation="auto">`, no composite-op | passThroughMode | ignored: GIMP ignores `isolation`, so the group becomes Normal |

That is 15 `svg:` modes and 12 `krita:` modes. GIMP's writer emits `svg:src-over` for every
other GIMP mode, including pass-through (`file-openraster.py:88-122`), so importing a GIMP ORA
never yields those modes.

**Reading foreign ORA** (our importer): accept every value above. Map `svg:plus` → Linear Dodge
(approximate), `krita:soft_light` → Soft Light (approximate), `krita:hard mix` → Hard Mix
(approximate), `krita:add` → Linear Dodge. `svg:src-atop` or `alpha-preserve="true"` means
"inherit alpha" (Krita). We have no such mode: import as a clipping mask only when the layer is
inside an isolated stack directly above its base (§3.5), otherwise use Normal and warn.
`svg:dst-in/out/dst-atop` and unknown values → Normal with a warning. **Never write `<filter>`
elements:** Krita's loader dereferences a null filter for any `type` that is not a known
`applications:krita:*` filter (`kis_open_raster_stack_load_visitor.cpp` loadGroupLayer, `filter`
branch: `f->factoryConfiguration(...)` with no null check). Adjustment layers go into
`document.json` only.

### 3.4 Masks and fill

ORA has **no masks and no fill opacity**. Krita writes `layer->projection()`, meaning pixels with
masks already applied (`kis_open_raster_stack_save_visitor.cpp` `saveLayer`), and imports plain
paint layers. GIMP stores the drawable pixels only (whether GIMP bakes the mask was not verified
here).

### 3.5 How `document.json` should extend ORA without breaking readers

1. **`stack.xml` stays a truthful viewing-baseline description.** For each layer, `src` points to
   pixels that render correctly in Krita. If the layer has no mask, fill = 1 and no clipping, use
   the raw layer PNG (no duplication). Otherwise use a **baked** PNG (mask applied, fill folded
   into alpha: exact for all modes except Photoshop's special-eight fill semantics).
2. **Clipping groups:** emit `<stack isolation="isolate" composite-op=base.mode
   opacity=base.opacity>` containing the base (at `svg:src-over`, opacity 1) and the clipped
   layers. A Normal clipped layer gets `svg:src-atop`; any other mode gets the mode plus
   `alpha-preserve="true"`. Inside an isolated stack whose backdrop is only the base, src-atop
   clips to the base's alpha, and the stack's mode/opacity reproduce `clbl=1`, which is
   Photoshop's default and D4's rule. GIMP ignores `alpha-preserve`, so non-Normal clipped layers
   will bleed in GIMP. That is acceptable for a viewing-only degradation.
3. **Pass-through groups:** `isolation="auto"`. ORA multiplies the stack's opacity into each child.
   If our pass-through opacity or mask semantics differ, foreign readers see an approximation, and
   `document.json` holds the truth.
4. **Private data** lives outside `data/`, so the "everything in data/ is referenced" rule holds:
   `document.json` at the archive root, plus a directory such as `rasterloom/` for unbaked layer
   sources (`rasterloom/layers/<uuid>.png`), masks (`rasterloom/masks/<uuid>.png`, 8-bit gray) and
   selections. Krita and GIMP ignore unreferenced entries (they read only `stack.xml`, the PNGs it
   references and the thumbnail). Put the stack.xml↔JSON link in a **namespaced attribute** on
   each `layer`/`stack`, e.g. `xmlns:rl="https://rasterloom.invalid/ns/1"` and `rl:id="<uuid>"`.
   QDom `attribute("name")` and ElementTree `attrib.get('name')` are unaffected by it.
5. **Foreign-edit detection:** `document.json` stores `"stack_sha256"` for the exact `stack.xml`
   bytes it was written with. On open, if the hash matches, `document.json` is authoritative
   (masks, fill, clipping, lock-transparency, adjustment layers, per-layer blend 4CC, pass-through
   semantics). If it does not match, another tool changed the stack: import from `stack.xml`
   generically and warn. (Krita and GIMP re-save without our files anyway; this catches in-place
   scripted edits.)
6. Write `version="0.0.6"`. Write `selected` for the active layer and `edit-locked` for full
   locks. Lock-transparency has no ORA attribute; the extension page reserves the `*-locked`
   suffix, so `rl:alpha-locked` or JSON are both acceptable.
7. **Extension `.orp` vs `.ora`:** GIMP 3's plug-in registers extension `ora` and MIME
   `image/openraster` with **no magic** (`file-openraster.py:513-523`). Opening `foo.orp` in GIMP
   therefore depends on MIME sniffing, which is unverified. Krita resolves via QMimeDatabase, which
   uses the shared-mime-info magic above, so it should work when the header bytes are exact.
   Verify this before claiming "GIMP/Krita/MyPaint can open it" for `.orp`, or offer "Export as
   .ora" (the same bytes).

---

## Appendix A: per-layer tagged-block byte diff (from `probe/tbdiff.py`)

Parses layer records without psd-tools decoding and reports, per layer, blocks that were
DROPPED, added or CHANGED, and whether the order of the shared keys changed.

```python
import struct, sys
BIG = (b'LMsk',b'Lr16',b'Lr32',b'Layr',b'Mt16',b'Mt32',b'Mtrn',b'Alph',b'FMsk',b'lnk2',b'FEid',b'FXid',b'PxSD',b'cinf')
def layers(path):
    d = open(path, 'rb').read(); psb = struct.unpack('>H', d[4:6])[0] == 2; L = 8 if psb else 4
    o = 26; o += 4 + struct.unpack('>I', d[o:o+4])[0]; o += 4 + struct.unpack('>I', d[o:o+4])[0]
    o += 2 * L; n = abs(struct.unpack('>h', d[o:o+2])[0]); o += 2; out = []
    for _ in range(n):
        o += 16; nc = struct.unpack('>H', d[o:o+2])[0]; o += 2 + nc * (2 + L)
        o += 4; bm = d[o:o+4]; o += 4; op, clip, flags = d[o], d[o+1], d[o+2]; o += 4
        extra = struct.unpack('>I', d[o:o+4])[0]; o += 4; end = o + extra
        o += 4 + struct.unpack('>I', d[o:o+4])[0]; o += 4 + struct.unpack('>I', d[o:o+4])[0]
        nl = d[o]; name = d[o+1:o+1+nl]; o += (1 + nl + 3) & ~3; blocks = []
        while o + 12 <= end:
            key = d[o+4:o+8]; big = psb and key in BIG
            ln = struct.unpack('>Q' if big else '>I', d[o+8:o+16 if big else o+12])[0]
            o += 16 if big else 12; blocks.append((key.decode('latin1'), d[o:o+ln])); o += ln
        o = end; out.append((name.decode('latin1'), bm.decode(), op, clip, flags, blocks))
    return out
```

(The full script, with the comparison loop, is in the scratch dir. Copy it into `tests/tools/`
when the corpus harness is built.)
