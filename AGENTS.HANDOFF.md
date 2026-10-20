# Image sync fix handoff

Updated 2026-10-20. The image validation fix is implemented and committed as
`063793b` (`fix: validate cached images on legacy ImageIO`). Leopard offline
recovery and a representative message-pane check now pass. Remaining work
includes the 32-byte size discrepancies below, missing originals, and deciding
with the user when to run a live sync or release. The fixed Mac build is now
installed on `imac-rsa` but has not been opened. Nothing has been pushed.
Read `AGENTS.md` before continuing.

## Production boundaries

- `imac-rsa` is the user's production iMac, running Leopard 10.5.8.
- `gomadango` is the user's production iPhone, running iOS 6. Its installed
  ENIL version was 1.1.0 when inspected.
- The user explicitly directed native testing to **`x4-vm`**. That machine
  actually runs **Tiger 10.4.11 on PowerPC**, not Leopard.
- Do not resume tests, syncs, app launches, installations, or database changes
  on the production phone. On October 20 the user authorized isolated offline
  recovery and UI testing on `imac-rsa`, with ENIL quit. This supersedes the
  earlier prohibition on testing that Mac. Use disposable Desktop copies,
  never the production account directory. The user subsequently authorized
  installing the fixed Mac build but explicitly said not to open it. No live
  sync is authorized. See the installation record below.
- The two devices use different LINE client identities (Chrome versus desktop)
  for the **same account**. Different account-directory or chat IDs do not mean
  different users. Match images by message ID, then use the destination
  database's `chat_id` to construct its media paths.
- Follow the root Git instructions: local commits as Strappy, no push.

## October 20 Leopard recovery testing

The installed Mac app is 1.1.1. App and complete account backups are under
`~/Desktop/enil-image-regression-20261020` on `imac-rsa`, with checksum manifests.
The production database, WAL, and app executable were compared against those
backups and match at the end of testing. No live sync was performed.

- Final image harness on Leopard/PPC: all 220 captured cache files and all
  seven synthetic cases passed.
- New `source/tests/attachment_mac_test.py/.m` harness runs the real
  `enil_sync_attachments` pipeline against a disposable database and copied
  media/purchases. It creates and removes its own synthetic session; never
  copy production `session.json` into this fixture.
- All application curl transfers are renamed at compile time to a refusal.
  Additionally run under `sandbox-exec`, denying `network*`, production
  account reads/writes, and writes under the user's Library. The observed
  missing-image requests fail before curl because the synthetic session has
  no OBS token. This is an offline cache test, not a download-success test.
- Baseline recovery: **115 images, zero complete before; 85 repaired and
  30 still unavailable offline**. Paths use destination chat IDs; dimensions
  and original/thumbnail validation passed. Total attachment failure count
  was 30. All 16 database tables matched before/after outside the six image
  path/dimension fields; exactly 85 rows changed. The sync cursor and message
  count were preserved.
- A repeat recovery run deliberately removed one copied thumbnail; it was
  regenerated successfully and the result remained 85 complete / 30 pending.
- The final `--ui --regenerate` run passed on Leopard: the actual message
  controller loaded recovered thumbnails, the original link resolved to an
  image that decoded, and a private screenshot was saved. This is a
  representative message-pane check using the production renderer with an
  offline model, not a full application startup, live sync, or external
  Preview launch. An earlier UI attempt selected a chat with no image in its
  first 50 messages; the harness now selects an image within the initial page.
- Of the 30 remaining originals, **15 are missing and 15 exist but are exactly
  32 bytes shorter than `media_plain_size`**. None of the residual cases was
  classified as a decode failure with matching size. Do not call these all
  expired downloads. The file crypto code uses a 32-byte authentication tag;
  whether the stored metadata includes that tag remains to be established.
  Do not drop the size check or change production metadata without evidence.
- `make test-host` passed (99 Python and 57 Worker tests). The PPC harness
  build emitted no compiler warnings. Worker tooling still emits the Node
  DEP0190 deprecation warning.

Native recovery files/logs are in the above Desktop directory's `recovery/`;
`account/` there is disposable. Local private database copies, audit results,
and logs are in `build/tests/image-regression/private/leopard-recovery/`.
The normal app and both test harnesses are all named `ENIL.app`; never install
a harness as the production app.

Build the recovery harness with:

```sh
ARCHS=ppc python3 source/tests/attachment_mac_test.py
```

Its package is `build/tests/attachment-native/macOS/release/ENIL/ENIL.zip`.
Run only on an explicitly prepared disposable account directory below Desktop
containing `.enil-offline-fixture`, copied database/WAL and cache files, and no
session. Optional `--regenerate` removes one fixture thumbnail and checks its
repair; `--ui` uses the actual message controller and page renderer with an
offline model. Web resources are restricted to files inside the fixture.
Logs and `ui.png` may contain private conversation data; keep them out of Git.

