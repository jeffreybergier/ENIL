# Image sync fix handoff

Updated 2026-10-19. The image validation fix is implemented and committed as
`063793b` (`fix: validate cached images on legacy ImageIO`). Remaining work is
primarily native compatibility and end-to-end sync/UI testing, then deciding
with the user when to deploy or release. Nothing has been pushed or installed
on production. Read `AGENTS.md` before continuing.

## Production boundaries

- `imac-rsa` is the user's production iMac, running Leopard 10.5.8.
- `gomadango` is the user's production iPhone, running iOS 6. Its installed
  ENIL version was 1.1.0 when inspected.
- The user explicitly directed native testing to **`x4-vm`**. That machine
  actually runs **Tiger 10.4.11 on PowerPC**, not Leopard.
- Do not resume tests, syncs, app launches, installations, or database changes
  on production devices. The user was shutting them down. Use saved fixtures
  and the authorized VM; arrange any additional test host with the user.
- The two devices use different LINE client identities (Chrome versus desktop)
  for the **same account**. Different account-directory or chat IDs do not mean
  different users. Match images by message ID, then use the destination
  database's `chat_id` to construct its media paths.
- Follow the root Git instructions: local commits as Strappy, no push.

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

2. Test the full attachment recovery path against a **disposable copy** of the
   saved Mac database and media, using destination chat IDs. Prefer an offline
   harness that refuses network requests. Verify successful cached images
   repopulate media/thumbnail paths and dimensions, and unavailable files
   remain accurately counted. The existing native harness tests ImageIO,
   not the complete database sync or chat UI.

3. Test visible images/thumbnails in an isolated app/test account on an
   authorized test machine. No actual UI or complete post-fix sync has yet
   been run. Do not inject saved login sessions into tests or inadvertently
   connect the user's account to LINE.

4. Obtain native **Leopard** coverage on a user-approved test host if possible;
   `x4-vm` proves Tiger behavior only. Also consider a modern Mac and test iOS
   device for shared-code regression coverage. iOS has cross-build coverage,
   not native runtime coverage of this fix. Do not use the production iMac or
   phone for this merely because they are reachable.

5. Report the remaining genuine unavailable-image count separately from the
   validation bug. All 115 disappearing has not been demonstrated: not every
   image exists in the captured phone cache, and some network objects return
   404. Investigate residual failures before promising a zero warning count.

6. Coordinate production deployment/release with the user after testing.
   Normal fixed Mac package: `build/macOS/release/ENIL/ENIL.zip`.
   Normal fixed iOS package: `build/iOS/release/ENIL/ENIL.ipa`.
   These are local builds of the new commit; the version metadata was not
   bumped and they are not a new published tag. Never deploy the test harness
   as the user's app. Nothing has been pushed.
