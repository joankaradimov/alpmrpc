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

# This runs under MSYS2's bash but is launched from CMake, so the PATH it
# inherits is whatever configured the build -- which is a Windows one and
# need not have /usr/bin on it. bsdtar was found there anyway; repo-add was
# not, and the repo then quietly did not exist.
PATH=/usr/bin:/bin:$PATH

ROOT=${1:?usage: mkroot.sh <root>}
ARCH=x86_64
VER=1.0-1
REPO=alpmrpc

# Absolute, because libalpm is handed these paths and a relative one would be
# resolved against wherever the server happens to be running.
ROOT=$(cd "$(dirname "$ROOT")" && pwd)/$(basename "$ROOT")

rm -rf "$ROOT"
mkdir -p "$ROOT"/{tmp,etc/pacman.d/hooks,var/lib/pacman/local,var/cache/pacman/pkg,var/log,usr/bin,bin,usr/share}

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

# mkpkg <name> <where: cache|repo> <conflicts-or-empty>
#
# A package in the cache is installable from a file; one only in the repo has
# to be downloaded first, which is the difference between exercising the
# download callbacks and not.
mkpkg() {
	local name=$1 where=$2 conflicts=${3:-}
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

	local file="$name-$VER-$ARCH.pkg.tar.zst"
	# .PKGINFO first, which is where libalpm expects to find it.
	bsdtar --zstd -cf "$WORK/$file" -C "$stage" .PKGINFO .INSTALL usr

	# Everything is in the repo, so it can be installed by name; only some
	# of it is in the cache, so the rest has to be fetched.
	cp "$WORK/$file" "$ROOT/repo/$file"
	if [ "$where" = cache ]; then
		cp "$WORK/$file" "$ROOT/var/cache/pacman/pkg/$file"
	fi
	echo "  $file ($where)"
}

mkdir -p "$ROOT/repo"

echo "packages:"
mkpkg alpmrpc-base  cache
mkpkg alpmrpc-rival cache alpmrpc-base
mkpkg alpmrpc-extra repo
# Never installed and never cached: something for alpm_fetch_pkgurl to fetch
# that has not already been fetched by something else.
mkpkg alpmrpc-spare repo

# A file:// repo, so the download path runs with no network at all. repo-add
# leaves alpmrpc.db as a symlink to the tarball; that is replaced with a copy
# because what reads it is libcurl, through the Cygwin file:// handler, and a
# link is one more thing that has to survive that.
repo-add --quiet "$ROOT/repo/$REPO.db.tar.gz" "$ROOT/repo"/*.pkg.tar.zst \
	> /dev/null
rm -f "$ROOT/repo/$REPO.db"
cp "$ROOT/repo/$REPO.db.tar.gz" "$ROOT/repo/$REPO.db"
echo "repo:  $ROOT/repo/$REPO.db"

# The server is a Cygwin process, so every path handed to libalpm is a POSIX
# path on its side, while the test reads the scriptlet and hook logs itself
# and needs the Windows one. Bash knows both, so it writes the POSIX form
# down rather than leaving the test to guess at a drive-letter mapping.
printf '%s\n' "$ROOT" > "$ROOT.posix"

echo "root: $ROOT"
