# Forever Launcher (fork)

Play **World of Warcraft** through **Battle.net** on a jailbroken **PS5**.

> [!IMPORTANT]
> **This is an unofficial fork** of [bizkut's Forever Launcher](https://github.com/bizkut/ps5-forever).
> It tracks upstream and adds fixes: a safer write helper, current ZIP and font
> parsers, a sturdier start-up and reproducible builds. It is **work in progress
> and may be unstable**. For the stable release, use upstream.
>
> - Versions read `01.UUU.FFF`: based on upstream `01.000.UUU`, fork build `FFF`.
> - It replaces an upstream install (same app ID `PPSA99030`, same
>   `/data/forever`). To go back to upstream, delete this app first; your data
>   stays.

> [!WARNING]
> **Alpha software.** Expect bugs. Forever Launcher is not made, approved or
> supported by Blizzard Entertainment, Sony Interactive Entertainment or Valve.
> Using unofficial software with an online game is at your own risk.

Forever Launcher runs Battle.net and the Windows version of WoW through Wine,
with DirectX 12 on the PS5's own GPU. Choose the WoW version in Battle.net.
Retail, Classic, Classic Era, Anniversary and the Forever beta were tested on a
console; PTR and other channels are detected the same way but untested. Other
Battle.net games are not supported.

## What's new in 01.024.005

Based on upstream [01.000.024](https://github.com/bizkut/ps5-forever/releases/tag/01.000.024).
Work in progress, tested on one console (firmware 13.40).

- **Much faster installs and patching:** the console throttles an app's file
  creates, renames and deletes (each one can stall for up to a second). Once
  one of them takes 200 ms, Wine hands them to the write broker, a helper that
  runs outside the app and isn't throttled. The broker only works below
  `/data/forever` and checks each file by inode and type. If it refuses, the
  app does the operation itself, so nothing is reported deleted that still
  exists.
- **Clean exit:** the app now always closes through the system's
  `sceSystemServiceLoadExec("exit")`. Before, a launcher that ended with
  nothing left to do called libc `exit()`, which the PS5 traps in an app's
  main process. That made the app report a crash.
- **Frame rate cap** in the Forever menu: Off, 30, 60 or 120 fps for every
  game. It sets vkd3d-proton's `VKD3D_FRAME_RATE` and DXVK's `maxFrameRate`,
  so it works for DirectX 12, 11 and 9 games. It applies at the next game
  start.
- **Output resolution** in the Forever menu: Auto (4K), 1440p, 1080p, or 16:10
  1600p, 1200p and 800p. The 16:9 sizes use the PS5's own scaler. A 16:10 size
  is scaled into a centred 3456×2160 area of a 4K output, with black side
  bars. The cursor stays inside that area and clicks land where they should.
  WoW's render scale follows the size, so the 3D view stays at about 1080
  lines.
- **FSR 3 frame generation removed** because its optical-flow-only mode made
  moving characters see-through on camera pans. Frame generation now runs
  only through Lossless Scaling (`method=lossless`, never falling back to
  FSR 3). It needs your own `Lossless.dll` and is untested.
- **USB keyboards** now open the way 13.x firmware expects: zeroed parameters
  first, with the old 5.50 call as a fallback. A Logi Bolt receiver also needs
  a firmware update before the PS5 reads it;
  [FelixStopa/BoltUpdateTool](https://github.com/FelixStopa/BoltUpdateTool)
  worked well for me, but YMMV.
- **New defaults:** 60 fps cap, frame generation off, 4K output rendered at
  1080p.
- **GPU and Wine fixes** carried as patches on the pinned sources:
  - RADV:
    - a GPU buffer is freed only after the GPU has finished with it;
    - a damaged pipeline cache is rebuilt instead of crashing every start;
    - a failed vblank wait no longer locks in 120 Hz (from Mihawk's
      PS5_Mesa);
    - Mesa 26.2.6 stable fixes.
  - vkd3d-proton: upstream present, barrier, queue-wait and RDNA2 transfer
    fixes.
  - Wine: five Proton fixes for wait/wake races, critical sections,
    thread-pool timeouts, server start-up and raw-input rescans.
- **Groundwork for other launchers:** memory reservations of 16 GiB and more
  now succeed, at a free fixed address (Chromium-based launchers ask for
  32 GiB). The launcher, its installer and a checksum-verified zip install
  come from the build profile.
- **Install note** for FTP copies to the internal drive: files created as
  `rw-rw-rw-` stop the app starting with error `0x80aa001a`. Set them to 755.

Known issue:

- **HDR is work in progress.** With it on, WoW's colours are wrong; turn it
  off in the Forever menu.

Fork releases are on the [releases page](https://github.com/RefNull/ps5-forever/releases).

## What you need

- A **jailbroken PS5** that runs homebrew apps. Tested on firmware 5.50 and,
  for this release, 13.40.
- **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**, in a
  version that lets apps use `/data`.
- **Free space** for the WoW versions you install: Classic needs about
  50–55 GB; Retail is much larger.
- A **Battle.net account** with the WoW version you want and an
  **authenticator** enabled.
- A **DualSense**. A USB keyboard and mouse also work.
- An **FTP server** on the console, to copy the app and, if you want them,
  `Lossless.dll` or a WoW install from a PC.

## Install

1. Download `PPSA99030.zip` from the [latest release](https://github.com/RefNull/ps5-forever/releases/latest).
2. Unzip it and copy the `PPSA99030` folder to a location ShadowMountPlus
   scans, such as `/data/homebrew/PPSA99030`. An external drive works too; your
   games and settings always live in `/data/forever`.
3. **Copying to the internal drive by FTP:** the files must be executable. If
   your FTP server creates them as `rw-rw-rw-`, the app fails to start with
   error `0x80aa001a`. Set them to 755, for example with your FTP client's
   chmod.
4. Let ShadowMountPlus register the app, then start **Forever Launcher** from
   the home screen.

To update, close the app and replace only the `PPSA99030` folder. Do not delete
`/data/forever`. The first start after an update checks the app's Windows files
(about 15–20 seconds).

## First start

1. The first start sets up the Windows files. This takes a few minutes; the
   app restarts by itself when it is done.
2. Forever Launcher downloads Blizzard's Battle.net installer and installs
   Battle.net. Wait for it.
3. Sign in to Battle.net with your **email address and password**, and choose a
   **code by email** when asked to confirm. Your account needs an authenticator.
   Google, Apple, Facebook, Xbox, PlayStation and other third-party sign-ins
   are not supported.
4. Choose your WoW version, press **Install**, wait for the download, then
   press **Play**.

Battle.net keeps its own sign-in token in your data folder, so you stay signed
in. Forever Launcher does not capture credentials.

### Copy WoW from a PC instead

1. Start Forever Launcher once, install Battle.net, then close the app.
2. With an FTP program, copy your PC's whole `World of Warcraft` folder
   (usually `C:\Program Files (x86)\World of Warcraft`; a Mac install works
   too), including the hidden `.build.info` and `.product.db`, to:

   ```text
   /data/forever/prefix/drive_c/Program Files (x86)/World of Warcraft
   ```

3. Start Forever Launcher and select the game. If Battle.net shows
   **Install**, use **Locate the game** and choose
   `C:\Program Files (x86)\World of Warcraft`. Battle.net then downloads only
   what is missing or newer.

The app fixes the permissions of copied folders at start-up.

## Controls

| Press | Does |
| --- | --- |
| **Options**, or **F12** on a USB keyboard | Opens or closes the Forever menu over Battle.net |
| **L2** | Escape in Battle.net (closes a dialog or dropdown) |
| **L3 + R3** | Opens the on-screen keyboard |
| Hold the touchpad still (about 0.4 s) | Voice-to-Text into WoW chat |

WoW starts with its own gamepad mode on. In Battle.net, the mouse cursor hides
after three seconds without movement.

**Voice-to-Text** (turn it on in the Forever menu): hold the touchpad and speak
into the DualSense microphone; a **Listening** panel shows while it records.
With the text in the chat box, **Cross** sends, **Circle** cancels, **Square**
opens the on-screen keyboard to edit, and **L1/R1** cycle the channel through
**/s /p /i /g /raid /r**. A muted microphone types nothing.

## Forever menu

Open it over Battle.net with **Options** or **F12**; each start shows a
ten-second hint. Use the D-pad and Cross, or a mouse. Circle or Options at the
top level closes it.

| Row | Does |
| --- | --- |
| **Frame generation** | Off or On (2x), with your own `Lossless.dll`; otherwise it shows **Needs Lossless Scaling**. See [Frame generation](#frame-generation). |
| **HDR** | On or Off. On by default; see the known issue above. |
| **Frame rate cap** | Off (the game decides), 30, 60 or 120 fps (needs a 120 Hz TV). A game's own lower cap still applies. |
| **Output resolution** | Auto (3840x2160), 1440p, 1080p, or 16:10 at 1600p (2560x1600), 1200p (1920x1200) or 800p (1280x800), centred with black side bars. The Battle.net desktop stays 1920x1080. |
| **Performance overlay** | On or Off. |
| **Invert mouse scroll** | USB mouse wheel only; on by default. |
| **Voice-to-Text** | On or Off. First use downloads a 190 MB speech model. |
| **Add-ons...** | See [Add-ons](#add-ons). |
| **Reinstall Battle.net...** | Removes Battle.net and installs it again from a fresh installer download. |
| **Uninstall Battle.net...** | Removes Battle.net, keeping installed games and their settings. Start the app again to reinstall it. |
| **Close** / **Exit Battle.net** | Closes the menu / closes Battle.net and the app. |

Frame generation and the overlay change while the game runs. HDR, the cap and
the resolution apply the next time a game starts. The cap and the resolution
are `fps=` and `res=` in `C:\wineps5-display.txt`.

### Add-ons

Open **Add-ons** before pressing Play. L1/R1 switch between your installed WoW
versions; Left/Right switch Installed, Updates, Find and Starter packs. Cross
opens or installs, Square updates the selected add-on, Triangle updates all,
and R3 removes it after a confirmation. Changes load the next time WoW starts,
or after `/reload`.

- **Find** searches [Forever's add-on catalogue](https://github.com/bizkut/github-wow-addon-catalogue)
  (refreshed daily, checked at most once a day; offline, the last copy or the
  bundled one is used) plus compatible WoWInterface and Tukui entries. Type on
  a USB keyboard or choose **Search…**. **Add from GitHub link…** takes
  `owner/repo` or a GitHub URL. CurseForge-only add-ons cannot be installed.
- Add-ons are matched to each WoW version and go into its own
  `Interface/AddOns` folder. Retail (with PTR and beta), Classic MoP,
  Anniversary, Classic Era and Forever are covered.
- **Wago** add-ons need a key that Wago gives its
  [Patreon supporters](https://addons.wago.io/patreon) (not the API key on
  Wago's account page). Choose **Wago key** on the Add-ons page and type it,
  or save it as the first line of `C:\Forever\Addons\wago-key.txt` and choose
  **Read from file** (the file is deleted after import). **Remove key** clears
  it.

If Battle.net needs another web renderer process, Add-ons pauses; close and
reopen it to resume.

## Frame generation

Frame generation draws extra frames between WoW's, for smoother motion at up to
120 Hz. It is **off by default** and needs **your own `Lossless.dll`** from
[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)
(version 3.2.2.0, or a build with the same shaders). The file is never
included or downloaded.

1. Find `Lossless.dll` in your Lossless Scaling install folder on your PC.
2. Copy it by FTP to `/data/forever/prefix/drive_c/Lossless.dll`.
3. Turn **Frame generation** on in the Forever menu and restart WoW.

Settings are in `/data/forever/prefix/drive_c/wineps5-fg.txt` and apply within
a second:

```text
fg=off mult=2 flow=1.0 method=lossless
```

| Option | Values |
| --- | --- |
| `fg` | `on`, `off`, or `auto` (on only while WoW renders below 70 fps); the menu switches `auto` to off |
| `mult` | `2`, `3` or `4` frames shown for every frame WoW draws |
| `flow` | `0.25` to `1.0`: lower is cheaper, `1.0` looks best |
| `method` | `lossless`; the launcher sets it at every start |

The overlay's settings are in `/data/forever/prefix/drive_c/wineps5-hud.txt`
(`hud=on` or `off`, `pos=tl`, `scale=2`).

**VRR:** set VRR to **Automatic** in the PS5's screen settings and use an
HDMI 2.1 VRR display. WoW asks for 120 Hz and falls back to 59.94 Hz.

## WoW settings

Each installed WoW version without a `WTF/Config.wtf` gets a preset: 4K output
at 1080p render scale, a 60 fps cap, HDR on and the controller enabled.
Existing settings are not overwritten. The HDR row changes only `HDROutput`.
The output resolution keeps 3D at about 1080 lines through `RenderScale`
(0.5 at Auto, 0.75 at 1440p, 1.0 at 1080p, 0.675 at 1600p, 0.9 at 1200p, 1.0
at 800p), but only while it still holds the previous resolution's value.

Automatic detection covers `C:\Program Files (x86)\World of Warcraft`. Other
C: or W: installs start from Battle.net's Play, without the preset, add-ons or
direct start.

**Start one version directly:** put its folder name (for example
`_classic_era_` or `_retail_`) in `/data/homebrew/PPSA99030/wine/direct-game`
and start the app. The file is used once. If that version is missing, the log
says why and Battle.net opens. An empty file prefers Classic beta, then
Forever, then another complete install.

## Your data

Everything that is yours is in **`/data/forever`**: Battle.net, WoW, settings,
`Lossless.dll` and the shader cache. The app folder holds only the app. To
uninstall, delete the app, then `/data/forever`.

Treat `/data/forever` as sensitive. Do not share registry files, browser
profiles, account files or saved credentials, and check logs and screenshots
before sharing them. Release logs keep only input-health counts, never what you
type.

## Troubleshooting

- **"Forever Launcher can't write to ..."**: the app repairs folder
  permissions under `/data/forever` and retries once. If the message stays,
  close the app and set `/data/forever` and everything in it to `777` by FTP.
- **"Forever Launcher cannot use /data" (error 2 or 30)**: update
  ShadowMountPlus to a version that lets apps use `/data`.
- **Sign-in fails**: use the email code and enable an authenticator.
- **Battle.net sees only about 17 GB free**: the write helper is not
  answering; restart the app. The kernel log shows `source=broker` (real) or
  `source=synthetic` (fallback).
- **Frame generation does nothing**: check that `wineps5-fg.txt` says `fg=on`
  and that the file is exactly `/data/forever/prefix/drive_c/Lossless.dll`. The
  performance overlay shows `FG LOSSLESS` when it works.
- **Drives**: only `C:` and `W:` work inside Wine.
- **Logs**: the console's kernel log (port 3232). Lines starting with
  `forever:` describe setup.

## Building

Releases are built by [`ps5/release/build-linux.sh`](ps5/release/build-linux.sh)
from the sources pinned in [`ps5/release/deps.json`](ps5/release/deps.json).
See [BUILDING.md](BUILDING.md).

## Credits

Thank you to everyone whose work this builds on. Licences and details are in
[NOTICE.md](NOTICE.md).

- [Forever Launcher](https://github.com/bizkut/ps5-forever) by bizkut: the
  upstream, and its forks of Wine, Mesa, vkd3d-proton, DXVK, PS5_Vulkan, the
  payload SDK and the pacbrew recipes.
- Mihawk ([mihawk-99](https://github.com/mihawk-99)): [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa),
  whose RADV patches this fork carries, [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan),
  [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK), and the earlier
  ps5-forever code behind the title font, artwork and icon.
- WoWPS5 by BeLikeBrett and the WoWPS5 contributors: the native audio port and
  VideoOut MAILBOX code, and the input handling this launcher follows.
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) by John Törnblom.
- [Wine](https://www.winehq.org/) and [Proton](https://github.com/ValveSoftware/Proton) (Valve).
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) by Hans-Kristian Arntzen and contributors.
- [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors.
- [Mesa](https://mesa3d.org/) and its RADV Vulkan driver.
- [Eden](https://eden-emu.dev/), [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden)
  and [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) by PancakeTAS: the
  Lossless Scaling frame-generation port.
- [ps5-framegen](https://github.com/swordpdf/ps5-framegen) by swordpdf and AMD
  [FidelityFX](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK):
  FSR 3 frame generation, still built into vkd3d-proton.
- [AddonForge](https://github.com/Topher121/AddonForge) by Topher121.
- [github-wow-addon-catalogue](https://github.com/layday/github-wow-addon-catalogue)
  by layday, the base of Forever's add-on catalogue.
- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) and
  [ggml](https://github.com/ggml-org/ggml), with OpenAI's
  [Whisper](https://github.com/openai/whisper) model.
- [miniz](https://github.com/richgel999/miniz), [FreeType](https://freetype.org/),
  [GnuTLS](https://www.gnutls.org/) with [Nettle](https://www.lysator.liu.se/~nisse/nettle/)
  and [libtasn1](https://www.gnu.org/software/libtasn1/), and
  [stb_image](https://github.com/nothings/stb).
- Noto and Droid fonts, and Mozilla's root certificates.

## Licence

Forever Launcher is licensed under the [GNU General Public License v3.0 or later](LICENSE).
Third-party components keep their own licences; see [NOTICE.md](NOTICE.md).

World of Warcraft, Battle.net and Blizzard are trademarks of Blizzard Entertainment, Inc.
PlayStation and PS5 are trademarks of Sony Interactive Entertainment Inc. All other
trademarks belong to their owners.
