# ENIL



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

`make test-host` covers certificate isolation, replacement, account removal,
QR restart, persistence failures, and real C login requests against offline
loopback fixtures, including accepted-certificate and rejected-certificate flows.
