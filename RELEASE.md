# Release Versioning

ENIL uses one public app version across the macOS and iOS bundles.

- `source/macOS/Info.plist` `CFBundleShortVersionString` is the source value
  used when bumping a release.
- `CFBundleShortVersionString` in both `Info.plist` files must match.
- `CFBundleVersion` is the bundle build version and uses the same value.
- GitHub release tags should be the version with an optional `v` prefix, for
  example `v1.0` or `v1.2.3`.

To bump the patch version and publish a release:

```sh
altivec-release bump patch
```

The `/altivec/bin/altivec-release` helper reads `.altivec-release.yml` and
does all of this:

- reads the current version from the macOS `Info.plist`
- bumps the patch version, e.g. `1.2.3` -> `1.2.4`
- updates both keys in both `Info.plist` files
- commits the version bump
- tags it as `v<version>`
- pushes the current branch
- pushes the tag, which triggers the GitHub Action

Read the current version from `source/macOS/Info.plist`; a patch bump advances
the final version component. The migration preserves app version `1.0.5`.

To preview the git commands without changing anything:

```sh
altivec-release bump patch --dry-run
```

To commit and tag locally without pushing:

```sh
altivec-release bump patch --no-push
```

The GitHub Action refuses to publish release assets if the tag version does
not match the plist version. Release assets are named with the same version,
for example `ENIL-1.2.3-macOS.zip` and `ENIL-1.2.3-iOS.ipa`.

## ENIL repository setup

Run release commands from the ENIL repository root. The app release helper
and tag-triggered workflow package the native apps; the Cloudflare Worker
uses its own Workers Builds configuration under `source/cloudflare/`.

Configure these repository secrets on **ENIL** before running the native
release workflow; secrets from ENIL-cocoa do not move with source files:

- `ALTIVEC_SDK_MACOS_105_URL`
- `ALTIVEC_SDK_MACOS_113_URL`
- `ALTIVEC_SDK_IPHONEOS_84_URL`

Each points to the corresponding SDK archive described in the root README.
The workflow validates SDK inputs using `altivec-sdk` and fetches the pinned
QR-code submodule recursively. `.env`, SDK archives, generated headers,
`release.env`, and staged `dist/` packages stay out of version control.

This migration preserves the current app version, but does not migrate old
Git tags or GitHub release assets from ENIL-cocoa.
