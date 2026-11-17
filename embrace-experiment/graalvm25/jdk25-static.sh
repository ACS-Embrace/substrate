#!/bin/bash
# Build the JDK 25 static libraries for Android arm64 from openjdk/mobile, following Gluon's workflow
# (build-android-linux.yml on gluonhq/mobile). Stages: configure | make | status
R=~/jdk-mobile
SRC=${SRC:-$R/src25}
NDK=$(ls -d ~/.gluon/substrate/Android/ndk/* | sort -V | tail -1)
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
# Boot and build JDK: a plain OpenJDK 25. GraalVM as boot JDK prints class-data-sharing errors on
# stdout when the build runs its tools, and those lines end up inside generated source files.
if [ ! -x "$R/bootjdk/bin/java" ]; then
    mkdir -p "$R/bootjdk" "$R/dl"
    [ -s "$R/dl/jdk25.tar.gz" ] || wget -q -O "$R/dl/jdk25.tar.gz" "https://api.adoptium.net/v3/binary/latest/25/ga/linux/x64/jdk/hotspot/normal/eclipse"
    tar -xf "$R/dl/jdk25.tar.gz" -C "$R/bootjdk" --strip-components=1
fi
BOOT=$R/bootjdk
# Gluon's 24-2.1 Android library references posix_spawn, nl_langinfo and iconv, which need API 28.
API=28
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
case "${1:-configure}" in
configure)
    cd "$R" || exit 1
    [ -d "$SRC" ] || true
    cd "$SRC" || exit 1
    echo "source: $(git log -1 --format='%h %ad %s' --date=short)"
    echo "Console native: $(grep -rl 'ttyStatus' src/java.base | tr '\n' ' ')"
    echo "ndk: $(basename $NDK), boot/build jdk: $($BOOT/bin/java -version 2>&1 | head -1), host gcc: $(gcc -dumpversion)"
    # Header-only dependencies, fetched without root: apt-get download + dpkg -x into $R/hdr
    if [ ! -d "$R/hdr/usr/include/cups" ]; then
        mkdir -p "$R/hdr-dl"
        ( cd "$R/hdr-dl" && apt-get download libcups2-dev libfontconfig1-dev libasound2-dev libfreetype6-dev > apt.log 2>&1
          for d in *.deb; do dpkg -x "$d" "$R/hdr"; done )
    fi
    echo "local headers: $(ls "$R/hdr/usr/include" 2>/dev/null | tr '\n' ' ')"
    bash configure \
        --with-cups-include="$R/hdr/usr/include" \
        --with-fontconfig-include="$R/hdr/usr/include" \
        --with-alsa-include="$R/hdr/usr/include" \
        --with-alsa-lib="$R/hdr/usr/lib/x86_64-linux-gnu" \
        --with-conf-name=android-linux-aarch64 \
        --enable-headless-only \
        --with-boot-jdk="$BOOT" \
        --with-build-jdk="$BOOT" \
        --with-toolchain-path="$TC/bin" \
        --with-sysroot="$TC/sysroot" \
        --with-toolchain-type=clang \
        --with-jvm-variants=minimal \
        --build=x86_64-unknown-linux-gnu \
        --host=aarch64-linux-android \
        --target=aarch64-linux-android \
        --with-version-opt=embrace-experiment \
        --with-extra-cflags="--target=aarch64-linux-android$API" \
        --with-extra-cxxflags="--target=aarch64-linux-android$API" \
        --with-extra-ldflags="--target=aarch64-linux-android$API" \
        --disable-warnings-as-errors > "$R/configure.log" 2>&1
    rc=$?
    echo "configure exit: $rc"
    grep -n -E "^configure: error|error:|Could not|cannot|WARNING" "$R/configure.log" | cut -c1-220 | head -20
    tail -22 "$R/configure.log" | cut -c1-160
    ;;
make)
    cd "$SRC" || exit 1
    START=$(date +%s)
    echo "started $(date +%H:%M:%S)" > "$R/make.status"
    make CONF=android-linux-aarch64 static-libs > "$R/make.log" 2>&1
    echo "finished $(date +%H:%M:%S) rc=$? seconds=$(( $(date +%s) - START ))" >> "$R/make.status"
    ;;
status)
    cat "$R/make.status" 2>/dev/null
    grep -n -E "error:|Error [0-9]|\*\*\* |ERROR:|No rule to make" "$R/make.log" | cut -c1-240 | head -${2:-20}
    tail -5 "$R/make.log" | cut -c1-200
    find "$SRC/build/android-linux-aarch64" -name '*.a' 2>/dev/null | wc -l
    ;;
esac
