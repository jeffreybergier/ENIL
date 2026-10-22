# ENILBulletinProbe

Read /altivec/templates/AGENTS.md for app-development defaults inside the image;
these project-specific instructions take precedence.
Use root Make targets and declarative products; build mechanics belong in Altivec.
Keep C logic in source/shared, Objective-C facades narrow, Mac UI MRC and iOS UI ARC.
Prefix compatibility helpers XP. Use bracket syntax and explicit retain/release in MRC.
Run make test-host and make check-project first, then selected platform builds,
analyze and validate-package. Device tests are explicit and use caller-supplied
NATIVE_HOST, NATIVE_STAGE and NATIVE_PLATFORM; never infer device validation from a cross-build.
Do not change application code automatically when updating a template.
