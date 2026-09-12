# ENIL

ENIL contains native LINE clients for retro macOS/iOS and their Cloudflare
crypto Worker. Paths and build commands below are relative to this repository.

## Native clients

For changes to `source/shared/`, `source/macOS/`, `source/iOS/`, native build
configuration, or native release automation, read
[docs/native-development.md](docs/native-development.md). It preserves the
native architecture, C/Objective-C boundaries, compatibility rules, and build
requirements. Outstanding app work is in [PLAN.md](PLAN.md).

Native validation: from the repository root, use `make clean` followed by
`make -j2 debug release` when shared build inputs change. Clean-build macOS and
iOS when shared code changes. For platform-only code, build the affected target. Inspect
compiler warnings and errors. A container cross-build does not establish that
the app launches on a Mac or iOS device; report separately whether that was tested.

Native outputs live in `build/<platform>/<configuration>/Intermediates` and
the surrounding configuration directory. `BUILD_ROOT` overrides the root.
Use platform-prefixed root targets (`macOS-debug`, `iOS-release`, etc.) for
individual builds. Child source lists and flags live in `build.mk`; the
platform `Makefile` files forward through `source/make/native-targets.mk`.
Run `make test-host` for the build-system and Worker tests hosted on Linux.

The native clients must not execute LINE extension code. They call the Worker
for LTSM crypto and LINE directly for normal API traffic.

## Cloudflare Worker

For `source/cloudflare/`, follow its [README](source/cloudflare/README.md).
Native compiler, UI, memory-management, and Apple SDK rules do not apply to
Worker-only changes. Run `npm test` from `source/cloudflare` for Worker changes.

The build downloads and verifies the extension, then generates the required
JavaScript/WASM. Keep downloaded extension material and generated runtime
assets out of Git. The running Worker is a crypto service and must not make
outbound LINE requests. Do not move extension code into the native clients.

## Repository boundaries

- Keep `.env`, local Worker secrets, SDK archives, generated headers, and build
  outputs out of Git.
- Preserve `source/deps/qrcodegen` as a pinned submodule.
- Research archives and captured fixtures live separately in ENIL-extras.
  They are not native or Worker build inputs.
- Native release packaging is configured in `.altivec-release.yml`; GitHub
  release automation is in `.github/workflows/release.yml`.
