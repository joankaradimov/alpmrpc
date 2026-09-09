/* Where libalpm frees objects the handle table still points at.
 *
 * The table promises that a stale id misses instead of reaching a freed
 * object. It can keep that promise for what it dropped itself, but libalpm
 * also frees things on its own, and nothing in the header says where:
 * alpm_trans_release frees the packages the transaction owned,
 * alpm_trans_commit frees the installed version of everything it replaced or
 * removed, alpm_db_update throws away a db's whole package cache, and
 * alpm_unregister_all_syncdbs frees the dbs. Each of those calls is
 * bracketed by a pair of hooks here: before it, note what is about to die;
 * after it, drop those ids. What a client still holds then fails a lookup
 * instead of reaching freed memory.
 *
 * A post hook must not call libalpm. Every public function begins by
 * resetting the handle's errno, and the client has not asked for it yet --
 * so whatever a post hook needs to know, the pre hook has to have found
 * out. alpm_errno itself only reads, and is the one exception.
 */
#include "arpc_server.h"

#include <alpm.h>

#include <stdlib.h>
#include <string.h>

/* Pointers collected before a call, dropped after it. */
typedef struct {
	void **ptr;
	size_t n, cap;
} dying;

static dying *dying_new(void)
{
	return (dying *)calloc(1, sizeof(dying));
}

static void note(dying *d, void *p)
{
	if (!d || !p)
		return;
	if (d->n == d->cap) {
		size_t cap = d->cap ? d->cap * 2 : 16;
		void **np = (void **)realloc(d->ptr, cap * sizeof(*np));
		if (!np)
			return;         /* that one keeps its id; nothing worse */
		d->ptr = np;
		d->cap = cap;
	}
	d->ptr[d->n++] = p;
}

static void drop_all(dying *d, arpc_handle_tag tag)
{
	if (!d)
		return;
	for (size_t i = 0; i < d->n; i++)
		arpc_handle_drop_ptr(d->ptr[i], tag);
	free(d->ptr);
	free(d);
}

static void drop_none(dying *d)
{
	if (d)
		free(d->ptr);
	free(d);
}

static alpm_handle_t *handle_arg(arpc_req *rq)
{
	return (alpm_handle_t *)arpc_arg_handle(rq, 0, ARPC_H_HANDLE);
}

/* ---- alpm_trans_release ----
 *
 * Frees what the transaction owned: the packages loaded from files, which
 * alpm_add_pkg took over, and the copies alpm_remove_pkg made. Packages that
 * came from a db are the db's and survive. */
void *arpc_hook_alpm_trans_release_pre(arpc_req *rq)
{
	alpm_handle_t *h = handle_arg(rq);
	dying *d = dying_new();
	if (!h)
		return d;
	for (alpm_list_t *l = alpm_trans_get_add(h); l; l = l->next)
		if (alpm_pkg_get_origin((alpm_pkg_t *)l->data) == ALPM_PKG_FROM_FILE)
			note(d, l->data);
	for (alpm_list_t *l = alpm_trans_get_remove(h); l; l = l->next)
		note(d, l->data);
	return d;
}

void arpc_hook_alpm_trans_release_post(arpc_req *rq, void *state, long long rc)
{
	(void)rq;
	if (rc == 0)
		drop_all((dying *)state, ARPC_H_PKG);
	else
		drop_none((dying *)state); /* no transaction: nothing was freed */
}

/* ---- alpm_trans_commit ----
 *
 * Installing over a package frees the installed one, and removing a package
 * frees it: either way it is the local db's copy that goes, and the
 * transaction's own copies are not it. So the victims are looked up by name
 * in the local db before the call. Whether they actually died depends on
 * how far the commit got, which the post hook can only judge from the
 * errno: the checks that fail before anything is touched are known, and
 * anything else is taken to have touched something. Dropping an id that
 * survived costs a caller a lookup miss; keeping one that did not would let
 * the next field read reach freed memory. */
