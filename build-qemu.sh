#!/usr/bin/env bash
# Build the patched QEMU for the IC-705 emulator.
#
# Everything project-specific comes from this repository:
#   qemu/machine/   RZ/A1H + IC-705 device models, linked into hw/arm/
#   qemu/patches/   changes to QEMU itself (build glue, icount I/O speed-ups)
# Only the QEMU release itself is downloaded, at a pinned tag.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
root="$PWD"
qemu="${IC705_QEMU_SOURCE:-$HOME/ic705-qemu}"
version=v11.1.1

if [[ ! -d "$qemu/.git" ]]; then
    git clone --depth 1 --branch "$version" https://github.com/qemu/qemu.git "$qemu"
fi
if [[ "$(git -C "$qemu" describe --tags --exact-match HEAD)" != "$version" ]]; then
    echo "Expected QEMU $version in $qemu" >&2
    exit 1
fi

for patch in "$root"/qemu/patches/*.patch; do
    if git -C "$qemu" apply --ignore-space-change --reverse --check "$patch" 2>/dev/null; then
        continue                                    # already applied
    fi
    if ! git -C "$qemu" apply --ignore-space-change --check "$patch" 2>/dev/null; then
        # an older version of this patch is applied: restore its files first
        git -C "$qemu" apply --numstat "$patch" | cut -f3 | xargs git -C "$qemu" checkout --
    fi
    git -C "$qemu" apply --ignore-space-change "$patch"
done
for source in "$root"/qemu/machine/*.{c,h}; do
    ln -sfn "$source" "$qemu/hw/arm/${source##*/}"
done
ln -sfn "$root/qemu/ic705" "$qemu/hw/arm/ic705"
find "$qemu/hw/arm" -maxdepth 1 -xtype l -delete    # links to removed sources

mkdir -p "$qemu/build"
cd "$qemu/build"
if [[ ! -f build.ninja ]]; then
    ../configure --target-list=arm-softmmu --enable-gtk
fi
ninja -j "${IC705_JOBS:-$(nproc)}" qemu-system-arm
