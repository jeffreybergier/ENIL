# Image sync fix handoff

Updated 2026-10-20. Image and keychain fixes are installed on both production
devices. The same 15 incomplete image messages were removed from both databases.
The user confirmed both devices sync without missing-file errors. At their
explicit request, the database-cleanup backup directories on both devices and
all local copies from that cleanup run have now been deleted. Earlier app
rollback backups and unrelated regression fixtures remain intact. Nothing pushed.
Read `AGENTS.md` before continuing.

## Cleanup backups deleted with user authorization

After confirming no missing-file errors on either device, the user explicitly
authorized deleting the database-cleanup backups. Removed and verified absent:
- imac-rsa: `/Users/me/Desktop/enil-message-cleanup-20261020`
- gomadango: `/var/root/tmp_altivec/enil-message-cleanup-20261020`
- local: `build/tests/image-regression/private/message-cleanup-20261020`

This includes the raw DB/WAL/SHM backups, held sidecars, standalone before/after
copies, deleted-row exports, verification snapshots and run-specific scripts.
The production databases were not touched. Earlier app/account rollback backups
and unrelated regression fixtures were outside this cleanup and retained.
The cleanup backup and evidence paths in the historical records below no longer
exist; their previous retention instructions are superseded by this authorization.

## Mac database cleanup and phone sync confirmation

After phone-only cleanup, a read-only coherent database/WAL capture confirmed
3,645 messages, 99 image messages, zero incomplete image rows, and zero deleted
IDs returned on gomadango. Its syslog recorded `[Sync.done] Done` at 22:41:07
(device local time). Private evidence:
`build/tests/image-regression/private/message-cleanup-20261020/post-sync-verification.json`.

The user then requested the same deletion on imac-rsa. ENIL was confirmed closed
and all database/sidecar checksums still matched the retained original backup.
Exactly the same 15 rows were removed: 3,673 -> 3,658 messages, 100 remaining image
messages, zero incomplete rows. All other rows across all 16 tables matched the
backup exactly; quick_check and foreign_key_check passed. The edited database
was transferred, validated, and installed with original me:staff ownership.
Original WAL/SHM were moved into `held-original/` beside the Mac backup.
ENIL was not launched; no sync ran. Await the user's Mac sync test.

Mac edited DB SHA256:
`2116f8eadf0b2153c8506a975c035ad82e51342a2bacdabfebc880d862d80949`.
Installed SHA1: `dffa865ad6602943e52105f875c16ae84a294fa4`.
Local audit/export: `message-cleanup-20261020/imac-rsa/` below the private path
listed above. Remote backup stage remains `~/Desktop/enil-message-cleanup-20261020`.
All backups for BOTH devices remain retained; no deletion authorization received.

## Historical database cleanup — backups subsequently deleted

The user requested database backups and deletion of the 15 missing-image
messages. Both apps were quit by the user. Verified snapshots showed the same
15 incomplete image IDs on both devices; no dependent sticon rows existed.
Twelve are within their chats' newest 50 local messages. Because full sync
fetches the latest 50 from LINE and can reinsert deleted rows, the user was
warned that one-time deletion may not persist. They chose: delete only on
gomadango and test sync there. No persistent exclusions were added.

Completed on gomadango: removed exactly those 15 rows from messages_v2;
3,660 messages -> 3,645; incomplete image rows 15 -> 0. SQLite quick_check and
foreign_key_check passed. Byte-preserving comparison across all 16 tables
confirmed all other rows unchanged, including sync cursors and chat metadata.
The edited standalone database was transferred and checksum-verified before
replacement, after confirming production files had not changed since backup.
Original sidecars were retained outside the account; ownership remains mobile.
The installed main DB SHA256 is
`7d737f98d0b2fcdcf4ad995da8bc56674f2de2acdfd99d33ee2e5b0b8c26063a`.
ENIL was not opened and no sync was run by the agent.

Backups to retain (do not remove without explicit user confirmation):
- imac-rsa: `~/Desktop/enil-message-cleanup-20261020/raw/` contains the original
  database, WAL and SHM, with before/after source checksums in the parent.
- gomadango: `~/tmp_altivec/enil-message-cleanup-20261020/raw/` contains the
  original database, WAL and SHM. `held-original/` retains the moved sidecars;
  `after.sqlite` is the installed edited copy. Source checksums are in the parent.
- Local private copies, standalone `before.sqlite` backups, exact target IDs,
  deleted-row export and audit:
  `build/tests/image-regression/private/message-cleanup-20261020/`.
  Do not commit these private files. The local scripts are specific to this run;
  do not blindly rerun them against later account state.

