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

It was taken together with pacman 6.1.0's `alpm_list.h`, and it is compiled
against the `alpm_list.h` the server's libalpm installs. Those have to be the
same header, or the list this client builds is not the list libalpm walks.
`alpm_list.h.sha256` is the digest of the header it was vendored with, and
`src/client/CMakeLists.txt` checks the installed one against it at configure
time: a mismatch fails the build and says to re-vendor both.
