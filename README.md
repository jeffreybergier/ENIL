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
docker compose run --rm altivec bash -lc 'make -C source/macOS clean && make -C source/macOS debug'
docker compose run --rm altivec bash -lc 'make -C source/iOS clean && make -C source/iOS debug'
```

Inside a prepared container, the two `make` commands can be run directly.
Native build artifacts appear under each target's ignored `build-debug/` or
`build-release/` directory. A source checkout alone does not supply the SDKs
or `/altivec` engine libraries.

Configure the Worker URL and secret in the app's preferences. The native
client also accepts `ENIL_WORKER_URL` and `ENIL_WORKER_SECRET` through its
environment; credentials are not embedded in the committed app source.

See [native development](docs/native-development.md), [outstanding work](PLAN.md),
and [release instructions](RELEASE.md).

## Cloudflare Worker

See [source/cloudflare/README.md](source/cloudflare/README.md) for local use,
validation, and deployment. Cloudflare Workers Builds continues to use
`source/cloudflare` as its root, `npm run build` as its build command, and
`npx wrangler deploy` as its deploy command.

Downloaded LINE code and generated JavaScript/WASM are build artifacts, excluded
from Git. The native apps call LINE directly and use the Worker for crypto.
