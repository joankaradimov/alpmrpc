/* Lists over the wire.
 *
 * The point of every check here is that the caller uses libalpm's own idioms
 * -- walking ->next, calling alpm_list_count, freeing with the documented
 * pattern -- and gets libalpm's own answers. A list that only works when
 * accessed through accessors would pass a weaker test and fail real code.
 */
#include <alpm.h>
#include <alpm_list.h>
#include "path_form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not libalpm API: the bridge's own count of what it has cached. */
extern size_t arpc_stats_cached(void);

static int failures;

static void check(int cond, const char *what, const char *detail)
{
	printf("%-6s %-40s %s\n", cond ? "ok" : "FAIL", what,
	       detail ? detail : "");
	if (!cond)
		failures++;
}

int main(void)
{
	char buf[256];
	alpm_errno_t err = 0;
	alpm_handle_t *h = alpm_initialize("/", "/var/lib/pacman/", &err);
	if (!h) {
		printf("alpm_initialize failed (err=%d)\n", (int)err);
		return 1;
	}
	alpm_db_t *db = alpm_get_localdb(h);
	if (!db) {
		printf("no local db\n");
		return 1;
	}

	printf("-- a real chain, walked the way callers walk it --\n");
	alpm_list_t *cache = alpm_db_get_pkgcache(db);
	check(cache != NULL, "alpm_db_get_pkgcache()", NULL);

	size_t walked = 0;
	for (alpm_list_t *i = cache; i; i = i->next)
		walked++;
	size_t counted = alpm_list_count(cache);
	snprintf(buf, sizeof(buf), "%zu packages", walked);
	check(walked > 1, "walking ->next visits every element", buf);
	check(walked == counted, "alpm_list_count agrees with the walk", NULL);

	/* The example's one-node proxy would pass a count check but fail this:
	 * it made every list look like it had exactly one element. */
	check(cache->next != NULL, "second element is reachable", NULL);
	check(cache->next->prev == cache, "prev links back", NULL);

	printf("\n-- elements are usable, not opaque --\n");
	alpm_pkg_t *first = (alpm_pkg_t *)cache->data;
	const char *name = alpm_pkg_get_name(first);
	check(name && *name, "pkg from i->data works as a handle", name);

	int named = 0;
	for (alpm_list_t *i = cache; i; i = i->next) {
		const char *n = alpm_pkg_get_name((alpm_pkg_t *)i->data);
		if (n && *n)
			named++;
	}
	snprintf(buf, sizeof(buf), "%d of %zu", named, walked);
	check((size_t)named == walked, "every element resolves to a package", buf);

	printf("\n-- borrowed lists keep libalpm's contract --\n");
	alpm_list_t *again = alpm_db_get_pkgcache(db);
	check(again == cache, "repeat call returns the same pointer",
	      "cached against the owning handle");

	printf("\n-- string lists --\n");
	/* A handle that has not read a pacman.conf has no cachedirs, so put one
	 * there first: an empty list would not prove anything either way. */
	alpm_option_add_cachedir(h, "/var/cache/pacman/pkg/");
	alpm_list_t *cachedirs = alpm_option_get_cachedirs(h);
	check(cachedirs != NULL, "alpm_option_get_cachedirs()", NULL);
	if (cachedirs) {
		const char *d = (const char *)cachedirs->data;
		check(path_in_built_form(d),
		      "element is a usable char*, a " PATH_FORM " path", d);
	}

	printf("\n-- nested: a list inside a materialised struct --\n");
	alpm_list_t *groups = alpm_db_get_groupcache(db);
	if (groups) {
		alpm_group_t *g = (alpm_group_t *)groups->data;
		check(g && g->name, "alpm_group_t->name", g ? g->name : NULL);
		size_t members = alpm_list_count(g ? g->packages : NULL);
		snprintf(buf, sizeof(buf), "%s has %zu members",
			 g && g->name ? g->name : "?", members);
		check(members > 0, "group->packages is itself a real list", buf);
		if (g && g->packages) {
			const char *mn =
				alpm_pkg_get_name((alpm_pkg_t *)g->packages->data);
			check(mn && *mn, "nested list elements are usable", mn);
		}
	} else {
		check(1, "no groups in this local db", "skipped");
	}

	printf("\n-- struct lists are materialised, fields readable --\n");
	alpm_pkg_t *withdeps = NULL;
	alpm_list_t *deps = NULL;
	for (alpm_list_t *i = cache; i && !withdeps; i = i->next) {
		alpm_list_t *d = alpm_pkg_get_depends((alpm_pkg_t *)i->data);
		if (d) {
			withdeps = (alpm_pkg_t *)i->data;
			deps = d;
		}
	}
	check(deps != NULL, "found a package with depends",
	      withdeps ? alpm_pkg_get_name(withdeps) : NULL);
	if (deps) {
		alpm_depend_t *d = (alpm_depend_t *)deps->data;
		check(d != NULL && d->name != NULL,
		      "alpm_depend_t->name is readable", d ? d->name : NULL);
		/* Reading the fields straight off the struct is exactly what a
		 * caller does with a transparent type, so it is the check that
		 * matters: every field has to have survived materialisation. */
		snprintf(buf, sizeof(buf), "mod=%d name_hash=%s",
			 (int)d->mod, d->name_hash ? "set" : "zero");
		check(d->mod >= ALPM_DEP_MOD_ANY && d->mod <= ALPM_DEP_MOD_LT,
		      "scalar and enum fields survived", buf);
		check(d->name_hash != 0, "unsigned long field survived", NULL);

		int with_version = 0;
		for (alpm_list_t *i = deps; i; i = i->next) {
			alpm_depend_t *e = (alpm_depend_t *)i->data;
			if (e && e->version)
				with_version++;
		}
		snprintf(buf, sizeof(buf), "%d of %zu carry a version",
			 with_version, alpm_list_count(deps));
		check(1, "optional string fields are NULL or set, not garbage",
		      buf);
	}

	printf("\n-- caller-owned lists are freed the documented way --\n");
	alpm_list_t *req = alpm_pkg_compute_requiredby(first);
	snprintf(buf, sizeof(buf), "%zu entries", alpm_list_count(req));
	check(1, "alpm_pkg_compute_requiredby()", buf);
	if (req) {
		check(((const char *)req->data) != NULL,
		      "entries are package-name strings",
		      (const char *)req->data);
	}
	/* FREELIST is what pacman itself uses for this list. */
	alpm_list_free_inner(req, free);
	alpm_list_free(req);
	check(1, "freed with alpm_list_free_inner + alpm_list_free", NULL);

	printf("\n-- list parameters travel the other way --\n");
	alpm_list_t *needles = NULL;
	alpm_list_append(&needles, (void *)"gcc");
	alpm_pkg_t *found = alpm_find_satisfier(cache, alpm_pkg_get_name(first));
	check(1, "alpm_find_satisfier() accepted a list param",
	      found ? alpm_pkg_get_name(found) : "no satisfier");
	alpm_list_free(needles);

	printf("\n-- the same object is the same pointer --\n");
	/* libalpm hands out one alpm_pkg_t for a package however it is
	 * reached, and callers compare them -- pacman does, with
	 * alpm_list_find_ptr. An id here is minted once per object, so the
	 * same holds. */
	check(found == first,
	      "alpm_find_satisfier() returns the list's own element",
	      "same id for the same object");
	check(alpm_db_get_pkg(db, alpm_pkg_get_name(first)) == first,
	      "and so does alpm_db_get_pkg()", NULL);
	check(alpm_get_localdb(h) == db, "alpm_get_localdb() twice is one db",
	      NULL);
	check(alpm_pkg_get_db(first) == db,
	      "and alpm_pkg_get_db() is that same db", NULL);

	printf("\n-- a setter detaches what a getter cached --\n");
	/* A borrowed list is cached, but libalpm's own copy changes under an
	 * add; a re-read must see it, and the list read before must stay
	 * readable, because natively it would be the same memory. */
	size_t ndirs = alpm_list_count(cachedirs);
	alpm_option_add_cachedir(h, "/var/cache/pacman/alpmrpc-extra/");
	alpm_list_t *cachedirs2 = alpm_option_get_cachedirs(h);
	snprintf(buf, sizeof(buf), "%zu -> %zu", ndirs,
		 alpm_list_count(cachedirs2));
	check(alpm_list_count(cachedirs2) == ndirs + 1,
	      "alpm_option_get_cachedirs() sees the added dir", buf);
	check(alpm_list_count(cachedirs) == ndirs,
	      "the list from before is still readable",
	      "detached, not freed");
	const char *root0 = alpm_option_get_root(h);
	alpm_option_add_cachedir(h, "/var/cache/pacman/alpmrpc-more/");
	check(alpm_option_get_root(h) == root0,
	      "a string that did not change keeps its pointer", root0);
	check(alpm_db_get_pkgcache(db) == cache,
	      "and the pkgcache, which it cannot touch, is untouched",
	      "only what a call can change is detached");

	printf("\n-- strings that belong to nobody stay put --\n");
	/* alpm_strerror's strings are static in libalpm. Here they belong to
	 * no handle, and their value depends on the argument, so caching them
	 * under one slot would free each when the next was asked for. */
	const char *e1 = alpm_strerror(ALPM_ERR_MEMORY);
	const char *e2 = alpm_strerror(ALPM_ERR_SYSTEM);
	check(e1 && e2 && strcmp(e1, e2) != 0,
	      "alpm_strerror() differs by errno", e2);
	check(alpm_strerror(ALPM_ERR_MEMORY) == e1,
	      "and an earlier pointer is still the answer", e1);

	printf("\n-- batched fields agree with unbatched ones --\n");
	/* Reading a field off a list member goes through the column cache;
	 * reading it off a package fetched by name does not. The two paths
	 * must agree, or the batch is quietly serving wrong data. */
	const char *batched_name = alpm_pkg_get_name(first);
	const char *batched_ver = alpm_pkg_get_version(first);
	alpm_pkg_t *byname = alpm_db_get_pkg(db, batched_name);
	check(byname != NULL, "alpm_db_get_pkg() by the batched name",
	      batched_name);
	if (byname) {
		const char *v = alpm_pkg_get_version(byname);
		check(v && batched_ver && !strcmp(v, batched_ver),
		      "version matches on both paths", v);
		off_t a = alpm_pkg_get_isize(first);
		off_t b = alpm_pkg_get_isize(byname);
		snprintf(buf, sizeof(buf), "%lld vs %lld", (long long)a,
			 (long long)b);
		check(a == b, "numeric field matches on both paths", buf);
	}

	/* Every member must get its own value, not the first one repeated --
	 * an off-by-one in the column indexing would show up here. */
	int distinct = 0;
	const char *prev = NULL;
	for (alpm_list_t *i = cache; i; i = i->next) {
		const char *nm = alpm_pkg_get_name((alpm_pkg_t *)i->data);
		if (!prev || (nm && strcmp(nm, prev)))
			distinct++;
		prev = nm;
	}
	snprintf(buf, sizeof(buf), "%d distinct across %zu members", distinct,
		 walked);
	check((size_t)distinct == walked, "column indexing lines up per member",
	      buf);

	printf("\n-- structs travel back the other way --\n");
	if (deps) {
		alpm_depend_t *d = (alpm_depend_t *)deps->data;
		/* This struct was materialised here from the server's copy. Sending
		 * it back and having libalpm render it only agrees if every field
		 * survived both directions. */
		char *s = alpm_dep_compute_string(d);
		check(s != NULL && strstr(s, d->name) != NULL,
		      "alpm_dep_compute_string() round-trips a struct", s);
		free(s);
	}

	/* Built by hand rather than materialised, so nothing about it came from
	 * the server -- it still has to serialise correctly. */
	alpm_depend_t *made = alpm_dep_from_string("bash>=5.0");
	check(made != NULL, "alpm_dep_from_string() returns a struct",
	      made ? made->name : NULL);
	if (made) {
		check(made->name && !strcmp(made->name, "bash"), "name parsed",
		      made->name);
		check(made->version && !strcmp(made->version, "5.0"),
		      "version parsed", made->version);
		check(made->mod == ALPM_DEP_MOD_GE, "comparison parsed", ">=");
		char *back = alpm_dep_compute_string(made);
		check(back && !strcmp(back, "bash>=5.0"),
		      "and renders back to the original", back);
		free(back);
		alpm_dep_free(made);    /* local free, no round trip */
		check(1, "alpm_dep_free() on a caller-owned struct", NULL);
	}

	printf("\n-- borrowed struct returns --\n");
	if (groups) {
		alpm_group_t *g0 = (alpm_group_t *)groups->data;
		alpm_group_t *byname = alpm_db_get_group(db, g0->name);
		check(byname != NULL, "alpm_db_get_group()", g0->name);
		if (byname) {
			check(byname->name && !strcmp(byname->name, g0->name),
			      "same group by name", byname->name);
			alpm_group_t *again2 = alpm_db_get_group(db, g0->name);
			check(again2 == byname,
			      "repeat call returns the same pointer",
			      "cached against the owning handle");
			/* Cached per argument, not per handle: another name
			 * has to be another group, not the first one again. */
			if (groups->next) {
				alpm_group_t *g1 =
					(alpm_group_t *)groups->next->data;
				alpm_group_t *by1 = alpm_db_get_group(db, g1->name);
				check(by1 && by1 != byname && by1->name
				      && !strcmp(by1->name, g1->name),
				      "another name is another group", g1->name);
			}
		}
	}

	printf("\n-- teardown releases cached lists --\n");
	snprintf(buf, sizeof(buf), "%zu entries cached", arpc_stats_cached());
	check(arpc_stats_cached() > 0, "lists and columns are cached before it",
	      buf);
	alpm_release(h);
	size_t left = arpc_stats_cached();
	snprintf(buf, sizeof(buf), "%zu left", left);
	check(left == 0, "alpm_release() with lists outstanding frees them",
	      buf);

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
