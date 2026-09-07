/* Client half of the mtree bridge.
 *
 * `struct archive` is a libarchive object, and a caller reads its entries with
 * libarchive functions -- archive_entry_pathname(), archive_entry_size() and
 * the rest. There is no accessor in libalpm's API for this bridge to
 * intercept, so it cannot be proxied, for exactly the reason alpm_list_t
 * cannot. The whole mtree therefore comes over in one call and is parsed
 * here, by this process's own libarchive, into a genuine archive whose
 * entries are genuine archive_entry objects. Whatever the caller asks of them
 * works, because nothing about them is a reconstruction.
 *
 * Linking libarchive here is not a compromise of the one rule this DLL keeps
 * -- that nothing from the MSYS2 tree is loaded into the calling process.
 * This is the mingw/ucrt64 libarchive, the same one a caller reading these
 * entries is already linking.
 *
 * A side effect: reading an mtree costs one round trip rather than one per
 * entry, because only the open goes to the server. next() is a local parse.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "arpc_client.h"

#include <alpm.h>
#include <archive.h>
#include <archive_entry.h>

#include <stdlib.h>

/* archive_read_open_memory does not copy what it is given, so the buffer has
 * to outlive the archive reading from it. It is kept here and freed when that
 * archive closes -- the caller never sees it and has nothing extra to free. */
typedef struct stream {
	struct stream *next;
	struct archive *archive;
	unsigned char *buf;
} stream;

static stream *g_streams;

static void remember(struct archive *a, unsigned char *buf)
{
	stream *s = (stream *)calloc(1, sizeof(*s));
	if (!s) {
		free(buf);
		return;
	}
	s->archive = a;
	s->buf = buf;
	s->next = g_streams;
	g_streams = s;
}

static void forget(struct archive *a)
{
	stream **pp = &g_streams;
	while (*pp) {
		if ((*pp)->archive == a) {
			stream *dead = *pp;
			*pp = dead->next;
			free(dead->buf);
			free(dead);
			return;
		}
		pp = &(*pp)->next;
	}
}

struct archive *alpm_pkg_mtree_open(alpm_pkg_t *pkg)
{
	arpc_call c;
	if (!arpc_begin(&c, "arpc.mtree"))
		return NULL;
	arpc_put_handle(&c, ARPC_ID(pkg));
	if (!arpc_invoke(&c)) {
		arpc_end(&c);
		return NULL;
	}

	size_t n = 0;
	unsigned char *buf = arpc_ret_bytes(&c, &n);
	arpc_end(&c);
	if (!buf)
		return NULL;            /* the package has no mtree */

	struct archive *a = archive_read_new();
	if (!a) {
		free(buf);
		return NULL;
	}
	archive_read_support_filter_all(a);
	archive_read_support_format_mtree(a);
	if (archive_read_open_memory(a, buf, n) != ARCHIVE_OK) {
		archive_read_free(a);
		free(buf);
		return NULL;
	}

	remember(a, buf);
	return a;
}

/* The same mapping libalpm's own implementation uses, rather than a more
 * generous one: 0 for an entry, 1 at the end, -1 for anything else. */
int alpm_pkg_mtree_next(const alpm_pkg_t *pkg, struct archive *archive,
			struct archive_entry **entry)
{
	(void)pkg;
	if (!archive || !entry)
		return -1;
	switch (archive_read_next_header(archive, entry)) {
	case ARCHIVE_OK:
		return 0;
	case ARCHIVE_EOF:
		return 1;
	default:
		return -1;
	}
}

int alpm_pkg_mtree_close(const alpm_pkg_t *pkg, struct archive *archive)
{
	(void)pkg;
	if (!archive)
		return -1;
	int rc = archive_read_free(archive);
	forget(archive);
	return rc == ARCHIVE_OK ? 0 : -1;
}
