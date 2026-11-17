#!/bin/bash
# Compile GraalVM 25's own C support libraries (libchelper, the JVM shim, libcontainer) for Android arm64
# from the GraalVM source tag that matches the binary. Flags follow substratevm/mx.substratevm/suite.py.
set -u
S=~/graal-forward/graal-src/substratevm/src
G=$(cat ~/graal-forward/home.txt)
NDK=$(ls -d ~/.gluon/substrate/Android/ndk/* | sort -V | tail -1); T=$NDK/toolchains/llvm/prebuilt/linux-x86_64
API=28
CC="$T/bin/aarch64-linux-android$API-clang"; CXX="$T/bin/aarch64-linux-android$API-clang++"
OUT=~/graal-forward/svm-clibs-android
rm -rf "$OUT"; mkdir -p "$OUT/libchelper" "$OUT/jvm" "$OUT/libcontainer"
COMMON="-g -fPIC -O2 -ffunction-sections -fdata-sections -fvisibility=hidden -D_FORTIFY_SOURCE=0"
echo "sources: libchelper $(ls $S/com.oracle.svm.native.libchelper/src/*.c | xargs -n1 basename | tr '\n' ' ')"

fail=0
echo "=== libchelper"
for c in "$S"/com.oracle.svm.native.libchelper/src/*.c; do
    n=$(basename "$c" .c)
    "$CC" -c $COMMON -D_LITTLE_ENDIAN -I"$S/com.oracle.svm.native.libchelper/include" -I"$G/include" -I"$G/include/linux" \
        -o "$OUT/libchelper/$n.o" "$c" 2> "$OUT/libchelper/$n.err" || { fail=$((fail+1)); echo "  FAILED $n: $(grep -m1 'error' "$OUT/libchelper/$n.err" | sed -E 's|^.*/src/||' | cut -c1-170)"; }
done
echo "=== jvm shim"
for c in "$S"/com.oracle.svm.native.jvm.posix/src/*.c; do
    n=$(basename "$c" .c)
    "$CC" -c $COMMON -D_GNU_SOURCE -I"$S/com.oracle.svm.native.libchelper/include" -I"$G/include" -I"$G/include/linux" \
        -o "$OUT/jvm/$n.o" "$c" 2> "$OUT/jvm/$n.err" || { fail=$((fail+1)); echo "  FAILED $n: $(grep -m1 'error' "$OUT/jvm/$n.err" | sed -E 's|^.*/src/||' | cut -c1-170)"; }
done
echo "=== libcontainer (flags from suite.py)"
L=$S/com.oracle.svm.native.libcontainer/src
sed -n '1105,1128p' ~/graal-forward/graal-src/substratevm/mx.substratevm/suite.py | cut -c1-190
for c in $(find "$L" -name '*.cpp'); do
    n=$(basename "$c" .cpp)
    "$CXX" -c -O2 -fno-rtti -fno-exceptions -fvisibility=hidden -fPIC -DNATIVE_IMAGE -DLINUX -DINCLUDE_SUFFIX_COMPILER=_gcc \
        -D_GNU_SOURCE -std=c++14 \
        -I"$L/java.base/share/native/include" -I"$L/java.base/unix/native/include" -I"$L/hotspot" -I"$L/hotspot/share" \
        -I"$L/hotspot/os/linux" -I"$L/hotspot/os/posix" -I"$L/hotspot/os/posix/include" -I"$L/svm" -I"$L/svm/share" \
        -o "$OUT/libcontainer/$n.o" "$c" 2> "$OUT/libcontainer/$n.err" || { fail=$((fail+1)); echo "  FAILED $n: $(grep -m1 'error' "$OUT/libcontainer/$n.err" | sed -E 's|^.*/src/||' | cut -c1-190)"; }
done
echo "objects: libchelper $(ls $OUT/libchelper/*.o 2>/dev/null | wc -l), jvm $(ls $OUT/jvm/*.o 2>/dev/null | wc -l), libcontainer $(ls $OUT/libcontainer/*.o 2>/dev/null | wc -l); failed: $fail"