## October 20 Mac installation

The user requested installation on `imac-rsa` without opening ENIL. The normal
`build/macOS/release/ENIL/ENIL.zip` was rebuilt and package validation passed;
it still reports version/build **1.1.1**. No release bump or publication.
Its SHA-256 is
`1da0ed471bc683c50d15b927c0ffc6c124148dc74cde16a39d76db206fdfe261`.

Leopard's package installer stalled without a prompt and was terminated.
The installed executable was verified unchanged at that point. The user then
explicitly approved direct bundle replacement as an exception to the workspace
restriction on manually modifying `/Applications`.

The normal fixed app is now at `/Applications/ENIL.app`. All **320 bundle
entries** (file hashes/executable bits, directories, symlinks) match the staged
release. The prior bundle was also verified after moving it to
`~/Desktop/enil-install-20261020/previous-ENIL.app`; `previous-app.tar` in that
directory provides another backup. Installation logs and the verified ZIP
remain in the same staging directory. ENIL was confirmed **not running** after
installation. Do not open it or run a live sync without a new user instruction.

## Problem and evidence

The Mac reported 115 unavailable attachments after sync. The warning was added
in v1.1.1 by `633351a`; v1.1.0 could report Done despite attachment failures.
The captured Mac database contained 115 image messages, all marked incomplete.
The phone had 114 image messages: 83 marked complete and 31 incomplete. There
were 112 shared message IDs, with identical saved download metadata but
111 differing chat IDs. These counts describe the snapshots, not a fresh sync.

Logs showed 110 HTTP 404 media requests during one retry and a final count of
115 unavailable attachments. Expiration is not established as the root cause:
valid cached originals existed on disk, but validation rejected them and caused
unnecessary download attempts. Do not assume every remaining 404 is fixable.

A native probe on Leopard and a reproduction on Tiger showed source status
Complete while per-image status went from UnknownType (-3) before decode to
Incomplete (-1) after successful decode. Earlier chat explanations incorrectly
called -3 "reading header"; use the actual enum values. ImageIO can also decode
truncated JPEGs, so simply dropping all status checks is not sufficient.

The same premature per-image check was present in both original validation
and thumbnail generation. Both are fixed.

## Implemented change

- `source/shared/enil_cocoa.m`: decode before consulting per-image status;
  share the completeness check between validation and JPEG/thumbnail creation.
- If ImageIO still reports Incomplete after decoding, accept JPEG/PNG only
  after independent container-completeness checks. Other errors and formats
  retain the strict status requirement.
- `enil_jpeg.c/.h`: bounded-memory JPEG marker/segment traversal through EOI,
  including scan stuffing, restart markers, and multiple scans. It is not a
  standalone pixel decoder; ImageIO decoding remains required.
- PNG fallback reuses `enil_png_info`, including chunk CRC and IEND checks.
- Both platform Makefiles include the JPEG helper.
- `test_native_jpeg.py` covers markers, progressive scan structure, malformed
  segments, and every truncation of a synthetic marker fixture.
- `image_mac_test.m/.py` builds a native harness using the production validator
  and thumbnail functions. It has no account/network operations. It writes
  only the explicitly supplied disposable thumbnail output path.

## Manual media recovery already performed

With ENIL closed and the user's authorization, cached phone media was copied
through the container to the Mac. The first transfer preserved source chat
folders, which was wrong for most Mac messages: 218 files were added and two
existing identical files left alone. A second transfer mapped by message ID to
Mac chat IDs: 30 additional files were added, 95 existing identical files and
67 different existing files were preserved. All added files were hash-verified.
Neither database was modified. Production staging directories were removed.

The first transfer's extra source-ID folders were not removed. Do not delete
folders wholesale: some IDs overlap destination data. Any eventual cleanup
must use the manifests, exact hashes, and current destination references.
This cleanup is not necessary to test the code fix.

## Preserved regression data

All paths below are relative to the repository. **Do not run cleanup that
removes these fixtures without first preserving them elsewhere.** They are
ignored build artifacts, not Git-tracked files; a fresh clone will not have
them. Do not commit or publish private media or databases.

`build/tests/image-regression/private/captured/` contains:

- `gomadango/snapshot.sqlite`: consistent phone snapshot, 3,632 messages.
- `imac-rsa/snapshot.sqlite`: consistent Mac snapshot, 3,645 messages.
- Each device directory has transfer checksums and a matching `sample.jpg`.
- `media-transfer/phone-media.tar` and extracted `media-transfer/media/`:
  220 phone cache files, approximately 65 MB.
