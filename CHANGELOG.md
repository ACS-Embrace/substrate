# Changelog

What each published version of Embrace's Substrate contains. Versions are `major.minor.patch-embrace`
(see "Versions and releases" in README.md): major when Embrace's builds must change to use it, minor
for a new feature they can take without changes, patch for fixes only. The `-embrace` marker keeps
our versions apart from Gluon's, which are published under the same name (`com.gluonhq:substrate`).

## 1.0.0-embrace

First version under this scheme; it replaces the `0.1.x-SNAPSHOT` versions.

**Based on** Gluon Substrate 0.0.68: our history last met Gluon's at "Prepare development of 0.0.69"
(October 2025). Gluon changes taken since, one by one: #1348, #1362, #1367 and #1368.

- **Android keyboard:** the activity's input connection is the AttachExtended keyboard module's
  mirrored one, so the soft keyboard can follow the focused field (needs AttachExtended 5.0.0).
- **Android:** the native long-press context menu is turned off in favour of Embrace's own;
  16 KB memory pages for Android 15; edge-to-edge handled, and the notifications permission
  requested; target SDK 35.
- **Android stability:** `eglSwapBuffers` is wrapped so the JavaFX render thread stops driving the
  GPU after the surface is destroyed, with related Android 16 crash fixes (JNI re-entrancy during
  surface teardown, a cold-start `onActivityResult` abort, a null Intent from cancelled pickers);
  the substrate library is loaded before surface creation to avoid a rare `UnsatisfiedLinkError`.
- **Android WebView:** the bridge is routed by instance handle, with JNI lifetime and geometry
  fixes; silent page load failures are reported and the page's console is logged; an
  `embraceNative` JavaScript bridge for page-to-Java calls.
- **iOS:** the minimum iOS version is read from the project's plist, with 15.0 as default and floor
  (#1362); a scene manifest and default scene (#1367); apps installed and run with `devicectl`
  (#1368); code signing uses the provisioning profile given, when one is set (#1348); the
  QuickLook and UniformTypeIdentifiers frameworks are linked.
- **Build:** Embrace's patched JavaFX static SDK is referenced by default; Java 17 toolchain.