At the phone-only stage, the imac-rsa database was not edited; the later Mac
cleanup is recorded above. Current phone account remains
under `/var/mobile/Applications/022CB2B8-672F-4FD3-B513-3521959403FB`.
If later restoring after new syncs, account for new messages rather than blindly
replacing a newer database with the whole old snapshot.

## Latest production confirmation and Mac installation (2026-10-20)

The user confirmed gomadango restored its existing Worker credential and then
reported only 15 files missing after sync, matching the Mac's remaining count.

At the user's request, the latest normal Mac release was directly installed to
`/Applications/ENIL.app` on imac-rsa using their already authorized replacement
exception. Release/package checks passed. Transfer and installed executable
checksums matched. ENIL was confirmed closed before and after; it was not opened
and no sync ran. The release contains the image fixes and improved Mac save
errors; the temporary keychain override remains iOS-only.

Stage: `~/Desktop/enil-keychain-update-20261020`, with the ZIP and prior bundle
`previous-ENIL.app` retained for rollback.
ZIP SHA1: `6706e5bdfc25dd5160c129dd0af0ea60b9d7fbae`.
Executable SHA1: `b91c94505b738d699f20d5da32c074f2424d52dd`.

## iOS 6 keychain follow-up (2026-10-20)

The user reported -25299 on gomadango after the first keychain fix. Disposable
native tests reproduced an iOS 6 difference: omitted authentication is absent
from returned attributes, not the empty string returned on koolphone5/iOS 8.
Exact empty-auth queries did not find/remove that record. ENIL can produce
-25299 when its bounded replacement loop fails to remove the discovered item;
it does not necessarily mean SecItemAdd returned that status.

AltivecIntelligence commit `7aaf397` pins absent-auth discoveries to their native
persistent reference. Reads/updates/deletes use that identity; the account/group
is checked locally, because iOS 6 rejects an access-group filter alongside the
persistent-reference query. Removed pinned entries are not recreated. Other
stores retain normal behavior. ENIL's temporary source now mirrors this commit.

Upstream library tests passed on both phones, including separate-process
persistence, distinct Default neighbors, invalid account/group protection and
no recreation after deletion. ENIL's real facade, with normal app entitlements,
also passed validation/error propagation, legacy reads/saves, URL replacement
and second-process persistence on both phones. All fixtures were synthetic and
removed; no real Worker credentials were inspected or modified. The installed
app was not launched by the agent during testing.

Evidence: `build/tests/keychain-research/gomadango-fixed.log` and
`koolphone5-ios6-fix.log` in ENIL; `build/keychain-legacy/ios6-final.log`,
`ios8-final.log`, `ios6-cross.log`, `recorder-ios6.log` in AltivecIntelligence.

The revised normal release IPA is installed on gomadango via appinst; transfer
and installed-executable SHA256 checks passed. Stage/log:
`~/tmp_altivec/enil-keychain-ios6-fixed-20261020`. New container:
`/var/mobile/Applications/022CB2B8-672F-4FD3-B513-3521959403FB`.
IPA: `18bab970bab6df436d49ce7cdedc78082b522eeb9c30380f9e5b0fe152edb3cc`.
Executable: `6f11ebfbc8927cbd3944f05bfbc1be48a802afc407674eeb376712e1282cb852`.
ENIL was left unopened for the user's check. Koolphone5's installed app was not
updated during this follow-up; only isolated test executables ran there.

Validation: clean iOS debug/release builds and package checks passed without
compiler warnings. Changed keychain source analysis had no findings. Host suite
passed (100 Python, 57 Worker). All six upstream architecture checks passed;
Mac runtime was not retested. No pushes or container changes were made.

## Keychain regression and temporary iOS fix (2026-10-20)

After the image-fix IPA install, the user reported missing Worker credentials
on gomadango and failed saves on koolphone5. The image-fix IPA was also installed
on koolphone5 for their testing. Native isolated research found that old iOS
writes omitted authentication type, leaving an empty value; the shared library
rejected discovery with -50. Saving replacements could fail on that old entry.
No real keychain credentials were read, deleted, or changed by the agent.

AltivecIntelligence commit `00f985c` fixes legacy authentication identity.
ENIL now has a temporary iOS-only copy named `ENILTemporaryKeychainStore` so
current containers can build the fix immediately. See `docs/keychain-migration.md`
for upstream identity, removal steps, and native test instructions. `/altivec`
was not modified. The Mac keychain implementation remains container-provided.

