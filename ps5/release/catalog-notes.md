# Release notes

`release.yml` publishes the section below as the GitHub release description.

## Release notes (GitHub release description, shown by the catalog)

### Forever Launcher 01.024.001 (fork)

Unofficial fork of bizkut's Forever Launcher, based on upstream
[01.000.024](https://github.com/bizkut/ps5-forever/releases/tag/01.000.024).
Work in progress; not yet tested on a console.

- Safer write helper: no longer follows links planted under `/data/forever`,
  cannot be stalled by special files, starts only once, and reports when
  another app holds its port.
- Updated components: miniz 3.1.2 plus ZIP-reader fixes, FreeType 2.15.0,
  current root certificates.
- Add-ons: sorting a large catalogue no longer stalls the screen.
- Leaner app: test-only code and unused tooling removed.
