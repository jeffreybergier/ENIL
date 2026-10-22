# ENIL BulletinBoard probe

An isolated, template-generated UIKit IPA for investigating pre-UserNotifications
APIs. It does not link ENIL, load accounts, or alter ENIL's notification backend.
Bundle ID: `com.jeffburg.enil.bulletinprobe`; Home Screen name: **BB Probe**.

## Build

Generated with `altivec-app new --template ios-basic`. Use the existing Altivec
SDK/toolchain and declarative Make targets:

```sh
make test-host check-project
make iOS-debug iOS-release iOS-analyze iOS-validate-package ARCHS=armv7
```

IPA: `build/iOS/release/ENILBulletinProbe/ENILBulletinProbe.ipa`.
Matching debug symbols are in the same product directory. The armv7 deployment
floor is iOS 5; native execution on iOS 5 or 6 is not established by that floor.
The generated portable tests exercise the template model, not BulletinBoard.

## Device workflow

Use an explicitly chosen device and fresh staging directory below `~/tmp_altivec`,
following `/altivec/docs/deployment.md`. Install with the device's IPA installer;
do not copy into system application directories. For example:

```sh
ssh koolphone5 'mkdir -p ~/tmp_altivec/enil-bulletin-probe-RUN'
scp -O build/iOS/release/ENILBulletinProbe/ENILBulletinProbe.ipa \
  koolphone5:tmp_altivec/enil-bulletin-probe-RUN/
ssh koolphone5 'appinst ~/tmp_altivec/enil-bulletin-probe-RUN/ENILBulletinProbe.ipa'
ssh koolphone5 'uiopen enil-bulletin-probe://inspect'
```

Unlock the phone before launching. Inspection runs automatically on launch and
can be repeated using **Inspect**. Logs append to
`Documents/bulletin-probe.log` in this app's data container and to the system log.
Find the container again after each installation; its UUID can change.

| Button / URL host | Operation |
| --- | --- |
| Inspect / `inspect` | Load BulletinBoard with `dlopen`; enumerate selected classes and method encodings. No server or observer instances. |
| Public +15s / `public` | Request public notification permission when available; schedule a synthetic `UILocalNotification`. Press Home after accepting permission. |
| Private +8s / `publish` | Retain one synthetic request with this app's section ID and unique publisher/record IDs; invoke `publish` after eight seconds. Press Home first. |
| Withdraw / `withdraw` | Invoke `withdraw` on that retained request; cannot withdraw an earlier process's request. |
| Clear public / `clear` | Cancel this test app's scheduled local notifications and set its badge to zero. |
| Public x3 / `batch` | Schedule three distinctly labeled notifications at +15/+18/+21 seconds, with badge 3. |
| Badge only / `badge` | Set the badge to zero without cancelling any notification. Log the before/after badge count. |
| Cancel all / `cancel` | Call `cancelAllLocalNotifications` without writing the badge. |
| Cancel one / `cancel-one` | Cancel the oldest saved public request from this process, without writing the badge. This is not a delivered-inventory lookup. |
| Pending / `pending` | Log `scheduledLocalNotifications`; these are pending requests, not delivered notifications. |
| Delivered / `list` | Create a main-queue `BBObserver`, request this app's list, and display observed bulletins. No callback within five seconds means unknown, not empty. |
| Clear selected / `clear-one` | Clear only the selected observed bulletin with `clearBulletins:inSection:`. Requires a successful read and row selection. No badge write or public fallback. |

URLs use `enil-bulletin-probe://HOST`. Commands are diagnostic triggers with no
caller-supplied content, section, or identifier. They never address another app.
The app deliberately does not clear notifications automatically when activated,
so returning to the UI does not confound removal experiments.

Private invocations verify return and argument types and use `NSInvocation`.
The framework remains loaded for process lifetime. Private publish has a bounded
background-task allowance; expiration cancels delayed work. These protections do
not prevent private code from blocking or crashing. Calls returning normally are
logged as **unverified**, never delivery/removal success. There is no automatic
public fallback that could obscure a failed private test or duplicate delivery.

## Comparing the clearing operations

Create a fresh **Public x3** batch for each operation, press Home, and wait at
least 22 seconds. Confirm the three entries visually before returning to the app.
Test **Badge only**, **Cancel all**, and **Cancel one** independently; inspect
both the badge and Notification Center after each. Reusing an already cleared
batch would confound the comparison. The OS may itself change badge/tray state
in response to cancellation; "no badge write" describes the probe's actions.

For the private path, tap **Delivered** after the public batch has appeared. If
rows arrive, tap one and then **Clear selected**. Check that only that entry is
removed, then request a fresh list. The table holds at most 100 own-section
bulletins and deduplicates their IDs; it is an observed view, not proof of a
complete snapshot. No returned objects means private clearing cannot proceed.
The probe never substitutes saved/scheduled requests for observed bulletins.

