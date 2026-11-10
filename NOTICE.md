# Third-party notices

Forever Launcher as a whole is distributed under the GNU General Public License,
version 3 or later (see `LICENSE`). It combines the components below, which keep
their own licences. External sources are listed at the exact revision in
`ps5/release/deps.json`; repository-contained third-party sources keep
content hashes and provenance beside their code. The ZIP includes the GPL text
and component licence/copyright notices in `licenses/`; the public source
repositories and pinned source archives provide corresponding source and build
instructions.

This is an unofficial fork of bizkut's
[Forever Launcher](https://github.com/bizkut/ps5-forever); the pinned Wine, Mesa,
vkd3d-proton, DXVK, PS5_Vulkan, payload SDK and pacbrew sources are bizkut's
forks. Patches applied on top are in `ps5/*/patches/`; those taken from others
keep their authors' `From:` lines.

| Component | Licence | Used for |
| --- | --- | --- |
| [Forever Launcher](https://github.com/bizkut/ps5-forever) (bizkut) | GPL-3.0-or-later | the upstream this fork tracks: title, runtime, helpers and build glue |
| [Proton](https://github.com/ValveSoftware/Proton) (Valve) | per component, see Proton's `LICENSE` and `dist.LICENSE` | starting point of the build system and Wine tree |
| [Wine](https://www.winehq.org/), via [bizkut's fork](https://github.com/bizkut/ps5-forever-wine) | LGPL-2.1-or-later | runs Battle.net and WoW; all files below `wine-title/` except those listed below. `ps5/wine/patches/` 0001–0005 are upstream Wine/Proton fixes, 0006–0008 this fork's |
| [AddonForge](https://github.com/Topher121/AddonForge) (Topher121) 0.3.0, headless core with bizkut's patches | MIT; Rust dependencies retain their licences in `licenses/AddonForge/` and the generated runtime notice | `wine-title/addonforge/addonforge-core.exe`, add-on catalogue/install/update/remove core |
| [Forever's github-wow-addon-catalogue fork](https://github.com/bizkut/github-wow-addon-catalogue), derived from [layday's catalogue](https://github.com/layday/github-wow-addon-catalogue) | GPL-3.0 | daily refreshed and bundled `wine-title/addonforge/github-catalogue.csv` data; copyright layday, bizkut and contributors |
| [miniz](https://github.com/richgel999/miniz) 3.1.2 plus master ZIP-reader fixes (`MZ_VERSION` 11.3.2), commit `275e34491519` | MIT | bounded ZIP parsing, CRC verification and parallel extraction in `wine-title/addon-worker.elf`; also compiled into the title (`forever-miniz.c`) for a launcher profile that installs from a zip; licence in `licenses/miniz/` |
| [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) (Hans-Kristian Arntzen and contributors), via [bizkut's fork](https://github.com/bizkut/ps5-forever-vkd3d-proton) | Combined frame-generation build: GPL-3.0-or-later; underlying vkd3d-proton code retains LGPL-2.1-or-later | `d3d12.dll`, `d3d12core.dll`. `ps5/vkd3d-proton/patches/` 0001–0004 are upstream vkd3d-proton fixes, 0005 this fork's |
| [Eden](https://eden-emu.dev/) frame generation (commit `67bada77`), via [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden) | GPL-3.0-or-later | `vkd3d-proton/libs/vkd3d/wineps5_fg*` |
| [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) | GPL-3.0-or-later | the Lossless Scaling frame-generation code Eden's port is based on |
| [ps5-framegen](https://github.com/swordpdf/ps5-framegen) (swordpdf), revision `64362101757c`, from RPCS3-PS5, as vendored by PSSwanStation | GPL-3.0-or-later | FSR 3 frame generation in `vkd3d-proton/libs/vkd3d/wineps5_fsr3fg/`; still compiled into `d3d12.dll`, though the launcher no longer selects it |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.3.0 shaders | MIT | FSR 3 optical flow and interpolation shaders, some modified for RPCS3-PS5; shipped with the FSR 3 code above |
| [DXVK](https://github.com/doitsujin/dxvk) (Philip Rebohle and contributors), via [bizkut's fork](https://github.com/bizkut/ps5-forever-dxvk) | zlib | `dxgi.dll` |
| [Mesa](https://mesa3d.org/) RADV, via [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (Mihawk) and [bizkut's fork](https://github.com/bizkut/ps5-forever-mesa) | MIT (per-file notices in the Mesa source) | the Vulkan driver linked into the app. `ps5/radv/patches/` 0004–0006, 0096 and 0097 are PS5_Mesa commits `53cc83f`, `249fe49`, `f5610fe`, `7bd4aac` and part of `47e9eb6` by Mihawk; 0007–0094 are Mesa 26.2.6 stable fixes by their upstream authors; 0001–0003 are bizkut's, 0095 this fork's |
| [whisper.cpp](https://github.com/ggml-org/whisper.cpp) 1.9.5 | MIT | Voice-to-Text speech recognition, statically linked into the game parent |
| [ggml](https://github.com/ggml-org/ggml) 0.26.0, with PS5 allocation limits | MIT | whisper.cpp CPU and Vulkan inference backends |
| [OpenAI Whisper](https://github.com/openai/whisper) speech model weights | MIT | Voice-to-Text model (`ggml-small-q5_1.bin`, whisper.cpp's ggml conversion), downloaded on first use; not included in the release ZIP |
| [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (mihawk-99), via [bizkut's fork](https://github.com/bizkut/ps5-forever-vulkan) | GPL-3.0-or-later; bundled components retain their notices | Vulkan loader integration and title build tools |
| [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) (mihawk-99), from [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), via [bizkut's fork](https://github.com/bizkut/ps5-forever-sdk) | GPL-3.0-or-later; libc and toolchain components retain their notices | toolchain, libc and `sce_module/libc.prx` |
| [FreeType](https://freetype.org/) | FTL or GPL-2.0-or-later | `wine-title/lib/libfreetype.so`; this distribution is based in part on the work of the FreeType Team |
| [GnuTLS](https://www.gnutls.org/) | LGPL-2.1-or-later | `wine-title/lib/libgnutls.so` |
| [Nettle](https://www.lysator.liu.se/~nisse/nettle/) | LGPL-3.0-or-later or GPL-2.0-or-later | linked into GnuTLS |
| [libtasn1](https://www.gnu.org/software/libtasn1/) | LGPL-2.1-or-later | linked into GnuTLS |
| [pacbrew](https://github.com/OpenAGC/pacbrew-repo) recipes, via [bizkut's fork](https://github.com/bizkut/ps5-forever-pacbrew) | see the repository | build recipes for GnuTLS, Nettle and libtasn1 |
| Mozilla root certificates (`cacert.pem`) | MPL-2.0 | `wine-title/share/wine/cacert.pem`, trusted roots for HTTPS |
| Noto and Droid fonts | OFL-1.1 / Apache-2.0 | `wine-title/share/wine/fonts/`; Noto Sans menu glyph rasters in `ps5/title/menu-font.h`; notices in `ps5/release/licenses/fonts/` |
| Wine built-in fonts | LGPL-2.1-or-later; Tahoma also retains the Bitstream Vera copyright/licence | `wine-title/share/wine/fonts/`; original font notices are included in `licenses/fonts/` |
| `ps5/title/bitmap-font.h` and the message panel in `ps5/title/compositor.c`, `ps5/title/tools/make-title-background.py` and `ps5/title/tools/make-title-art.py`, from ps5-forever by Mihawk | MIT, notices kept in the files | title screen drawing, background and placeholder icon |
| `ps5/probes/psmkit-wine/title-icon.h`, from ps5-forever `examples/battlenet/icon.c` by Mihawk | MIT, notice kept in the file | the home-screen icon made from the installed Battle.net launcher |
| WoWPS5 v0.1.1 (BeLikeBrett and the WoWPS5 contributors) | MIT, notice in `licenses/WoWPS5/` | the native audio port in `ps5/probes/psmkit-wine/native-entry.c`; the VideoOut MAILBOX present mode in `ps5/radv/patches/0001` |
| [stb_image](https://github.com/nothings/stb) v2.30 (Sean Barrett) | Public domain or MIT | decoding that launcher's PNG icon |

## Not included

- **`Lossless.dll`** from [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)
  is proprietary. It is never shipped or downloaded. Users supply their own copy;
  Forever Launcher reads shader resources from it and never runs its code.
- **Build-only tools** such as ccache, which leave nothing in the app.
- **Battle.net and World of Warcraft** are downloaded by the user from Blizzard
  with Blizzard's own installer and are subject to Blizzard's terms.

World of Warcraft, Battle.net and Blizzard are trademarks of Blizzard Entertainment, Inc.
PlayStation and PS5 are trademarks of Sony Interactive Entertainment Inc.
