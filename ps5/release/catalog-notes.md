# Release notes

`release.yml` publishes the section below as the GitHub release description.

## Release notes (GitHub release description, shown by the catalog)

### Forever Launcher 01.024.005 (fork)

Unofficial fork of bizkut's Forever Launcher, based on upstream
[01.000.024](https://github.com/bizkut/ps5-forever/releases/tag/01.000.024).
Work in progress; tested on one console (firmware 13.40).

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
- Install note for FTP copies to the internal drive (error `0x80aa001a`).
- **Known issue:** HDR is work in progress; with it on, WoW's colours are
  wrong, so turn it off in the Forever menu.