- `media-transfer/manifest.json`: source paths and hashes.
- `media-transfer/mapped-manifest.json`: source-to-Mac paths and hashes.
- Transfer scripts and logs recording the two copy operations.

Both snapshots were made locally from database plus WAL transfers that stayed
unchanged during capture and matched remote checksums. SQLite quick_check
passed; native LOCALIZED collation index ordering was not verified.

A later refresh was interrupted when the Mac disconnected. The file
`build/tests/image-regression/private/mac-media.interrupted.tar` is incomplete
and **must not be treated as a verified fixture**. Likewise do not rely on the
later raw Mac database/WAL files in `/tmp/enil-attachment-compare/`. Prefer the
preserved `snapshot.sqlite` files above. The earlier verified captures are
sufficient to continue while production machines are off.

## Completed validation

- `make test-host`: 99 Python tests and 57 Worker tests passed.
- Clean Mac/iOS debug and release builds passed, with final edits rebuilt.
  Mac: ppc, i386, x86_64, arm64. iOS: armv7, arm64.
- `make macOS-validate-package iOS-validate-package`: passed.
- Final builds emitted no compiler warnings.
- `make macOS-analyze iOS-analyze`: completed. Four warnings remain in untouched
  code: possible null strcmp input in `enil_asset_cache.c:39`, possible nil
  dictionary key in iOS `QRLoginViewController.m:395`, and unused assignments
  in `ENILAccount.m:211` and `enil_db.c:1048`. No warnings in changed code.
- Native Tiger/PPC on `x4-vm`: all 220 captured cache files validated and
  generated thumbnails successfully.
- Native synthetic cases: baseline and progressive JPEG plus PNG accepted;
  truncated JPEG, missing EOI, truncated PNG, and HTML error rejected by
  validation and thumbnail generation. Seven cases passed.
- The 220-file run preceded a small EOF-check sequencing correction in the
  JPEG parser. Portable tests and the seven native synthetic cases passed
  again with that final correction. The full 220-file run was not repeated.

Local logs: `build/tests/image-regression/results.txt`, `captured-result.log`,
`synthetic-final.log`, `analysis.log`, `enil-host-tests.log`,
`enil-build-complete.log`, and `enil-package-check.log`.

Native fixtures, harness, and logs remain under
`~/Desktop/enil-image-regression` on `x4-vm`. Do not confuse its harness
`ENIL.app` with the normal application: its main function runs tests and exits.

## Remaining work

1. If exact-final-build coverage is desired, rerun the 220 captured files with
   the final harness on `x4-vm`. Native harness build:

   ```sh
   ARCHS=ppc python3 source/tests/image_mac_test.py
   ```

   Output: `build/tests/image-native/macOS/release/ENIL/ENIL.zip`.
   Transfer only to the authorized VM staging directory, unpack, then run:

   ```sh
   cd ~/Desktop/enil-image-regression
   ./ENIL.app/Contents/MacOS/ENIL thumbnail.jpg media/*/*.jpg
   ./ENIL.app/Contents/MacOS/ENIL thumbnail.jpg \
     valid.jpg progressive.jpg valid.png \
     --reject truncated.jpg no-eoi.jpg truncated.png error.html
   ```

   `thumbnail.jpg` is a disposable output overwritten by the harness.
   Local synthetic fixtures are in `build/tests/image-regression/`.

2. Investigate the 15 originals with the exact 32-byte size discrepancy using
   the preserved metadata and fixtures. Determine what `MEDIA_CONTENT_INFO`
   `fileSize` describes before changing the strict size check. The offline
   attachment pipeline, database updates, and thumbnail regeneration have
   now been tested; see the October 20 results above.

3. Representative message-pane rendering now passes. Broader application
   lifecycle and live post-fix sync remain untested. Do not inject saved login
   sessions into offline tests or inadvertently connect the account to LINE.

4. Leopard coverage is now established on `imac-rsa`; `x4-vm` proves Tiger
   behavior only. Consider a modern Mac and an approved test iOS device for
   shared-code regression coverage. iOS has cross-build coverage, not native
   runtime coverage of this fix. Production phone testing is not authorized.

5. Report the 30 offline failures separately: 15 missing originals and 15
   size mismatches. Historical 404 responses do not establish that every
   remaining image is expired. Investigate before promising zero warnings.

6. The fixed Mac build is installed but unopened; coordinate any live sync or
   published release with the user. The phone has not been updated.
   Normal fixed Mac package: `build/macOS/release/ENIL/ENIL.zip`.
   Normal fixed iOS package: `build/iOS/release/ENIL/ENIL.ipa`.
   These are local builds of the new commit; the version metadata was not
   bumped and they are not a new published tag. Never deploy the test harness
   as the user's app. Nothing has been pushed.
