#!/bin/bash
# Prepare JDK 25.0.5 release source with the OpenJDK Mobile build support applied.
#   mobile patch = openjdk/mobile at its last JDK 25 commit, diffed against the mainline commit it merged.
R=~/jdk-mobile
cd "$R/mobile" || exit 1
M=$(cat ../last25.txt); P2=$(git log -1 --format=%P "$M" | cut -d' ' -f2)
git diff "$P2" "$M" > "$R/mobile-support-jdk25.patch"
echo "mobile patch: $(grep -c '^diff --git' "$R/mobile-support-jdk25.patch") files, $(wc -l < "$R/mobile-support-jdk25.patch") lines"
cd "$R" || exit 1
echo "--- release tags available"
git ls-remote --tags https://github.com/openjdk/jdk25u.git 'jdk-25.0.5*' | grep -v '\^{}' | awk '{print $2}' | sed 's|refs/tags/||' | sort -V | tail -6 | tr '\n' ' '; echo
TAG=${1:-jdk-25.0.5-ga}
if [ ! -d src25ga/.git ]; then
    git clone -q --depth 1 --branch "$TAG" https://github.com/openjdk/jdk25u.git src25ga > clone-ga.log 2>&1 || { echo "clone failed"; tail -3 clone-ga.log; exit 1; }
fi
cd src25ga || exit 1
echo "source: $(git describe --tags 2>/dev/null) $(git log -1 --format='%h %ad' --date=short)"
grep -E "DEFAULT_VERSION_(FEATURE|UPDATE)=" make/conf/version-numbers.conf | tr '\n' ' '; echo
if git diff --quiet; then
    git apply --check "$R/mobile-support-jdk25.patch" 2> "$R/apply-check.log" && { git apply "$R/mobile-support-jdk25.patch"; echo "mobile patch applied cleanly"; } || {
        echo "does not apply cleanly; applying with rejects"; git apply --reject "$R/mobile-support-jdk25.patch" > "$R/apply-reject.log" 2>&1
        echo "rejected hunks: $(find . -name '*.rej' | wc -l) files"; find . -name '*.rej' | head -10
    }
fi
git status --short | wc -l
for f in src/java.base/unix/native/libjava/Console_md.c src/java.base/unix/native/libnio/ch/Net.c; do echo "$f: ttyStatus $(grep -c ttyStatus $f), shutdownWrite $(grep -c shouldShutdownWriteBeforeClose0 $f)"; done
