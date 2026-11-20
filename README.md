# ENIL

ENIL performs request signing, E2EE and LEGY transport crypto locally by default.
No Cloudflare Worker URL or secret is required. Existing account keys and
version-1 `workerRestoreState` continue to work; there is no account migration
or automatic remote fallback. The explicit `ENIL_LOCAL_CRYPTO=0` build option
retains the old remote backend and its saved credentials.

The static library, public C API, reproducible generation, offline tests and
platform-validation limits are documented in [Native LTSM](docs/native-ltsm.md).
Initialize the pinned submodules, then prepare the verified build inputs:

```sh
git submodule update --init --recursive
npm ci --prefix source/cloudflare
make ltsm-prepare
make test-host
make debug release
```

Run these Make targets in the configured Altivec environment. Preparation may
download the pinned LINE extension; generated JS/WASM/C are ignored build
inputs, not checked-in sources. Cached builds and native crypto execute offline.

With Podman Compose and the SDK archives in `~/.altivec/.altivec-sdk`, run:

```sh
podman compose --profile tools run --rm -e ALTIVEC_SDK_ARCHIVE_DIR=/root/.altivec-sdk altivec-sdk install
podman compose run --rm --entrypoint /bin/bash altivec -lc \
  'npm ci --prefix source/cloudflare && make ltsm-prepare && make debug release && make test-host'
```

Build roots are resolved from the project Makefile and inherited by child makes.
`BUILD_ROOT=custom` is relative to the repository root. Native artifacts live in
`build/<platform>/<configuration>/<product>/`; `BUILD_VARIANT=name` keeps an
additional build variant. Use `make macOS-clean PRODUCT=<name>` for one product,
`make macOS-clean` for a platform, or `make clean-all` for the whole build root.
CI stages release assets in `build/release` (including the release manifest).

Run `make benchmark-render` for the offline Linux 2,000-message rendering
benchmark. See [workload, baseline and comparison instructions](docs/render-benchmark.md).

QR logins remember the latest nonempty LINE certificate in
`Application Support/ENIL/.qr-certificate-<profile>.json`, with separate slots
for `chrome`, `desktopwin`, and `android`. These private, atomically written
files belong to the installation, not an account, and survive account removal
and fresh QR attempts. Each login reads its slot immediately before certificate
verification; LINE can still require PIN verification. Reauthentication can
reuse an older account's certificate when no installation slot exists. Tokens
and message encryption keys remain in the account's `session.json`.

Tablet sessions use direct Compact Thrift over HTTPS for Talk (`/S4`) and
live polling (`/SYNC4`). Desktop and Chrome routing is unchanged, and the
Tablet transport change does not require signing in again or change message E2EE.

`make test-host` covers certificate isolation, replacement, account removal,
QR restart, persistence failures, and real C login requests against offline
loopback fixtures, including accepted-certificate and rejected-certificate flows.
