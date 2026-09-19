# Additional LINE client identities

Review date: 2026-09-19. ENIL baseline: `56dd791`.

## Recommendation

Investigate three additional identities, in this order:

1. Mac (`DESKTOPMAC`)
2. iPad (`IOSIPAD`)
3. Android secondary (`ANDROIDSECONDARY`)

These could take ENIL from two profiles to five. They are feasibility candidates,
not verified additions. Mac is the clearest first choice for reusing the Windows
native transport. Apple Watch and Wear OS are two further research targets;
visionOS is an enum-only lead from this review.

## Extension evidence

A fresh download of the official LINE Chrome extension was byte-for-byte
identical to ENIL's cached archive:

- Extension ID: `ophjlpahpchlmihnnnihgmmeilfjmjjc`
- Version: `3.7.2`
- Archive SHA-256:
  `3722ab02edb0aba49c229399ee8df5b4b5a38c60b76efadadfbae395015eb859`
- Both JavaScript/WASM hashes pinned in
  `source/cloudflare/scripts/extension.json` matched.

The extension's `main.js` contains 34 base application types, or 133 values when
RC, beta, and alpha variants are included. The six candidates below appear only
in the application-type enum in the inspected JavaScript. The active Chrome QR
login uses `systemName: CHROMEOS` and `modelName: CHROME`; media requests use a
`CHROMEOS` application header.

An enum entry establishes a name and numeric ID. It does not establish that the
extension implements that client, that LINE still accepts it, or that ENIL can
use its authentication and messaging protocols.

## Candidate assessment

| Identity | Enum value | Assessment |
| --- | --- | --- |
| Mac (`DESKTOPMAC`) | 112 | Best next candidate. The Windows native transport provides a strong starting point; exact identity values and compatibility need validation. |
| iPad (`IOSIPAD`) | 304 | Credible candidate for native transport reuse. Validate secondary QR login, key recovery, and messaging behavior. |
| Android secondary (`ANDROIDSECONDARY`) | 528 | Credible candidate, with more uncertainty about login and synchronization differences. |
| Apple Watch (`WATCHOS`) | 496 | Research target. Pairing, authentication, and feature restrictions need investigation. |
| Wear OS (`WEAROS`) | 544 | Research target. Pairing, authentication, and feature restrictions need investigation. |
| visionOS (`VISIONOS`) | 560 | Enum-only lead in this review; insufficient evidence to estimate implementation. |

LINE officially documents Mac, iPad, and Android as secondary devices, including
QR login for iPad and Android. This supports prioritizing those three, but does
not prove compatibility with ENIL's native protocol implementation. Apple Watch
and Wear OS are also documented products with a more limited feature set.

Do not count legacy entries such as `WINPHONE` and `BLACKBERRY`, internal/service
types, or release-channel variants as viable additions solely because their
names remain in the enum. Different operating systems running the Chrome
extension also do not establish additional client identities.

## Existing implementation to reuse

ENIL currently has two profiles and two transports:

| Profile | Transport |
| --- | --- |
| Chrome (`CHROMEOS`) | `chrome-gateway` |
| Windows (`DESKTOPWIN`) | `native-thrift` |

The Windows implementation supplies native secure QR authentication, Compact
Thrift, LEGY framing, operation polling, token refresh, media identity handling,
and native-ID E2EE support. These are a starting point for other native clients;
per-client compatibility remains unverified.

Each session already persists its exact identity and transport. Pending-login
recovery compares complete identities before reusing saved attempts. Preserve
those guarantees when adding profiles, including the frozen defaults for legacy
Chrome sessions and the explicit transport of earlier Windows gateway sessions.

## Implementation plan

- [ ] Obtain and verify the candidate's application version, User-Agent, system
  name, and model name. The Chrome enum does not supply these values.
- [ ] Expand the two-profile registry and Windows-only native validation in
  `source/shared/enil_identity.c` into explicit per-profile metadata.
- [ ] Generalize Windows-specific defaults and messages in
  `source/shared/enil_native_login.c`, preserving the selected session identity.
- [ ] Add profile choices to the macOS and iOS login screens.
- [ ] Reuse transport-based routing where compatible; investigate any required
  differences in authentication, endpoints, schemas, or synchronization.
- [ ] Add meaningful regression coverage for profile persistence, routing, and
  recovery isolation; run the required native and Worker checks for changed code.
- [ ] Complete live validation before treating a candidate as supported.

Keep extension code out of the native clients. The Worker remains responsible
for crypto, while ENIL makes LINE requests directly.

## Validation for each candidate

- Secure QR login, PIN/certificate handling, and token issuance.
- Phone-side device classification, independently of the supplied device name.
- E2EE key recovery and encrypted message sending/receiving.
- Contacts, chats, operation polling, and reconnect behavior.
- Media upload/download and identity propagation.
- Access-token refresh and recovery after interrupted persistence.
- Pending-login reuse, reauthentication, and isolation from other profiles.
- Coexistence with Chrome, Windows, and the official candidate client.

More identities do not necessarily mean more simultaneous sessions. Mac and
Windows could share a server-enforced login category. Test coexistence explicitly;
neither enum values nor successful anonymous QR creation establish independent
login slots or full client compatibility.

This review performed static source inspection and an extension download. It did
not perform account logins or validate any additional identity against LINE.

## Base application-type inventory

| Name | Numeric value |
| --- | --- |
| IOS | 16 |
| ANDROID | 32 |
| WAP | 48 |
| BOT | 64 |
| WEB | 80 |
| DESKTOPWIN | 96 |
| DESKTOPMAC | 112 |
| CHANNELGW | 128 |
| CHANNELCP | 144 |
| WINPHONE | 160 |
| BLACKBERRY | 176 |
| WINMETRO | 192 |
| S40 | 208 |
| CHRONO | 224 |
| TIZEN | 256 |
| VIRTUAL | 272 |
| FIREFOXOS | 288 |
| IOSIPAD | 304 |
| BIZIOS | 320 |
| BIZANDROID | 336 |
| BIZBOT | 352 |
| CHROMEOS | 368 |
| ANDROIDLITE | 384 |
| WIN10 | 400 |
| BIZWEB | 416 |
| DUMMYPRIMARY | 432 |
| SQUARE | 448 |
| INTERNAL | 464 |
| CLOVAFRIENDS | 480 |
| WATCHOS | 496 |
| OPENCHAT_PLUG | 512 |
| ANDROIDSECONDARY | 528 |
| WEAROS | 544 |
| VISIONOS | 560 |

## Official references

- [LINE Chrome Web Store listing](https://chromewebstore.google.com/detail/line/ophjlpahpchlmihnnnihgmmeilfjmjjc)
- [Main and secondary device support](https://help.line.me/line/?contentId=20000132&lang=en)
- [iPad and Android secondary login](https://help.line.me/line/?contentId=20018574&lang=en)
- [Smartwatch features](https://help.line.me/line/smartphone?contentId=20024198&lang=en)
- [LINE system requirements](https://help.line.me/line/?contentId=10002433&lang=en)
