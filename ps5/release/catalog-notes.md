# Release notes

`release.yml` publishes the section below as the GitHub release description.

## Release notes (GitHub release description, shown by the catalog)

### Forever Launcher 01.024.005 (fork)

Unofficial fork of bizkut's Forever Launcher, based on upstream
[01.000.024](https://github.com/bizkut/ps5-forever/releases/tag/01.000.024).
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

#### Known issue

- **HDR is work in progress.** With it on, WoW's colours are wrong; turn it
  off in the Forever menu.
