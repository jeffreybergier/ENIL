# ENIL Cloudflare Worker

A crypto service for ENIL clients. Clients talk directly to LINE and use this
Worker for LTSM request signing, key generation, and message encryption/decryption.
The running Worker does not contact LINE. The build downloads the extension.

## Build and test

Requires Node.js 22 or newer and npm.

```sh
cd source/cloudflare
npm ci
npm test
```

`npm run build` downloads LINE from Google's Chrome extension update service,
checks its manifest version and the SHA-256 hashes of the required JavaScript and
WASM, extracts the required modules using a JavaScript parser, and applies ENIL's
Worker compatibility and portable-key-state changes. It does not execute the
extension during extraction or need the old ENIL-cloud checkout.

The supported version and input hashes are in `scripts/extension.json`. Only
metadata and ENIL source/build/test code belong in Git. These paths are ignored:

- `.cache/`: downloaded extension archive.
- `src/ltsm-worker.js` and `src/ltsm.wasm`: generated LINE-derived runtime assets.
- `dist/`, `.wrangler/`, and `node_modules/`: build and development artifacts.
- `.dev.vars*` and `.env*`: local secrets.

A verified local archive is reused. Run `npm run build -- --refresh` to fetch
again. A clean checkout always downloads. Unsupported versions and changed
hashes fail the build and remove stale generated assets. Google may stop serving
a supported version; the pin validates downloads but does not make historical
versions available. Review the extractor and tests before updating the pin.

The committed integration tests use synthetic requests and freshly generated
keys; they do not include captured account keys or messages. They cover real
WASM signing, key generation, reset, and portable key restoration. Routing tests
cover the E2EE endpoints with mocks; full real-message E2EE fixtures remain in
the original research repo.

## Local Wrangler

Put a local test secret in `.dev.vars`:

```dotenv
WORKER_SECRETS=your-local-test-secret
```

Then run `wrangler dev --local` (or `npm run dev`). Wrangler's custom build hook
runs the download/extraction automatically. The custom build watcher watches
`scripts/` to avoid watching its own generated output.

Clients send `X-Worker-Secret` matching one of the comma-separated values in
`WORKER_SECRETS`. Never commit a real secret.

To check deployment packaging without uploading:

```sh
wrangler deploy --dry-run --outdir dist
```

## Cloudflare Workers Builds

Connect the ENIL repository to the Worker and configure:

| Setting | Value |
| --- | --- |
| Root directory | `source/cloudflare` |
| Build command | `npm run build` |
| Deploy command | `npx wrangler deploy` |
| Worker name | `enil-cloud` (must match `wrangler.toml`) |

Cloudflare installs dependencies from `package-lock.json`. The deploy command's
Wrangler build hook repeats extraction using the verified download cache.
Configure `WORKER_SECRETS` as a **runtime secret** on the Worker. It is not
required for building. The tests use their own test-only secret.

The repository excludes downloaded LINE code and binaries; the deployed Worker
bundle still includes the extracted JavaScript and WASM it needs at runtime.
