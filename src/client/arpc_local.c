/* libalpm functions that run here and never reach the server.
 *
 * Not an optimisation. These operate on a struct this client materialised,
 * and their result has to point back into that same struct -- so going to the
 * server would return a pointer into libalpm's copy, which is not the one the
 * caller is holding. The generated *_free stubs are local for the mirror
 * reason: they free what this client made, and the server's copy is not ours.
 *
 * Each one is its own shape, so they are written out rather than generated,
 * and the overlay records that: coverage.json says "hand-written in
 * arpc_local.c" instead of reporting them as unsupported.
 */
#include <alpm.h>

#include <stdlib.h>
#include <string.h>

/* libalpm sorts a filelist by name and searches it with plain strcmp. This
 * is that search, not a reimplementation of it: a linear scan would agree
 * only for as long as the ordering did, and this way the two cannot drift. */
static int file_cmp(const void *a, const void *b)
{
	const alpm_file_t *f1 = a, *f2 = b;
	return strcmp(f1->name, f2->name);
}

alpm_file_t *alpm_filelist_contains(const alpm_filelist_t *filelist,
				    const char *path)
{
	alpm_file_t key;

	if (!filelist || filelist->count == 0)
		return NULL;

	key.name = (char *)path;
	return (alpm_file_t *)bsearch(&key, filelist->files, filelist->count,
				      sizeof(alpm_file_t), file_cmp);
}