void *arpc_hook_alpm_trans_commit_pre(arpc_req *rq)
{
	alpm_handle_t *h = handle_arg(rq);
	dying *d = dying_new();
	alpm_db_t *local = h ? alpm_get_localdb(h) : NULL;
	if (!local)
		return d;
	/* alpm_pkg_find rather than alpm_db_get_pkg: a miss is the normal
	 * case for a fresh install, and alpm_db_get_pkg logs each one as an
	 * error at debug level, which a client's logcb would be shown. */
	alpm_list_t *cache = alpm_db_get_pkgcache(local);
	for (alpm_list_t *l = alpm_trans_get_add(h); l; l = l->next)
		note(d, alpm_pkg_find(cache,
				      alpm_pkg_get_name((alpm_pkg_t *)l->data)));
	for (alpm_list_t *l = alpm_trans_get_remove(h); l; l = l->next)
		note(d, alpm_pkg_find(cache,
				      alpm_pkg_get_name((alpm_pkg_t *)l->data)));
	return d;
}

/* The failures libalpm reports from its checks, before it has installed or
 * removed anything. */
static int touched_nothing(alpm_errno_t e)
{
	switch (e) {
	case ALPM_ERR_WRONG_ARGS:
	case ALPM_ERR_TRANS_NULL:
	case ALPM_ERR_TRANS_NOT_PREPARED:
	case ALPM_ERR_TRANS_NOT_LOCKED:
	case ALPM_ERR_TRANS_TYPE:
	case ALPM_ERR_RETRIEVE:
	case ALPM_ERR_PKG_INVALID:
	case ALPM_ERR_PKG_INVALID_CHECKSUM:
	case ALPM_ERR_PKG_INVALID_SIG:
	case ALPM_ERR_PKG_OPEN:
	case ALPM_ERR_FILE_CONFLICTS:
	case ALPM_ERR_DISK_SPACE:
		return 1;
	default:
		return 0;
	}
}

void arpc_hook_alpm_trans_commit_post(arpc_req *rq, void *state, long long rc)
{
	alpm_handle_t *h = handle_arg(rq);
	if (rc == 0 || !h || !touched_nothing(alpm_errno(h)))
		drop_all((dying *)state, ARPC_H_PKG);
	else
		drop_none((dying *)state);
}

/* ---- alpm_db_update ----
 *
 * A db whose file was fetched has its package cache freed, to be rebuilt on
 * next use. Which dbs those were is not reported -- only that at least one
 * was (0), none were (1), or something failed (-1) -- so unless none was,
 * every db passed in is taken to have been. The dbs are the request's own
 * argument, so there is nothing to find out before the call. */
void *arpc_hook_alpm_db_update_pre(arpc_req *rq)
{
	(void)rq;
	return NULL;
}

void arpc_hook_alpm_db_update_post(arpc_req *rq, void *state, long long rc)
{
	(void)state;
	if (rc == 1)
		return;                 /* all up to date: nothing was rebuilt */
	int dbs = arpc_arg_node(rq, 1);
	for (int e = arpc_node_first(rq, dbs); e >= 0; e = arpc_node_next(rq, e))
		arpc_handle_drop_under((uint64_t)arpc_node_i64(rq, e));
}

/* ---- alpm_unregister_all_syncdbs ----
 *
 * Frees every sync db, and with each one its package cache. The local db is
 * not among them and keeps its id. */
void *arpc_hook_alpm_unregister_all_syncdbs_pre(arpc_req *rq)
{
	alpm_handle_t *h = handle_arg(rq);
	dying *d = dying_new();
	for (alpm_list_t *l = h ? alpm_get_syncdbs(h) : NULL; l; l = l->next)
		note(d, l->data);
	return d;
}

void arpc_hook_alpm_unregister_all_syncdbs_post(arpc_req *rq, void *state,
						long long rc)
{
	(void)rq;
	if (rc == 0)
		drop_all((dying *)state, ARPC_H_DB);
	else
		drop_none((dying *)state);
}
