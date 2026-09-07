/* Server half of the mtree bridge: a package's whole file listing, as bytes.
 *
 * `struct archive` is a libarchive object the caller reads with libarchive
 * functions, so it cannot be proxied -- the same argument that makes
 * alpm_list_t a real chain on the client rather than a cookie. The client
 * materialises its own archive and its own entries from what this sends, and
 * reads them with its own libarchive.
 *
 * What travels is not the file libalpm opened. It is what libarchive's mtree
 * *writer* produces from the entries libarchive's mtree *reader* produced --
 * out through the same format it came in by. That is deliberate: it keeps the
 * mapping between mtree keywords and archive_entry fields libarchive's
 * business rather than this bridge's, so nothing here has to enumerate which
 * fields matter and nothing is silently dropped when libarchive learns a new
 * one. The writer's default keyword set is exactly the entry-backed ones; the
 * digests in the original are not among them because archive_entry has
 * nowhere to put a digest, so no caller could have read them either way.
 */
#include "arpc_server.h"

#include <alpm.h>
#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* libarchive writes in blocks, and an mtree is as long as the package has
 * files, so the sink grows rather than guessing a cap. */
typedef struct {
	char *buf;
	size_t len, cap;
	int failed;
} sink;

static int sink_open(struct archive *a, void *ctx)
{
	(void)a; (void)ctx;
	return ARCHIVE_OK;
}

static la_ssize_t sink_write(struct archive *a, void *ctx, const void *b,
			     size_t n)
{
	sink *s = (sink *)ctx;
	if (s->len + n > s->cap) {
		size_t cap = s->cap ? s->cap : 8192;
		while (cap < s->len + n)
			cap *= 2;
		char *p = (char *)realloc(s->buf, cap);
		if (!p) {
			s->failed = 1;
			archive_set_error(a, ENOMEM, "out of memory");
			return -1;
		}
		s->buf = p;
		s->cap = cap;
	}
	memcpy(s->buf + s->len, b, n);
	s->len += n;
	return (la_ssize_t)n;
}

static int sink_close(struct archive *a, void *ctx)
{
	(void)a; (void)ctx;
	return ARCHIVE_OK;
}

int arpc_mtree_get(arpc_req *rq, arpc_res *rs)
{
	alpm_pkg_t *pkg = (alpm_pkg_t *)arpc_arg_handle(rq, 0, ARPC_H_PKG);
	if (arpc_req_bad(rq) || !pkg)
		return arpc_fail(rs, ARPC_E_INVALID_PARAMS,
				 "arpc.mtree: bad arguments");

	struct archive *in = alpm_pkg_mtree_open(pkg);
	if (!in) {
		/* A package with no mtree is not an error; it is a package
		 * with no mtree, and the caller gets NULL from open. */
		arpc_ret_null(rs);
		return 0;
	}

	sink s;
	memset(&s, 0, sizeof(s));

	struct archive *out = archive_write_new();
	if (!out) {
		alpm_pkg_mtree_close(pkg, in);
		return arpc_fail(rs, ARPC_E_INTERNAL, "out of memory");
	}
	archive_write_set_format_mtree(out);
	if (archive_write_open(out, &s, sink_open, sink_write, sink_close)
	    != ARCHIVE_OK) {
		archive_write_free(out);
		alpm_pkg_mtree_close(pkg, in);
		return arpc_fail(rs, ARPC_E_INTERNAL, "cannot write mtree");
	}

	struct archive_entry *e = NULL;
	int rc = 0;
	while (alpm_pkg_mtree_next(pkg, in, &e) == 0) {
		if (archive_write_header(out, e) != ARCHIVE_OK) {
			rc = -1;
			break;
		}
	}

	archive_write_close(out);
	archive_write_free(out);
	alpm_pkg_mtree_close(pkg, in);

	if (rc != 0 || s.failed) {
		free(s.buf);
		return arpc_fail(rs, ARPC_E_INTERNAL,
				 "arpc.mtree: could not re-serialise the mtree");
	}

	arpc_ret_bytes(rs, (const unsigned char *)s.buf, s.len);
	free(s.buf);
	return 0;
}
