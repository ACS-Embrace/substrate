#!/bin/bash
# First attempt at a JDK 25 libvmone.a for Android arm64:
#   all objects of our 24 JDK 25 static libraries, plus the 15 GraalVM-side members taken unchanged from
#   Gluon's 24-2.1 file (those come from GraalVM's own C sources and are still the GraalVM 23 versions).
# Then build the probe against it and list every symbol the resulting library cannot resolve on Android.
SP=$1
B=${JDKBUILD:-~/jdk-mobile/src25ga/build/android-linux-aarch64}
GL=~/.gluon/substrate/javaStaticSdk/24-2.1/android-aarch64/lib/libvmone.a
W=~/jdk-mobile/vmone
NDK=$(ls -d ~/.gluon/substrate/Android/ndk/* | sort -V | tail -1); T=$NDK/toolchains/llvm/prebuilt/linux-x86_64
AR=$T/bin/llvm-ar; NM=$T/bin/llvm-nm
OUT=$W/android-aarch64/lib
rm -rf "$W/obj" "$OUT"; mkdir -p "$W/obj/jdk" "$W/obj/svm" "$OUT"
# JDK objects: extract each library into its own folder so equal file names do not overwrite each other
for a in $(find "$B/support/native" -name '*.a' | sort); do
    d="$W/obj/jdk/$(basename "$a" .a)"; mkdir -p "$d"; ( cd "$d" && "$AR" x "$a" )
done
# GraalVM-side members: GraalVM 25's own C support libraries, compiled for Android by svm-clibs-android.sh.
# Four members of Gluon's file have no counterpart there and are kept from it (origin not traced).
cp ~/graal-forward/svm-clibs-android/libchelper/*.o ~/graal-forward/svm-clibs-android/jvm/*.o "$W/obj/svm/"
mkdir -p "$W/obj/container" "$W/obj/gluon"
cp ~/graal-forward/svm-clibs-android/libcontainer/*.o "$W/obj/container/"
( cd "$W/obj/gluon" && "$AR" x "$GL" AccessController.o SecurityManager.o dummy.o debug.o )
# Fallback for a JVM entry point that libmanagement references and GraalVM 25's JVM shim does not define.
# Substrate carries the same kind of stub for iOS (JvmFuncsFallbacks.c).
mkdir -p "$W/obj/fallback"
cat > "$W/obj/fallback/android_jvm_fallbacks.c" <<'EOC'
#include <stdio.h>
#include <stddef.h>
void* JVM_GetManagement(int version) {
    fputs("JVM_GetManagement called: unimplemented", stderr);
    fputc(10, stderr);
    return NULL;
}
EOC
"$T/bin/aarch64-linux-android28-clang" -c -fPIC -O2 -o "$W/obj/fallback/android_jvm_fallbacks.o" "$W/obj/fallback/android_jvm_fallbacks.c"
"$AR" rcs "$OUT/libvmone.a" $(find "$W/obj" -name '*.o' | sort)
echo "libvmone.a: $(stat -c %s "$OUT/libvmone.a") bytes, $("$AR" t "$OUT/libvmone.a" | wc -l) members (gluon 24-2.1: $(stat -c %s "$GL") bytes, 247 members)"
echo "defines Console_ttyStatus: $("$NM" "$OUT/libvmone.a" 2>/dev/null | grep -c ' T Java_java_io_Console_ttyStatus')"

G=$(cat ~/graal-forward/home.txt)
bash "$SP/probe/setup-and-build.sh" "$SP/probe" substratefix "$G" ce25-ga2 2.0.0-embrace 1.0.1-embrace-dev-jdk23 \
    "-Dsubstrate.android.isolates=true\ -Dprobe.extra.arg=-H:-ForeignAPISupport\ -Djavalibspath=$OUT" > /tmp/forward-probe.out 2>&1
grep -E "^mvn (exit|extra)|NO FRESH APK|tmpprobe-.*apk" /tmp/forward-probe.out | cut -c1-260
L=~/tmpfile-probe/logs/build-ce25-ga2.log
grep -n -E "Error:|Fatal error|undefined (symbol|reference)|ld.lld: error|BUILD (SUCCESS|FAILURE)" "$L" | grep -v -E "Re-run|MojoExec" | cut -c1-230 | head -20
grep -c "libvmone" "$L" | sed 's/^/log lines mentioning libvmone: /'; grep -o -E "[^ ]*javaStaticSdk[^ ]*|[^ ]*jdk-mobile/vmone[^ ]*" "$L" | sort -u | head -4

# every undefined symbol of the final library against what Android (API 28) and the APK provide
SO=$(find ~/tmpfile-probe/target -name 'libsubstrate.so' | head -1)
[ -f "$SO" ] || { echo "no libsubstrate.so"; exit 1; }
"$NM" -D --undefined-only "$SO" | awk '{print $NF}' | sed 's/@.*//' | sort -u > "$W/undef.txt"
: > "$W/provided.txt"
for so in "$T"/sysroot/usr/lib/aarch64-linux-android/28/*.so; do "$NM" -D --defined-only "$so" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//'; done | sort -u >> "$W/provided.txt"
for so in $(find ~/tmpfile-probe/target -name '*.so' ! -name libsubstrate.so); do "$NM" -D --defined-only "$so" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//'; done | sort -u >> "$W/provided.txt"
sort -u -o "$W/provided.txt" "$W/provided.txt"
comm -23 "$W/undef.txt" "$W/provided.txt" > "$W/missing.txt"
echo "libsubstrate.so undefined symbols: $(wc -l < "$W/undef.txt"); not provided by Android API 28 or the APK: $(wc -l < "$W/missing.txt")"
cat "$W/missing.txt" | head -60 | tr '\n' ' '; echo
