#!/usr/bin/env bash
#
# Build a throwaway pacman root arranged to make libalpm do the three things
# the bridge has never been made to do: ask a question, run a scriptlet, and
# run a hook. The last two are fork()+exec() inside a chroot, which is the
# reason the server is single-threaded.
#
# Run under MSYS2 bash. Writes only into the directory it is given; the real
# MSYS2 installation is read from and never written to.
#
#   ./mkroot.sh /f/work/alpmrpc/build/scratch
#
# The packages are two that conflict by name, so installing the second while
# the first is installed is a question rather than an error. Both carry a
# scriptlet, and the root carries a hook, so a commit forks twice over.
set -euo pipefail

ROOT=${1:?usage: mkroot.sh <root>}
ARCH=x86_64
VER=1.0-1

# Absolute, because libalpm is handed these paths and a relative one would be
# resolved against wherever the server happens to be running.
ROOT=$(cd "$(dirname "$ROOT")" && pwd)/$(basename "$ROOT")

rm -rf "$ROOT"
mkdir -p "$ROOT"/{tmp,etc/pacman.d/hooks,var/lib/pacman/local,var/cache/pacman/pkg,usr/bin,bin,usr/share}

# libalpm reads this before it will touch a local db it did not create.
echo 9 > "$ROOT/var/lib/pacman/local/ALPM_DB_VERSION"

# --- a shell inside the root -------------------------------------------------
#
# Scriptlets and hooks run chroot()ed to the root, so /bin/sh has to exist
# *inside* it. bash needs nothing from MSYS2 but msys-2.0.dll, so a working
# shell is two files. /bin and /usr/bin are both populated rather than linked:
# a symlink is one more thing that has to survive the chroot, for no gain.
for d in bin usr/bin; do
	cp /usr/bin/bash.exe "$ROOT/$d/bash.exe"
	cp /usr/bin/bash.exe "$ROOT/$d/sh.exe"
	cp /usr/bin/msys-2.0.dll "$ROOT/$d/msys-2.0.dll"
done

# --- what the scriptlets and the hook leave behind ---------------------------
#
# Each writes a line to a file inside the root. Those files are the evidence
# that a fork actually happened, independent of anything the bridge reports.
cat > "$ROOT/alpmrpc-hook.sh" <<'EOF'
echo "hook ran" >> /alpmrpc-hook.log
EOF

cat > "$ROOT/etc/pacman.d/hooks/alpmrpc.hook" <<'EOF'
[Trigger]
Operation = Install
Operation = Remove
Type = Package
Target = alpmrpc-*

[Action]
Description = alpmrpc scratch hook
When = PostTransaction
Exec = /bin/sh /alpmrpc-hook.sh
EOF

# --- packages ----------------------------------------------------------------

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# mkpkg <name> <conflicts-or-empty>
mkpkg() {
	local name=$1 conflicts=${2:-}
	local stage="$WORK/$name"
	rm -rf "$stage"
	mkdir -p "$stage/usr/share/alpmrpc-scratch"

	echo "$name $VER" > "$stage/usr/share/alpmrpc-scratch/$name.txt"
	local size
	size=$(stat -c %s "$stage/usr/share/alpmrpc-scratch/$name.txt")

	{
		echo "pkgname = $name"
		echo "pkgbase = $name"
		echo "pkgver = $VER"
		echo "pkgdesc = alpmrpc scratch package"
		echo "url = https://example.invalid/alpmrpc"
		echo "builddate = 1750000000"
		echo "packager = alpmrpc scratch <nobody@example.invalid>"
		echo "size = $size"
		echo "arch = $ARCH"
		echo "license = MIT"
		[ -n "$conflicts" ] && echo "conflict = $conflicts"
	} > "$stage/.PKGINFO"

	# Printed lines become ALPM_EVENT_SCRIPTLET_INFO; the appended lines are
	# proof the shell really ran.
	cat > "$stage/.INSTALL" <<EOF
post_install() {
	echo "alpmrpc scriptlet: $name post_install \$1"
	echo "$name post_install" >> /alpmrpc-scriptlet.log
}

pre_remove() {
	echo "alpmrpc scriptlet: $name pre_remove \$1"
	echo "$name pre_remove" >> /alpmrpc-scriptlet.log
}
EOF

	local out="$ROOT/var/cache/pacman/pkg/$name-$VER-$ARCH.pkg.tar.zst"
	# .PKGINFO first, which is where libalpm expects to find it.
	bsdtar --zstd -cf "$out" -C "$stage" .PKGINFO .INSTALL usr
	echo "  $out"
}

echo "packages:"
mkpkg alpmrpc-base
mkpkg alpmrpc-rival alpmrpc-base

# The server is a Cygwin process, so every path handed to libalpm is a POSIX
# path on its side, while the test reads the scriptlet and hook logs itself
# and needs the Windows one. Bash knows both, so it writes the POSIX form
# down rather than leaving the test to guess at a drive-letter mapping.
printf '%s\n' "$ROOT" > "$ROOT.posix"

echo "root: $ROOT"
