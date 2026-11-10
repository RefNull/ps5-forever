# Steam test product (parked)

This branch adds a second product, Forever Steam Test (`PPSA99031`), that runs
the Windows Steam client in Big Picture on the same runtime as Forever
Launcher. It is parked: Steam does not reach its UI yet.

## Build

```bash
FOREVER_TITLE_ID=PPSA99031 FOREVER_TITLE_NAME="Forever Steam Test" \
  bash ps5/release/build-linux.sh SOURCE_ROOT VERSION
```

The profile is `ps5/probes/psmkit-wine/b1-steam.json`; the build refuses to
use it for `PPSA99030`.

## Works on a console (firmware 13.40)

- First start downloads Valve's 64-bit bootstrapper zip, checks its SHA-256 and
  unpacks `steam.exe` (`SteamSetup.exe` cannot load: 32-bit, fixed base).
- Steam updates itself; the write broker absorbs the console's file throttle.
- Chromium's 32 GiB address reservation succeeds (Wine patch 0007).
- `steamwebhelper` starts as a single process with `--no-sandbox` and
  `--disable-gpu`; probe programs and the overlay are refused.

## Open

Steam still shows "There was a problem with your Steam installation". Leads:

- `steamsysinfo.exe` is refused with access denied; other PS5 setups make it
  look missing instead.
- No service manager runs in the title, so Steam's service cannot start.
- `GetAdaptersAddresses` and DNS configuration fail inside the browser.

Streaming from the PS5 through Steam is not realistic here: Wine's and DXVK's
desktop duplication are stubs and there is no hardware encoder or audio
loopback. The PS5's own Remote Play with chiaki-ng is the practical route.
