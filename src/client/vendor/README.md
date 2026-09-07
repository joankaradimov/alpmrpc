# Vendored from pacman

`alpm_list.c` is taken verbatim from pacman v6.1.0
(`lib/libalpm/alpm_list.c`), GPL-2.0-or-later — the same terms as this
project, which is not a coincidence: linking it is why those are the terms.
See [../../../LICENSE](../../../LICENSE).

It is here because `alpm_list_t` is a *transparent* type: callers walk
`->next` and call `alpm_list_count`, `alpm_list_free`, `alpm_list_msort` and
friends directly. Those functions have to exist in the client with exactly
libalpm's semantics, and the file is self-contained by design -- its own
header says so, and it includes nothing but `stdlib.h`, `string.h` and
`alpm_list.h`.

The `alpm_list.h` in this pacman tree is byte-identical to the one installed
by the libalpm the server links against, which is what makes this safe. If
that ever stops being true, the build should fail rather than drift: see the
header check in `src/client/CMakeLists.txt`.
