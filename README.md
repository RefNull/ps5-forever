# Forever Launcher (fork)

Run **Battle.net** on a jailbroken **PlayStation 5**.

> [!IMPORTANT]
> **This is an unofficial fork** of [bizkut's Forever Launcher](https://github.com/bizkut/ps5-forever).
> It tracks upstream and adds fixes and optimisations that are **work in progress
> and may be unstable**. For the stable release, use upstream.
>
> **Why this fork exists:** mainly to close security and reliability gaps in
> upstream, such as a write-helper flaw that let a link under `/data/forever`
> redirect writes elsewhere, outdated ZIP and font parsers, and a fragile helper
> start-up. Along the way it removes dead code and makes builds reproducible.
> Graphics performance is currently identical to upstream; the next focus is
> improvements from a newer vkd3d-proton.
>
> - Versions read `01.UUU.FFF`: based on upstream `01.000.UUU`, fork build `FFF`.
> - It replaces an upstream install (same app ID, same `/data/forever`). To go
>   back to upstream, delete this app first; your data stays.

Forever Launcher (`PPSA99030`) runs Battle.net and World of Warcraft through
Wine, using DirectX 12 on the PS5's own GPU. Choose the WoW version you want in
Battle.net: Retail, Classic, Classic Era, Anniversary, Forever, beta or PTR.
The launcher detects channel folders rather than keeping a list of versions.
The supplied settings use 4K output at 1080p render scale, HDR and a 60 fps cap;
frame generation is off until you turn it on.

Retail, Classic, Classic Era, Anniversary and the Forever beta start from
Battle.net's **Play** button and were tested on a console. Other channels, such
as PTR, are detected the same way but untested; detection does not guarantee
that a game build renders or works online.

> [!NOTE]
> This launcher is for World of Warcraft. Other Battle.net games are not
> supported. A USB keyboard and mouse work in Battle.net and WoW.

> [!WARNING]
> **Alpha software.** Expect bugs. Forever Launcher is not made, approved or
> supported by Blizzard Entertainment, Sony Interactive Entertainment or Valve.
> Using unofficial software with an online game is at your own risk.

## What's new in 01.024.005

Based on upstream [01.000.024](https://github.com/bizkut/ps5-forever/releases/tag/01.000.024).
Tested on one console (firmware 13.40).

- **Faster installs and patching:** when the console starts stalling file
  creates, renames and deletes, the write broker does them outside the app.
- **Clean exit:** the app no longer reports a crash when its program ends.
- **Frame rate cap** in the Forever menu: Off, 30, 60 or 120 fps for every game.
- **Output resolution** in the Forever menu: Auto (4K), 1440p, 1080p, or 16:10
  1600p, 1200p and 800p, centred with black side bars.
- **FSR 3 frame generation removed;** Lossless Scaling only.
- **USB keyboards** open the way current firmware expects. A Logi Bolt receiver
  needs its firmware updated before the PS5 reads it.
- **New defaults:** 60 fps cap, frame generation off, 4K output rendered at
  1080p.
- **GPU and Wine fixes** carried as patches on the pinned sources: a GPU
  buffer is no longer freed while the GPU may still use it, no crash from a
  stale pipeline cache, Mesa 26.2.6 stable fixes for RADV, upstream
  vkd3d-proton present and barrier fixes, and Proton fixes for waits, locks
  and raw input.
- **Groundwork for other launchers:** very large memory reservations succeed,
  and the launcher and installer (including a verified zip) come from the
  build profile.

Known issues:

- **HDR is work in progress.** With HDR on, WoW's colours are wrong. Turn HDR
  off in the Forever menu.

Fork releases are on the [releases page](https://github.com/RefNull/ps5-forever/releases).

## What you need

- A **jailbroken PS5** that can run homebrew apps. Tested on firmware 5.50.
- **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**, in a version that lets
  apps use `/data`. It installs the app; your games and settings live in `/data`.
- Enough **free space** on the console for the WoW versions you install: Classic
  versions need about 50–55 GB; Retail is much larger.
- A **Battle.net account** with access to the WoW version you want, with an
  **authenticator** enabled (see [Signing in to Battle.net](#signing-in-to-battlenet)).
- A **DualSense** controller. A USB keyboard also works.
- An **FTP server** on the console, only to add your own `Lossless.dll` for
  frame generation or change settings files by hand.

## Install

1. Download `PPSA99030.zip` from the [latest release](https://github.com/RefNull/ps5-forever/releases/latest).
2. Unzip it. Copy the `PPSA99030` folder to a location ShadowMountPlus scans,
   such as `/data/homebrew/PPSA99030`. The app folder can also be on an external drive;
   games and settings remain in `/data/forever`.
3. Let ShadowMountPlus register it, then start **Forever Launcher** from the home screen.

## First start

1. The first start sets up the Windows files. This takes a few minutes. The app
   restarts by itself when it is done.
2. Forever Launcher downloads Blizzard's Battle.net installer and automatically
   continues past its fixed install-location page. Wait while Battle.net installs.
3. Sign in to Battle.net (see below).
4. In Battle.net, choose the World of Warcraft version you want, press
   **Install** and wait for the download, then press **Play**.

### Faster: copy WoW from a PC

If WoW is already installed on a PC, you can copy it to the console over your
home network instead of downloading it again. Battle.net then only checks the
files and downloads what is missing.

1. Start Forever Launcher once and install Battle.net (steps 1–3 above), then
   close the app.
2. On the PC, find your `World of Warcraft` folder. On Windows it is usually
   `C:\Program Files (x86)\World of Warcraft`. A Mac install
   (`/Applications/World of Warcraft`) works too; Battle.net replaces the Mac
   game files with the Windows ones.
3. With an FTP program, copy the whole folder, including the hidden files
   `.build.info` and `.product.db`, to the console as:

   ```text
   /data/forever/prefix/drive_c/Program Files (x86)/World of Warcraft
   ```

   Use a wired network if you can. Classic is about 50 GB; Retail is much larger.
4. Start Forever Launcher. In Battle.net, select the game. If it shows
   **Install**, use **Locate the game** below the button and choose
   `C:\Program Files (x86)\World of Warcraft`.
5. Battle.net checks the files and downloads only what is missing or newer.
   For a full check, use the gear next to **Play** → **Scan and Repair**.

Forever Launcher fixes the file permissions of copied folders at startup, so
you normally don't need to change them by hand. If it still says it can't
write to `/data/forever/prefix`, see [Troubleshooting](#troubleshooting).

## Signing in to Battle.net

- Sign in with your **email address and password**, and choose to get a **code by email**
  when Battle.net asks to confirm the sign-in.
- Your account needs an **authenticator** enabled.
- Signing in with **Google, Apple, Facebook, Xbox, PlayStation or any other
  third-party account is not supported.**
- Forever Launcher does not implement credential capture. Battle.net keeps its
  own sign-in token in your user data folder so you stay signed in.

## Controls

| Press | Does |
| --- | --- |
| **Options** | Opens or closes the Forever menu while Battle.net is open |
| **F12** on a USB keyboard | Opens or closes the Forever menu while Battle.net is open |
| **L2** | Escape in Battle.net (closes an open Battle.net dialog or dropdown) |
| **L3 + R3** (press both sticks briefly) | Opens the on-screen keyboard |
| Hold the touchpad (still, about 0.4 s) | Voice-to-Text: speech is typed into WoW chat |

The controller is turned on in WoW by default (WoW's own gamepad mode).

With Voice-to-Text enabled, hold the touchpad still and speak into the DualSense
microphone. A red-dot **Listening** panel appears in the middle of the screen
while recording and disappears on release. While the text is in the chat box,
**Cross** sends it, **Circle**
cancels, and **Square** opens the on-screen keyboard to edit. **L1/R1** cycle
the chat channel through **/s /p /i /g /raid /r**. The DualSense **Mute** button
mutes the microphone: a muted mic types nothing.

In Battle.net, the mouse cursor hides after three seconds without pointer
movement. Move the pointer or click to show it again. A new visible cursor
shape also restarts the timer; the game's or app's own hidden cursor stays hidden.

## Forever menu

Open the menu over Battle.net with **Options**, or press **F12** on a USB
keyboard. Use the D-pad and Cross; press Circle or Options at the top level to
close, or click with a mouse. The menu can:

- turn frame generation, HDR and the performance overlay on or off;
- set a **Frame rate cap** for games: Off (the game decides), 30, 60 or 120 fps
  (120 needs a 120 Hz TV); a game's own lower cap still applies;
- set the **Output resolution** games see: Auto (3840x2160), 1440p, 1080p, or
  16:10 1600p (2560x1600), 1200p (1920x1200) and 800p (1280x800, the Steam
  Deck's size). A 16:10 image keeps its shape, with black bars at the sides;
  the Battle.net desktop stays 1920x1080;
- turn **Invert mouse scroll** on or off (USB mouse wheel only; on by default);
- turn **Voice-to-Text** On/Off (first use downloads a **190 MB speech model**);
- browse, install, update and remove add-ons for the selected WoW version;
- install a curated starter pack;
- remove Battle.net while keeping installed games and their settings;
- remove and reinstall Battle.net from a fresh installer download;
- close Battle.net and the app (**Exit Battle.net**).

Each app start shows a ten-second hint for **Options** over Battle.net.

Open **Add-ons** before pressing Play. L1/R1 cycles through detected installed
WoW channels; Left/Right switches Installed, Updates, Find and Starter packs.
Cross opens or installs, Square updates the
selected add-on, Triangle updates all, and R3 removes after a confirmation whose
default is Cancel. In Find, type on a USB keyboard or choose **Search…** for the
on-screen keyboard (results filter as you type); **Add from GitHub link…** accepts `owner/repo` or its GitHub
URL. CurseForge-only rows say so and cannot be installed. GitHub, WoWInterface
and TukUI entries work without a key. Changes load the next time WoW starts, or
after typing `/reload` in WoW.

If Battle.net needs another web renderer process, Add-ons pauses instead of
blocking Battle.net. Close and reopen **Add-ons** to resume.

Add-ons are checked against the selected channel's product and client version,
and go into that channel's own `Interface/AddOns` folder. Find and Starter packs
cover Retail (including PTR/beta), Classic MoP, Anniversary, Classic Era and
Forever. Find uses [Forever's own GitHub add-on catalogue](https://github.com/bizkut/github-wow-addon-catalogue),
derived from [layday's GPL-3.0 catalogue](https://github.com/layday/github-wow-addon-catalogue),
and compatible WoWInterface/Tukui entries. Our list refreshes daily and the
launcher checks it at most once every 24 hours; offline use keeps the last good
cache or the build's bundled snapshot.
**Add from GitHub link…** remains available. Unknown products say “Add-ons aren't
supported for this WoW version yet”; they never receive another flavour's files.

For Wago add-ons, choose **Wago key** on the Add-ons page. Wago gives the key
for add-on managers to its Patreon supporters: subscribe at
[addons.wago.io/patreon](https://addons.wago.io/patreon), then copy the key it
gives you. The API key on Wago's account page is for publishing add-ons and
does not work here. Type the key, or choose **Read from file** after saving it
as the first line of `C:\Forever\Addons\wago-key.txt` (up to 256 characters).
A successful import deletes the file. The key is saved locally by AddonForge;
**Remove key** clears it. If Wago cannot be reached, it is saved as **set (not
checked)**. Without a key, Wago-only add-ons show **Wago key required**; all
other sources work without one.

If frame generation currently uses `fg=auto`, selecting it turns it off;
selecting it again turns it on. Frame generation and the performance overlay
use their live settings files; HDR, the frame rate cap and the output
resolution apply the next time a game starts. The cap and the resolution are
`fps=` and `res=` in `C:\wineps5-display.txt`; the cap reaches Direct3D 12
games through vkd3d-proton and Direct3D 9 to 11 games through DXVK. After removing Battle.net, close and start Forever Launcher to install it
again.

## Frame generation

Frame generation shows extra, generated frames between the frames WoW draws,
so motion looks smoother at up to 120 Hz. It is **off by default**; turn it on
in the Forever menu (2x). It needs **your own copy of `Lossless.dll`** from
[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)
(version 3.2.2.0, or a build with the same shaders); the file is not included
and never downloaded. Without it, the menu shows **Needs Lossless Scaling**.

**VRR**: with VRR set to **Automatic** in the PS5's Screen and Video settings
and an HDMI 2.1 VRR display, refresh varies while WoW runs. WoW asks for
120 Hz (119.88 Hz) and falls back to 59.94 Hz if the display refuses. There is
no menu switch; the console and display settings decide.

To use Lossless Scaling:

1. On your PC, find `Lossless.dll` in your Lossless Scaling folder
   (Steam: `steamapps/common/Lossless Scaling/`).
2. Copy it by FTP to the console as:

   ```text
   /data/forever/prefix/drive_c/Lossless.dll
   ```

3. Restart WoW.

Settings live in `/data/forever/prefix/drive_c/wineps5-fg.txt`. Changes apply
while the game runs, within a second.

```text
fg=off mult=2 flow=1.0 method=lossless
```

| Option | Values |
| --- | --- |
| `fg` | `on`, `off`, or `auto` (on only while WoW renders below 70 fps) |
| `mult` | `2`, `3` or `4` frames shown for every frame WoW draws |
| `flow` | `0.25` to `1.0`: lower is cheaper, `1.0` looks best |
| `method` | `lossless`; the launcher sets it at every start |

The on-screen display settings are in `/data/forever/prefix/drive_c/wineps5-hud.txt`
(`hud=on` or `hud=off`, `pos=tl`, `scale=2`); the Forever menu changes `hud`
without replacing the other options.

## WoW settings

WoW starts with settings tuned for the PS5: 4K output at half render scale
(1080p), a 60 fps cap, HDR on, and the controller enabled. Each detected installed channel gets the
preset only when its own `WTF/Config.wtf` is missing. Existing settings are not
overwritten. The Forever menu changes only `HDROutput` for every detected
channel, leaving other settings, including `HDRFactor`, alone. The output
resolution sets `RenderScale` so WoW draws its 3D at about 1080 lines: 0.5 at
Auto, 0.75 at 1440p, 1.0 at 1080p, 0.675 at 1600p, 0.9 at 1200p and 1.0 at
800p. A preset gets the value when it is installed; an existing file changes
only while its `RenderScale` is still the value for the previous resolution.

Automatic discovery currently scans
`C:\Program Files (x86)\World of Warcraft`. Custom C: or W: installations can
launch from Battle.net Play, but are not yet included in automatic settings,
Add-ons discovery or direct start.

### Start one channel directly

Create `/data/homebrew/PPSA99030/wine/direct-game` with just the channel folder
name, for example `_classic_era_` or `_retail_`, then start Forever Launcher.
The file is consumed once. If that channel is missing or incomplete, the log
explains why and Battle.net opens; another channel is never chosen instead.
An empty file prefers Classic beta, then Forever, then another complete
installed channel.

## Where your data is

Everything that is yours is in **`/data/forever`**: Battle.net, WoW, your
settings, `Lossless.dll` and the shader cache. The app folder
(`/data/homebrew/PPSA99030`) holds only the app.

Updating the app replaces only the app folder. Your games and settings stay.

## Updating

Close the app and replace only its `PPSA99030` app folder with the folder from
the new zip. Do not delete `/data/forever`.

Every start checks the app's Windows files in `/data/forever` and repairs any
that differ from the app's copies. The first start after an update checks every
file (about 15–20 seconds); later starts take under a second.

## Uninstalling

Delete the app from the home screen (or delete `/data/homebrew/PPSA99030`), then
delete `/data/forever` to remove Battle.net, WoW and your settings.

## Privacy and support data

- Release logs retain aggregate input-health counters, not raw keyboard records,
  key usages or modifiers.
- Treat `/data/forever` as sensitive shared Wine-prefix storage. Never include
  registry hives, browser profiles, session/account files or saved credentials in
  support bundles, and review Wine/application logs and screenshots before sharing.

## Troubleshooting

- **"Forever Launcher can't write to ..." (permission denied)**: the app
  restarts its file helper once to repair folder permissions under
  `/data/forever`, then tries again. If the message remains, close the app and,
  on a PC, use FTP to set `/data/forever` and everything in it to permission
  `777`; then start Forever Launcher again.
- **Drives:** only `C:` and `W:` work inside Wine; `Z:` and network shares are
  not supported.
- **"Forever Launcher cannot use /data" (error 2 or 30)**: `/data` is missing
  or read-only. Update ShadowMountPlus to a version that lets apps use `/data`.
- **Battle.net sign-in fails**: use the email code and make sure an authenticator is
  enabled on your account. Third-party sign-in is not supported.
- **Battle.net reports only about 17 GB free**: the write helper is not answering
  or is from an older app version, so a fixed 16 GiB answer is used. Restart the
  app. The kernel log shows
  `wine: title disk space source=broker` (real) or `source=synthetic` (fallback).
- **Frame generation does nothing**: check that `wineps5-fg.txt` says `fg=on`.
  Open the Forever menu from Battle.net to turn the performance overlay on; it
  shows `FG LOSSLESS`. The file must be exactly
  `/data/forever/prefix/drive_c/Lossless.dll`.
- **Logs**: the console's kernel log (port 3232) shows what the app is doing.
  Lines starting with `forever:` describe setup.

## Building from source

Releases are built by [`ps5/release/build-linux.sh`](ps5/release/build-linux.sh)
from the public sources pinned in [`ps5/release/deps.json`](ps5/release/deps.json).
See [BUILDING.md](BUILDING.md).

## Credits

Forever Launcher is built on the work of many projects. Thank you to all of them.
Licences and details are in [NOTICE.md](NOTICE.md).

- [Forever Launcher](https://github.com/bizkut/ps5-forever) by bizkut: the upstream this fork
  tracks, including its Wine, vkd3d-proton, DXVK and Mesa forks.
- [Proton](https://github.com/ValveSoftware/Proton) by Valve: the base this project starts from.
- [Wine](https://www.winehq.org/): runs Windows programs (LGPL-2.1-or-later).
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) by Hans-Kristian Arntzen
  and contributors: DirectX 12 on Vulkan (upstream LGPL-2.1-or-later;
  this frame-generation build GPL-3.0-or-later).
- [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors: DXGI (zlib).
- [Mesa](https://mesa3d.org/) and its RADV Vulkan driver (MIT).
- [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)
  and [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) by mihawk-99: RADV, Vulkan
  and the SDK on PS5.
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) by John Törnblom.
- [Eden](https://eden-emu.dev/) emulator: the frame-generation port this project adapts (GPL-3.0-or-later).
- [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden): frame generation on PS5.
- [ps5-framegen](https://github.com/swordpdf/ps5-framegen) by swordpdf: FSR 3 frame generation host
  (GPL-3.0-or-later), with AMD [FidelityFX](https://gpuopen.com/fidelityfx-super-resolution-3/) shaders (MIT).
- [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) by PancakeTAS: the Vulkan Lossless Scaling frame-generation
  code that Eden's port builds on (GPL-3.0-or-later).
- [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) by THS: frame-generation shaders,
  used only from your own copy.
- [FreeType](https://freetype.org/), [GnuTLS](https://www.gnutls.org/),
  [Nettle](https://www.lysator.liu.se/~nisse/nettle/), [libtasn1](https://www.gnu.org/software/libtasn1/)
  and the [pacbrew](https://github.com/OpenAGC/pacbrew-repo) recipes that build them.
- [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) by drakmor.
- Noto and Droid fonts (OFL / Apache-2.0) and Mozilla's root certificates (MPL-2.0).
- Mihawk's earlier Forever Launcher: generated placeholder artwork and runtime icon extraction (MIT).
- [stb_image](https://github.com/nothings/stb) by Sean Barrett and contributors: PNG decoding (MIT or public domain).

## Licence

Forever Launcher is licensed under the [GNU General Public License v3.0 or later](LICENSE).
Third-party components keep their own licences; see [NOTICE.md](NOTICE.md).

World of Warcraft, Battle.net and Blizzard are trademarks of Blizzard Entertainment, Inc.
PlayStation and PS5 are trademarks of Sony Interactive Entertainment Inc. All other
trademarks belong to their owners.