The expanded upstream device test fails before the fix and passes afterward.
Native tests on koolphone5 passed legacy read/update/delete, distinct Default
identity, and persistence across separate processes. ENIL's actual facade and
normal app entitlements also passed legacy reads, credential saves, URL changes,
and second-process persistence with synthetic fixtures, which were removed.
Logs: `build/tests/keychain-research/facade.log` in ENIL and
`build/keychain-legacy/` in AltivecIntelligence. Device test stage:
`~/tmp_altivec/enil-keychain-research-20261020`.

Both preferences UIs now distinguish input validation from keychain failures
and display the native OSStatus code in alerts. The shared adapter preserves
errors and aborts replacement on denied/locked lookups. Native injected-error
checks passed and preserved the existing synthetic credential.

Validation: clean Mac/iOS debug and release builds passed with no compiler
warnings; 100 Python and 57 Worker tests passed; both package validations passed.
Static analysis completed with the same four pre-existing findings (asset cache
NULL strcmp, database/account dead stores, QR login nil dictionary key), no new
findings. Both iOS slices contain only the temporary keychain class, not a
second linked AIKeychainStore class. Installed alert UI was not exercised.

The normal release IPA with `dc85658` was installed on koolphone5 at the user's
request using appinst; installation succeeded and the transferred IPA and
installed executable hashes were verified. Stage/log:
`~/tmp_altivec/enil-keychain-fixed-20261020`. New app container:
`/var/mobile/Containers/Bundle/Application/52257E06-8425-4B23-8978-8332DE9D1C69`.
IPA SHA256: `a4e7b1d34419b86868a33fdb478561d981cf17956a944fafde237b56a88d6021`.
Executable SHA256: `daddb2975c1a5079cf5219c34d7a0760fd409dcdbad3a1e4ebc1113e59efb432`.
The agent did not launch ENIL or run sync.
The user confirmed koolphone5 automatically restored its existing keychain
settings. At their subsequent request the same IPA was installed on gomadango
using appinst. Installation succeeded; IPA and installed executable hashes match
those above. Stage/log: `~/tmp_altivec/enil-keychain-fixed-20261020`.
New gomadango container:
`/var/mobile/Applications/635B001B-679F-42A6-AA12-2AAE086F3F9D`.
The agent did not open ENIL or run sync. Gomadango user testing is pending.

## October 20 iOS release installation

The user explicitly authorized installing the fresh release IPA on gomadango,
superseding the earlier phone-install restriction below. `make iOS-release
 iOS-validate-package` passed. The normal 1.1.1 IPA includes both image fixes.
`appinst` reported successful installation; installed executable SHA256 matches
the local release executable. ENIL was not launched and no live sync ran.

Stage: `~/tmp_altivec/enil-image-fix-20261020` on gomadango, containing the IPA,
installer log, account checksums, and `previous-container.tar` (full prior app
container backup). iOS relocated the container from
`BCBDC1A2-E6FF-49EF-BC52-9F6A2C61F26F` to
`FECC5C94-9D7F-4E78-974C-87A8971705FD` under `/var/mobile/Applications`.
Database, WAL, SHM, and session.json checksums all match after relocation.

- IPA SHA256: `108db809c8a7a103c09b5a617598f4ad8856fc5fef3d6b460cc53356d0240d45`
- Executable SHA256: `6e8a23ebd005ca7d476671bf0a6e0a6e99cee9da079b68fabba82f858a2bc8d3`

Camera-roll sends use shared image processing, so the validation fix may also
resolve that reported failure; this has not yet been verified on the phone.

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
  installing the first fixed Mac build but explicitly said not to open it.
  The user later opened/synced it themselves, then quit both apps for a
  requested read-only phone-cache recovery attempt. Do not launch either app
  or run a production sync as part of tests. See the records below.
- The two devices use different LINE client identities (Chrome versus desktop)
  for the **same account**. Different account-directory or chat IDs do not mean
  different users. Match images by message ID, then use the destination
  database's `chat_id` to construct its media paths.
- Follow the root Git instructions: local commits as Strappy, no push.

## Encrypted-size fix and final recovery result

The fresh phone audit found originals/thumbnails for only 15 of the 30 pending
IDs. Those originals are byte-identical to files already in the Mac's correct
chat directories. The other 15 IDs have no matching originals or thumbnails
in the phone media cache. Both fresh SQLite snapshots and transferred media
were checksum-verified; no production files or database values were changed.
Evidence is private under
`build/tests/image-regression/private/phone-recovery-20261020/`.

