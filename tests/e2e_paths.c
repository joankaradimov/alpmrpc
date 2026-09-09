/* Win32 paths in, Win32 paths out.
 *
 * libalpm's paths are the server's, and the server is an MSYS2 process: to
 * it the fixture root is /f/work/..., and a native caller that wants to
 * open the files libalpm names has to know the Windows spelling. A DLL built
 * with ALPMRPC_WIN32_PATHS -- the default, and the only build this test is
 * registered for -- has the server convert, on the server, where
 * cygwin_conv_path is. This test hands libalpm nothing but Win32 paths and
 * checks what comes back the way a native program would: by opening it.
 *
 * Every direction a path travels is exercised: an argument, a returned
 * string, a returned list, a list argument, a record's field arriving in a
 * failed commit, a callback's argument, and an out-list a fetch fills.
 *
 *   argv[1] is the fixture root as a Windows path, and this test uses only
 *   that. <argv[1]>.posix holds the POSIX form, which is used once: to give
 *   libcurl a file:// URL it understands, since a URL is not a path.
 */
#include <alpm.h>
#include <alpm_list.h>

#ifndef ALPMRPC_WIN32_PATHS
#error "e2e_paths tests the DLL built with ALPMRPC_WIN32_PATHS, and this is not it"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int cond, const char *what, const char *detail)
{
	printf("%-6s %-46s %s\n", cond ? "ok" : "FAIL", what,
	       detail ? detail : "");
	if (!cond)
		failures++;
}

/* A drive letter, a colon and a separator: what no POSIX path starts with. */
static int is_win32(const char *p)
{
	return p && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))
	       && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}

/* The same place, whatever the separators and case, ignoring a trailing
 * separator: libalpm keeps its directories with one. */
static int same_place(const char *a, const char *b)
{
	size_t na = strlen(a), nb = strlen(b);
	while (na && (a[na - 1] == '\\' || a[na - 1] == '/'))
		na--;
	while (nb && (b[nb - 1] == '\\' || b[nb - 1] == '/'))
		nb--;
	if (na != nb)
		return 0;
	for (size_t i = 0; i < na; i++) {
		char x = a[i], y = b[i];
		if (x == '/') x = '\\';
		if (y == '/') y = '\\';
		if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
		if (x != y)
			return 0;
	}
	return 1;
}

static int can_open(const char *path)
{
	FILE *f = path ? fopen(path, "rb") : NULL;
	if (f)
		fclose(f);
	return f != NULL;
}

static int fetch_calls;
static char fetch_local[512];

static int on_fetch(void *ctx, const char *url, const char *localpath,
		    int force)
{
	(void)ctx; (void)url; (void)force;
	fetch_calls++;
	snprintf(fetch_local, sizeof(fetch_local), "%s",
		 localpath ? localpath : "");
	return -1;
}

static void on_question(void *ctx, alpm_question_t *q)
{
	(void)ctx;
	q->any.answer = 1;
}

