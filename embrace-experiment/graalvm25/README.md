# Experiment tooling: stock GraalVM 25 on mobile

Local experiment only. Not for release, not published to S3. Branch `experiment/graalvm25`.

This folder holds the scripts and the JDK patch that produced a working Android build of Embrace 1.4.6 on
stock GraalVM Community 25.0.5 (2026-11-16), so the same can be attempted for iOS on a Mac. The Android
scripts ran in WSL (Ubuntu 20.04) and have machine paths in them; read them as a record of the exact steps
and adapt them, do not run them blind.

## What the GraalVM 25 path needs, per platform

| Piece | Android (done) | iOS (to do) |
|---|---|---|
| GraalVM | stock `graalvm-community-jdk-25i5-25.0.5.0.3` for the build host | the macOS aarch64 download of the same release |
| Target type table (CAP cache) | `gen-android-cap.sh`: native-image emits query programs, NDK compiles them, they run on a phone over adb, their output is the `.cap` files | same idea; an iPhone will not run loose programs, so run the simulator builds with `xcrun simctl spawn` |
| GraalVM C support libraries | `svm-clibs-android.sh`: `libchelper`, the JVM shim and `libcontainer` from `oracle/graal` tag `vm-25.5.5.0.3` | `libchelper`, `jvm.posix` and `com.oracle.svm.native.darwin`; `libcontainer` is Linux only |
| JDK 25.0.5 static libraries | `jdk25ga-prepare.sh` + `jdk25-static.sh`: `openjdk/jdk25u` tag `jdk-25.0.5-ga` with the mobile build support applied (`jdk25.0.5-android-support.patch`) | same source; configure as Gluon does for iOS (below) |
| One combined library | `vmone-build.sh`: all JDK objects + the GraalVM C objects + a `JVM_GetManagement` stub, as `libvmone.a`; passed with `-Djavalibspath=<dir>` | Substrate links `vmone` for iOS too; inspect the existing iOS `24-2.1` folder for its layout first |
| Substrate | this branch | this branch, plus an iOS type table |
| Build settings | `-Dsubstrate.android.isolates=true`, native-image arg `-H:-ForeignAPISupport`, `-Djavalibspath=...` | `-H:-ForeignAPISupport`, `-Djavalibspath=...`; iOS device builds already use isolates (backend `lir`) |

## Things learned the hard way on Android

- Use a plain OpenJDK as boot and build JDK for the JDK build. GraalVM prints class-data-sharing errors on
  stdout that end up inside generated source files.
- For a cross-compile the build JDK must be the same feature version as the source.
- The fork-point JDK 25 source is too old for GraalVM 25.0.5: natives were added later
  (`Console.ttyStatus`, `Net.shouldShutdownWriteBeforeClose0`). Use the release tag.
- Before any device run, list the undefined symbols of the linked library and check each against what the
  platform and the bundle provide. It finds every missing native at once.
- GraalVM 25 rejects built-in libraries whose `JNI_OnLoad_<name>` reports less than JNI 1.8.
- Substrate could not parse a five-part Java version (`25.0.5.0.3`); fixed on this branch.
- GraalVM 25's Foreign Function feature is not cross-compile safe; switch it off.
- From GraalVM 23, SIGPIPE is only ignored when signal handling is enabled, which is off for library
  images. The iOS launcher on this branch ignores it explicitly.

## Gluon's iOS configure line for the static JDK libraries

From `gluonhq/mobile`, branch `gluon-24-2`, `.github/workflows/build-ios-macos.yml` (they used Xcode
14.3.1 and cups 2.3.6 headers):

```
bash configure --with-conf-name=ios-macos-aarch64 --enable-headless-only \
  --with-boot-jdk=<jdk> --openjdk-target=aarch64-macos-ios \
  --with-cups-include=/tmp/cups-2.3.6 \
  --with-sysroot=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk
make CONF=ios-macos-aarch64 static-libs
```

Applying the mobile patch to the release source rejected one macOS hunk
(`src/java.base/macosx/native/libjava/java_props_macosx.c`). It did not matter for Android and has to be
resolved by hand for iOS.

## Files

- `gen-android-cap.sh`: generate the Android type table on a phone.
- `svm-clibs-android.sh`: compile GraalVM's C support libraries for Android.
- `jdk25ga-prepare.sh`: fetch the JDK release source and apply the mobile support.
- `jdk25-static.sh`: configure and build the Android static libraries.
- `vmone-build.sh`: assemble `libvmone.a`, build the probe, run the symbol check.
- `jdk25.0.5-android-support.patch`: the full JDK source change used for Android (44 files).