Root cause: `build_e2ee_image_metadata` writes encrypted byte length (including
the 32-byte HMAC) into `MEDIA_CONTENT_INFO.fileSize`; the historical
`media_plain_size` field stores that unchanged. Other saved encrypted messages
already use plaintext byte length, so subtracting 32 unconditionally is wrong.

`enil_sync.c` now accepts either exact plaintext length or exactly 32 fewer
bytes for messages with nonempty `ENC_KM`. Both cached-image reuse and
post-download verification use the same check. Unencrypted messages remain
strict; full image validation and authenticated download decryption remain
required. No sender-format changes, schema migration, or metadata rewriting.

- Portable regression failed before the fix and passes after it. It exercises
  cached originals and fresh downloads with plaintext/wire sizes, rejects
  adjacent wrong sizes and unencrypted tag allowances, checks thumbnail
  regeneration, metadata clearing on failure, and cursor preservation.
- Native Leopard/PPC fixture: **115 images; 85 complete before; 100 complete
  after; 15 unavailable offline**. Explicit `--expect-complete=100` passed.
  Deliberate thumbnail deletion/regeneration and the representative message
  pane/original-link check also passed. No production account access or
  network requests were allowed by the sandbox.
- All 16 database tables matched the prior recovered fixture outside the six
  image path/dimension fields; exactly 15 additional image rows were repaired.
- `make test-host`: 100 Python and 57 Worker tests passed.
- `make clean` then `make debug release`: passed for all four Mac and both
  iOS architectures, both configurations, with no compiler warnings. Clean
  targets preserved `build/tests/` and its private fixtures.
- Both platform package validations passed. iOS has cross-build coverage,
  not native runtime coverage of this fix.
- Both analyzers completed with the same four existing findings: possible
  null input in `enil_asset_cache.c:39`, possible nil dictionary key in
  `QRLoginViewController.m:395`, and unused assignments in
  `ENILAccount.m:211` and `enil_db.c:1051`. No new findings.

Native fixtures and logs:
`~/Desktop/enil-size-regression-20261020` on `imac-rsa`.
Local private results:
`build/tests/image-regression/private/size-recovery-20261020/`.
Build/test logs have the `size-` prefix in `build/tests/image-regression/`.
Current normal release packages under `build/macOS` and `build/iOS` include
this second fix; the Mac package is now installed as recorded below.
Version metadata is still 1.1.1; this is not a published release.

## Encrypted-size fix installation

The user explicitly requested direct replacement in Applications, without an
installer script. `/Applications/ENIL.app` now contains the normal release
build with `e2aec94`. The transferred ZIP and installed executable checksums
were verified; ENIL was confirmed closed before and after replacement.
No agent-run live sync occurred.

Staging/rollback directory:
`~/Desktop/enil-size-fix-install-20261020`.
The prior installed bundle is `previous-ENIL.app` there.
ZIP SHA-1: `5030f553b14a94d103f719175bc479adbd1a5fb7`.
Installed executable SHA-1: `509614e73ca7f38a815dcef9108195a63140481d`.
Expected next-sync unavailable count is 15, assuming unchanged cache/data,
based on the offline native result of 100 complete images out of 115.

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

2. The user confirmed 15 missing on the Mac. Historical next-sync
   result; offline native testing recovered the 15 size-mismatched originals.
   Do not install a test harness.

3. Representative message-pane rendering now passes. Broader application
   lifecycle and live post-fix sync remain untested. Do not inject saved login
   sessions into offline tests or inadvertently connect the account to LINE.

4. Leopard coverage is now established on `imac-rsa`; `x4-vm` proves Tiger
   behavior only. Consider a modern Mac and an approved test iOS device for
   shared-code regression coverage. iOS has cross-build coverage, not native
   runtime coverage of this fix. Production phone testing is not authorized.

5. Investigate the remaining 15 missing originals. Neither the Mac fixture
   nor the fresh phone media cache has those message IDs. Historical 404
   responses do not prove expiration. The size mismatches are now fixed.

6. Coordinate any phone update, agent-run live sync, or published release with
   the user. The phone has not been updated.
   Normal fixed Mac package: `build/macOS/release/ENIL/ENIL.zip`.
   Normal fixed iOS package: `build/iOS/release/ENIL/ENIL.ipa`.
   These are local builds of the new commit; the version metadata was not
   bumped and they are not a new published tag. Never deploy the test harness
   as the user's app. Nothing has been pushed.