static int run(alpm_handle_t *h, alpm_pkg_t *p, alpm_list_t **data)
{
	*data = NULL;
	if (alpm_trans_init(h, 0) != 0 || alpm_add_pkg(h, p) != 0)
		return -1;
	int rc = alpm_trans_prepare(h, data);
	if (rc == 0) {
		alpm_list_free(*data);
		*data = NULL;
		rc = alpm_trans_commit(h, data);
	}
	return rc;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: e2e_paths <fixture root, as a Windows path>\n");
		return 2;
	}
	const char *win_root = argv[1];

	char sidecar[1024], posix_root[1024];
	snprintf(sidecar, sizeof(sidecar), "%s.posix", win_root);
	FILE *f = fopen(sidecar, "rb");
	if (!f) {
		printf("cannot read %s -- run tests/scratch/mkroot.sh\n", sidecar);
		return 2;
	}
	size_t got = fread(posix_root, 1, sizeof(posix_root) - 1, f);
	fclose(f);
	posix_root[got] = '\0';
	posix_root[strcspn(posix_root, "\r\n")] = '\0';

	char buf[512];
	printf("-- giving libalpm nothing but Win32 paths --\n");

	char dbpath[1024], cachedir[1024], hookdir[1024], base_pkg[1024];
	char squatter_pkg[1024];
	snprintf(dbpath, sizeof(dbpath), "%s\\var\\lib\\pacman", win_root);
	snprintf(cachedir, sizeof(cachedir), "%s\\var\\cache\\pacman\\pkg",
		 win_root);
	snprintf(hookdir, sizeof(hookdir), "%s\\etc\\pacman.d\\hooks", win_root);
	snprintf(base_pkg, sizeof(base_pkg),
		 "%s\\var\\cache\\pacman\\pkg\\alpmrpc-base-1.0-1-x86_64.pkg.tar.zst",
		 win_root);
	snprintf(squatter_pkg, sizeof(squatter_pkg),
		 "%s\\var\\cache\\pacman\\pkg\\"
		 "alpmrpc-squatter-1.0-1-x86_64.pkg.tar.zst", win_root);

	alpm_errno_t err = 0;
	alpm_handle_t *h = alpm_initialize(win_root, dbpath, &err);
	snprintf(buf, sizeof(buf), "err=%d", (int)err);
	check(h != NULL, "alpm_initialize() with a Win32 root and dbpath", buf);
	if (!h)
		return 1;

	printf("\n-- strings come back as Win32 --\n");
	const char *root = alpm_option_get_root(h);
	check(is_win32(root), "alpm_option_get_root() is a Win32 path", root);
	check(root && same_place(root, win_root),
	      "and it is the root that was given", win_root);
	const char *dbp = alpm_option_get_dbpath(h);
	check(is_win32(dbp) && same_place(dbp, dbpath),
	      "alpm_option_get_dbpath() likewise", dbp);
	const char *lock = alpm_option_get_lockfile(h);
	check(is_win32(lock) && strstr(lock, "db.lck") != NULL,
	      "alpm_option_get_lockfile(), which libalpm made itself", lock);

	printf("\n-- a list goes in and comes back --\n");
	check(alpm_option_add_cachedir(h, cachedir) == 0,
	      "alpm_option_add_cachedir() with a Win32 path", NULL);
	alpm_list_t *dirs = alpm_option_get_cachedirs(h);
	const char *d0 = dirs ? (const char *)dirs->data : NULL;
	check(is_win32(d0) && same_place(d0, cachedir),
	      "alpm_option_get_cachedirs() returns it as Win32", d0);
	alpm_list_t *hooks = NULL;
	alpm_list_append(&hooks, hookdir);
	check(alpm_option_set_hookdirs(h, hooks) == 0,
	      "alpm_option_set_hookdirs() with a Win32 list", NULL);
	alpm_list_free(hooks);
	alpm_list_t *hooks2 = alpm_option_get_hookdirs(h);
	const char *h0 = hooks2 ? (const char *)hooks2->data : NULL;
	check(is_win32(h0) && same_place(h0, hookdir),
	      "and comes back the same way", h0);

	printf("\n-- a file argument reaches libalpm --\n");
	char *sum = alpm_compute_md5sum(base_pkg);
	check(sum && strlen(sum) == 32,
	      "alpm_compute_md5sum() on a Win32 path", sum);
	free(sum);

	alpm_option_set_questioncb(h, on_question, NULL);
	alpm_pkg_t *base = NULL;
	check(alpm_pkg_load(h, base_pkg, 1, 0, &base) == 0 && base,
	      "alpm_pkg_load() on a Win32 path", NULL);
	alpm_list_t *data = NULL;
	int rc = base ? run(h, base, &data) : -1;
	alpm_list_free(data);
	alpm_trans_release(h);
	check(rc == 0, "and it installs",
	      rc == 0 ? NULL : alpm_strerror(alpm_errno(h)));
	snprintf(buf, sizeof(buf), "%s\\alpmrpc-scriptlet.log", win_root);
	check(can_open(buf), "into the root that was named: the scriptlet log "
	      "is there", "opened natively");

	printf("\n-- a record's field arrives as Win32 --\n");
	/* alpmrpc-squatter ships a file alpmrpc-base owns; the commit refuses
	 * with an alpm_fileconflict_t whose file is an absolute path. */
	alpm_pkg_t *sq = NULL;
	alpm_pkg_load(h, squatter_pkg, 1, 0, &sq);
	rc = sq ? run(h, sq, &data) : -1;
	check(rc != 0 && alpm_errno(h) == ALPM_ERR_FILE_CONFLICTS,
	      "the commit fails with ALPM_ERR_FILE_CONFLICTS", NULL);
	if (data) {
		alpm_fileconflict_t *fc = (alpm_fileconflict_t *)data->data;
		check(is_win32(fc->file), "the conflict's file is a Win32 path",
		      fc->file);
		check(can_open(fc->file),
		      "and this process can open it by that name", NULL);
		for (alpm_list_t *l = data; l; l = l->next)
			alpm_fileconflict_free((alpm_fileconflict_t *)l->data);
		alpm_list_free(data);
	}
	alpm_trans_release(h);

	printf("\n-- a callback's argument arrives as Win32 --\n");
	/* A URL is not a path, so the file:// repo is named in the form
	 * libcurl on the server understands. */
	char server[1024];
	snprintf(server, sizeof(server), "file://%s/repo", posix_root);
	alpm_db_t *sync = alpm_register_syncdb(h, "alpmrpc", 0);
	check(sync && alpm_db_add_server(sync, server) == 0,
	      "alpm_register_syncdb() + alpm_db_add_server()", server);
	alpm_option_set_fetchcb(h, on_fetch, NULL);
	alpm_list_t *dbs = NULL;
	alpm_list_append(&dbs, sync);
	alpm_db_update(h, dbs, 1);      /* refused by the callback: fine */
	check(fetch_calls > 0, "the fetch callback fired", NULL);
	check(is_win32(fetch_local),
	      "with a Win32 path to put the download in", fetch_local);
	alpm_option_set_fetchcb(h, NULL, NULL);

	printf("\n-- an out-list comes back as Win32 --\n");
	check(alpm_db_update(h, dbs, 1) == 0,
	      "alpm_db_update() by libalpm itself", NULL);
	alpm_list_free(dbs);
	char url[1200];
	snprintf(url, sizeof(url),
		 "%s/alpmrpc-spare-1.0-1-x86_64.pkg.tar.zst", server);
	alpm_list_t *urls = NULL, *fetched = NULL;
	alpm_list_append(&urls, url);
	rc = alpm_fetch_pkgurl(h, urls, &fetched);
	alpm_list_free(urls);
	const char *got_path = fetched ? (const char *)fetched->data : NULL;
	check(rc == 0 && is_win32(got_path),
	      "alpm_fetch_pkgurl() names the file in Win32 form", got_path);
	check(can_open(got_path), "and this process can open it", NULL);
	alpm_list_free_inner(fetched, free);
	alpm_list_free(fetched);

	alpm_release(h);
	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
