# Shared Keychain migration

The native credential facade now uses AltivecCocoa's `AIKeychainStore`.
Build against an Altivec engine containing this class and its public header;
the earlier published image alone does not contain this new implementation.
Existing public app APIs and credential names remain unchanged. No saved
credentials are renamed or rewritten just by launching the app.

`XPKeychain` remains an app policy facade. Internet credentials retain the
legacy account-only discovery and URL attributes. Same-scope edits update in
place; URL changes retain the existing delete-and-replace policy. Mac deletion
removes the first match; iOS iterates account matches with a 128-attempt race
bound. Unknown native protocols/authentication types fail without rewriting
credentials. Environment overrides, caching and login behavior remain app-owned.

Validation on 2026-10-15: clean Mac/iOS debug and release builds, the app's
complete offline suite, and isolated consumer-facade tests on the host Mac
and `x4-vm` (Tiger 10.4.11/PPC) passed. Shared tests are in the sibling engine's
`libs/cocoa/tests/test_keychain_consumers{,_cross}.sh`; they exercise production
facades using disposable Keychains. rCloud's existing-item ACL-edit call is
recorded to avoid an interactive native authorization prompt. Its actual ACL
edit authorization is still unverified; its storage operations are native.
`koolphone5` was unreachable, so phone runtime tests were skipped with user
approval. Cross-build success is not proof of iPhone runtime behavior.

No installed app or existing user credentials were changed by validation.
The engine's `docs/keychain.md` records API choice, source reduction and full
coverage limits. No image was rebuilt or published for this migration.

## Temporary iOS override (2026-10-20)

AltivecIntelligence commit `00f985c` fixes discovery and matching of legacy iOS
internet-password entries whose authentication type is empty. The old ENIL
writer omitted this attribute; treating it as explicit Default or rejecting it
prevents reading the old item and can block ENIL's replacement-save policy.

Until that engine version is available in the container, iOS builds compile
`source/iOS/ENILTemporaryKeychainStore.m` under MRC. Its implementation is copied
from that commit, with only a prologue and header import/class-name substitution.
`XPFoundation.m` maps the class name to `ENILTemporaryKeychainStore` on iOS only.
This avoids conflicting with the container's existing Objective-C class and
requires no engine binary/header changes. Mac builds retain their existing path.

After updating the container, remove the temporary source, its entry in the iOS
Makefile's shared source group, and the temporary class-name macro in
`XPFoundation.m`. Rebuild and rerun the legacy persistence test before shipping.
Do not remove the override based only on a newer container tag: verify it
contains the engine fix.

Native koolphone5 validation passed using the production facade, temporary
implementation and normal ENIL entitlements. The test seeds an old-style entry,
reads it, saves changed credentials, replaces the URL, then reads from a second
process and removes the fixture. It never accesses the real `shared-secret`
account. Build the isolated fixture from the repository root:

```sh
clang -target armv7-apple-ios5.0 \
  -isysroot /osxcross/modern/SDK/iPhoneOS8.4.sdk -B/osxcross/modern/bin \
  -Wall -Wextra -Werror -Wno-deprecated-declarations \
  -Wno-semicolon-before-method-body \
  -I/altivec/libs/cocoa/build-phone/include -Isource/shared \
  -framework Foundation -framework Security \
  source/iOS/ENILTemporaryKeychainStore.m source/shared/XPFoundation.m \
  source/shared/ENILKeychain.m source/tests/keychain_ios_test.m -o build/tests/keychain-research/facade
ldid -Ssource/iOS/Entitlements.plist build/tests/keychain-research/facade
```

Stage below `~/tmp_altivec` on the authorized test phone, then run `./facade seed
org.enil.keychain-test.<unique-run>` and `./facade read
org.enil.keychain-test.<same-run>` as separate invocations. A failed/interrupted
seed can leave its synthetic fixture; the read/failure path cleans that exact
account. Force-quit/relaunch through the actual installed UI remains a user
verification step.

The facade test also checks missing/invalid inputs separately from injected
native access failures, verifies OSStatus propagation into the shared alert
message, and confirms a failed save leaves the existing synthetic secret intact.
If the device rejects an overwritten executable with signal 9, sign the staged
fixture on-device using `ldid -SENIL.entitlements facade`, where ENIL.entitlements
is a copy of `source/iOS/Entitlements.plist`.

## Save errors on both platforms

`XPKeychain` and `ENILKeychain` now expose NSError-returning save variants.
Missing fields and malformed/non-HTTP(S) URLs return the validation domain with
a localized explanation. Native lookup, delete and write failures preserve
`NSOSStatusErrorDomain` and the actual status code. An access failure during
lookup aborts the save before replacement/deletion. Messages contain no secret.
The Mac NSAlert and the iOS Add Account/Done alerts display the shared explanation;
for example, a denied interaction produces `Keychain error -25308` instead of
asking the user to correct a valid URL. The iOS footer retains the same message.
