# ENIL

LINE Messenger clients for retro Mac OS X and iOS, with a Cloudflare Worker
for the LINE extension's cryptographic operations. Native source was migrated
from ENIL-cocoa at commit `c1359001724b2651eac73e6b0c129078e291da9c`.

| Directory | Purpose |
| --- | --- |
| `source/shared/` | Shared C/Objective-C protocol, database, sync, media, and account code |
| `source/macOS/` | AppKit app, resources, and Mac Makefile |
| `source/iOS/` | UIKit app, resources, signing entitlements, and iOS Makefile |
| `source/deps/qrcodegen/` | Pinned QR-code generator submodule |
| `source/cloudflare/` | Crypto Worker and extension download/build pipeline |

## Native development

Initialize the dependency after cloning:

```sh
git submodule update --init --recursive
```

Native builds require the Altivec engine/libraries and OSXCross toolchains
provided by the Altivec Intelligence container, plus three external SDK archives:
`MacOSX10.5.sdk.tar.xz`, `MacOSX11.3.sdk.tar.xz`, and `iPhoneOS8.4.sdk.tar.gz`.
Place them in the ignored `.altivec-sdk/` directory, then run from the repo root:

```sh
docker compose run --rm altivec-sdk preflight
docker compose run --rm altivec-sdk install
docker compose run --rm altivec bash -lc 'make clean && make -j2 debug release'
```

Inside a prepared container, build directly from the repository root:

```sh
make                         # Release builds for macOS and iOS
make debug
make macOS-release           # One platform
make iOS-debug
make analyze                 # Reports under build/<platform>/analyze/
make validate
make help
```

Native outputs are in `build/macOS/{debug,release}/` and
`build/iOS/{debug,release}/`. Each configuration contains `Intermediates/`,
`ENIL.app`, debug symbols, and its `ENIL.zip` (Mac) or `ENIL.ipa` (iOS).
The default `build/` sits beside this root Makefile. Override it with
`BUILD_ROOT=/another/path`; relative overrides resolve against the repository
root, including direct child calls such as `make -C source/iOS debug`.
The underlying Altivec rules do not support whitespace in output paths.

Use `make macOS-<target>` or `make iOS-<target>` to forward any child target.
The child `Makefile` files are public wrappers; source lists, flags, and
Altivec compiler/linker rules are configured in each platform's `build.mk`.
`source/make/native-targets.mk` selects output directories and packages iOS
archives without changing the container's installed build engine.

`make clean` removes both native platform output trees; `make macOS-clean`
or `make iOS-clean` removes only one. Unrelated files in a custom build root
are retained, and source/Git directories are rejected as cleaning destinations.
Run cleaning before a parallel build, e.g. `make clean && make -j2 debug release`.
A source checkout alone does not supply SDKs or `/altivec` engine libraries.

Configure the Worker URL and secret in the app's preferences. The native
client also accepts `ENIL_WORKER_URL` and `ENIL_WORKER_SECRET` through its
environment; credentials are not embedded in the committed app source.

See [native development](docs/native-development.md), [outstanding work](PLAN.md),
and [release instructions](RELEASE.md).

## Linux-hosted tests

```sh
npm --prefix source/cloudflare ci
make test-host               # Build-system checks plus real-WASM Worker tests
make test-build-system       # Fast wrapper/cleanup checks without Apple SDKs
make cloudflare-test         # Worker suite only
```

These tests have no native debug/release executable. Build-system tests create
and remove temporary fixtures; Worker tests use the existing ignored download
cache and generated JS/WASM under `source/cloudflare`. Native source outputs
are always under the selected root build directory.

## Cloudflare Worker

See [source/cloudflare/README.md](source/cloudflare/README.md) for local use,
validation, and deployment. Cloudflare Workers Builds continues to use
`source/cloudflare` as its root, `npm run build` as its build command, and
`npx wrangler deploy` as its deploy command.

Downloaded LINE code and generated JavaScript/WASM are build artifacts, excluded
from Git. The native apps call LINE directly and use the Worker for crypto.