The observer is invalidated and detached before each new read and on teardown.
Callbacks are marshalled to the main thread and checked against the current
observer instance. Other apps' bulletin contents are neither retained nor logged.
Runtime protocol metadata is used for callback ABI checks where available. On
8.4.1 that metadata is absent; known callback declarations are used with the
runtime `observerFeed` integer width verified. See the
[reference delegate declarations](https://github.com/nst/iOS-Runtime-Headers/blob/master/protocols/BBObserverDelegate.h).
Private methods still require matching signatures; no feed masks or privileged
entitlements are added. Connection events, invalidated IDs, timeouts, and actual
bulletin callbacks are logged separately.

## Koolphone5 results — 2026-10-22

Device: iPhone5,2, armv7, iOS **8.4.1 (12H321)**. Installed through `appinst`,
launched through SpringBoard as uid 501 with an app data container. The IPA's only
explicit entitlement is its application identifier. This jailbroken environment
(including injected tweaks) is not evidence of stock-iOS sandbox behavior.

- Portable template tests, metadata/artwork checks, debug/release cross-builds,
  static analysis, and release IPA validation passed. Final build: no compiler
  warnings. Initial generic-function-pointer cast warnings were eliminated by
  signature-driven invocation; no warning suppression was added.
- `dlopen` succeeded. `BBBulletinRequest`, `BBBulletin`, `BBServer`, `BBObserver`,
  `BBDataProviderConnection`, `BBDataProviderProxy`, and `BBServerConduit` exist.
- `publish` and `withdraw` both advertise `v8@0:4` on this armv7 runtime.
- The user allowed notifications and confirmed the public control appeared.
- The user subsequently confirmed that **Clear public** removed the public
  notification. This validates the combined `cancelAllLocalNotifications` and
  badge-zero operation on this device; it does not isolate which operation
  removed the delivered notification or establish behavior with badges disabled.
- A private publish executed while backgrounded (`applicationState == 2`) and
  returned normally. The user saw only the public control, no private bulletin.
  The system log explains the result:

  ```text
  You can no longer publish a bulletin by calling -publish on it. You must go through the data provider functions.
  ```

- Withdrawal also returned normally, with this system diagnostic:

  ```text
  You can no longer withdraw a bulletin by calling -withdraw on it. You must got through the data provider functions.
  ```

- A subsequent foreground invocation also logged the same publish diagnostic.
  Neither call establishes successful IPC, authorization, delivery, or removal.

This confirms that selector availability alone is insufficient: these convenience
methods are retained but no longer perform their original operation on this OS.
It does **not** demonstrate an entitlement denial or establish iOS 6 behavior.

The next investigation is the provider registration/publication path (the runtime
exposes `BBDataProviderConnection.addDataProvider:` and
`BBDataProviderProxy.addBulletin:forDestinations:`), with the matching binary or
headers used to establish initialization, service identity, lifetime, and IPC
permissions before invocation. No destination masks or entitlements were guessed.
The expanded observer test creates and invalidates an observer successfully,
but receives `connection=disconnected` and no bulletin callbacks within five
seconds. Its initial feed is zero. The installed framework lacks runtime
`BBObserverDelegate` protocol metadata. Adding its required invalidated-ID
callback removed the earlier delegate-contract diagnostic, but did not establish
a connection. This is not proof of a particular entitlement denial: connection,
subscription, and authorization requirements need further investigation.
Consequently, private enumeration and selected-bulletin removal remain unproven.
Tap routing, actual iOS 6 testing, and stock-device behavior also remain open.

The user confirmed that **Badge only** removed both the badge and the delivered
notifications after a three-notification batch. This isolates badge-zero as
sufficient for tray clearing on this device; it is no longer only evidence for
the combined Clear public operation. The pending-list control reported all three
scheduled requests before their fire times. Cancellation-only results require
separate visual checks; private delivered reads must not be inferred from those
pending counts.

Raw logs from this run are retained locally under
`build/device/koolphone5-20261022/` and `build/device/koolphone5-buttons/`;
build artifacts and logs are ignored by Git.
Remote staging is `~/tmp_altivec/enil-bulletin-probe-20261022`. The installed test
app is retained for further experiments. Use the supported app uninstall UI or
installer to remove it; deleting staging does not uninstall the app.

Reference: [iOS 6 headers at the investigated commit](https://github.com/caughtinflux/iOS6-Private-Frameworks/tree/8c44c2416cc75b3344b5ad0a1595a506ce5130b0/BulletinBoard.framework).
