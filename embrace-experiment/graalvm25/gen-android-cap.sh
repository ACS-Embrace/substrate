#!/bin/bash
# Generate the Android arm64 CAP cache (target type table) for a given GraalVM, using a connected phone.
#
#   1. native-image emits its query programs (C source) for the target, without running them.
#   2. Each is compiled with the Android NDK for arm64.
#   3. Each is pushed to the phone over adb and run there; its output is the .cap file.
#
# Usage: gen-android-cap.sh <graalvm-home> <out-dir> [android-api-level]
set -u
G=$1; OUT=$2; API=${3:-23}
NDK=$(ls -d ~/.gluon/substrate/Android/ndk/* | sort -V | tail -1)
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang
ADB=${ADB:-/mnt/c/platform-tools/adb.exe}
DEV=/data/local/tmp/capq
mkdir -p "$OUT/query" "$OUT/bin" "$OUT/cap" "$OUT/work"
rm -f "$OUT"/query/* "$OUT"/bin/* "$OUT"/cap/*
echo "graalvm: $("$G/bin/native-image" --version 2>&1 | head -1)"
echo "ndk: $(basename "$NDK"), compiler: $(basename "$CC"), device: $("$ADB" shell getprop ro.product.model | tr -d '\r') API $("$ADB" shell getprop ro.build.version.sdk | tr -d '\r')"

# 1. query programs for the target
cd "$OUT/work" || exit 1
[ -f Hello.class ] || { printf 'public class Hello { public static void main(String[] a) { System.out.println("hi"); } }\n' > Hello.java; "$G/bin/javac" Hello.java; }
"$G/bin/native-image" -H:+UnlockExperimentalVMOptions -H:+SharedLibrary -H:-SpawnIsolates \
    -Dsvm.targetArch=aarch64 '-Dsvm.platform=org.graalvm.nativeimage.Platform$LINUX_AARCH64' \
    -H:+ExitAfterQueryCodeGeneration -H:QueryCodeDir="$OUT/query" -cp . Hello -o hello > "$OUT/gen.log" 2>&1
echo "query generation: $(ls "$OUT/query" | wc -l) files ($(grep -c -i "error" "$OUT/gen.log") error lines in the log)"
[ "$(ls "$OUT/query" | wc -l)" -gt 0 ] || { tail -15 "$OUT/gen.log"; exit 1; }

# Items glibc has and Android's bionic does not (dlmopen link-map namespaces). native-image still asks
# for them on a Linux platform; they are unused on Android. Values are glibc's.
cat > "$OUT/work/bionic-shim.h" <<'EOF2'
#ifndef LM_ID_BASE
typedef long Lmid_t;
#define LM_ID_BASE 0
#define LM_ID_NEWLM (-1)
#define RTLD_DI_LMID 1
#endif
EOF2

# 2 and 3. compile, run on the device
"$ADB" shell "rm -rf $DEV && mkdir -p $DEV"
ok=0; bad=0
for src in "$OUT"/query/*.c; do
    n=$(basename "$src" .c)
    if "$CC" -I"$G/include" -I"$G/include/linux" -include "$OUT/work/bionic-shim.h" -o "$OUT/bin/$n" "$src" 2> "$OUT/bin/$n.err"; then
        "$ADB" push "$(wslpath -w "$OUT/bin/$n")" "$DEV/$n" > /dev/null 2>&1
        "$ADB" shell "chmod 755 $DEV/$n && $DEV/$n" | tr -d '\r' > "$OUT/cap/$n.cap"
        lines=$(grep -c "NativeCodeInfo" "$OUT/cap/$n.cap")
        echo "  $n: compiled, ran, $lines entries"
        ok=$((ok+1))
    else
        echo "  $n: COMPILE FAILED ($(grep -c "error:" "$OUT/bin/$n.err") errors) first: $(grep -m1 "error:" "$OUT/bin/$n.err" | sed -E 's|^.*/query/||' | cut -c1-150)"
        bad=$((bad+1))
    fi
done
"$ADB" shell "rm -rf $DEV"
echo "programs run: $ok, failed to compile: $bad, total entries: $(cat "$OUT"/cap/*.cap 2>/dev/null | grep -c NativeCodeInfo)"
