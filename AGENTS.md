# Agent guide

Forever Launcher (`PPSA99030`) runs Battle.net and World of Warcraft on a
jailbroken PS5 through a Wine fork. This repository is the launcher (title,
runtime, helpers) plus the build glue. Wine, Vulkan/RADV, the payload SDK,
vkd3d-proton, DXVK, AddonForge and the pacbrew recipes are separate pinned
forks: `ps5/release/fetch-sources.py SOURCE_ROOT` checks them out next to this
checkout (`SOURCE_ROOT/ps5-proton/ps5-wow`). Changes this fork makes to them
are numbered patches applied on top of the pin (`ps5/radv/patches/`,
`ps5/vkd3d-proton/patches/`, `ps5/addonforge/patches/`); refresh a patch when
its pin moves.

## Layout

- `ps5/title/` – the PS5 title: desktop, menu, input, IME, voice.
- `ps5/probes/psmkit-wine/` – Wine runtime parent/child, `build-runtime.py`,
  and the host tests (`Makefile`).
- `ps5/writebroker/`, `ps5/addon-worker/`, `ps5/addonforge/` – console helpers.
- `ps5/build-*.sh`, `ps5/radv/`, `ps5/whisper/` – dependency builds.
- `ps5/release/` – pin manifest (`deps.json`, read via `pins.py`), container
  (`Dockerfile`), `build-linux.sh`, packaging, licence records.
- `.github/workflows/release.yml` – tag-triggered release build.

## Judges, weakest first

1. macOS: the Wine-free subset of the host tests (individual `test-*`
   targets, `make -C ps5/probes/psmkit-wine test-pins`).
2. Linux: the full host suite, `make -C ps5/probes/psmkit-wine test` (needs
   the pinned Wine checkout).
3. Release build: `ps5/release/build-linux.sh SOURCE_ROOT NN.NNN.NNN` in the
   pinned container; it also runs the host suite.
4. Console. Host tests are not console acceptance; say which judge a claim
   rests on.

## Rules

- Done means a test that fails without the change, run by the strongest judge
  available. Say what remains unproven.
- One commit per purpose. Subject line, then bullet points of the key changes.
- Pins (commits, versions, URLs, checksums) live only in
  `ps5/release/deps.json`. Scripts read it; copies that cannot (Dockerfile,
  workflow default, Wine lock, licence provenance) are checked by `test-pins`.
  Bump a pin in `deps.json`, update those copies, run `test-pins`.
- Never commit personal identifiers: no addresses, hostnames, account or
  personal names, local paths or credentials, in files or commit messages.
- Keep docs brief and current; no history or marketing in them.
