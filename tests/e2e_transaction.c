/* A real transaction, driven through the bridge against a throwaway root.
 *
 * This is the test the write path was arranged around, and it is the first
 * one that makes libalpm do the things the design is a bet on:
 *
 *   - ask a question and refuse to continue until it is answered, which is
 *     why callbacks are delivered nested inside the call that triggered them
 *     rather than queued;
 *   - fork() and exec() a shell for a scriptlet and again for a hook, which
 *     is why the server is single-threaded -- that is the only shape Cygwin
 *     supports fork in.
 *
 * The fixture (tests/scratch/mkroot.sh) builds two packages that conflict by
 * name, each with a scriptlet, and a root with a post-transaction hook. So
 * installing the second while the first is installed is a question, and
 * answering it yes is a remove and an install, three scriptlet runs and a
 * hook run.
 *
 * Every claim here is checked twice where it can be: once through what the
 * bridge reported to the callbacks, and once against what the shell left
 * behind in the root, read from this process with no libalpm involved. A
 * scriptlet that libalpm merely *said* it ran would pass the first check and
 * fail the second.
 *
 *   argv[1] is the fixture root as a Windows path, for reading those files.
 *   <argv[1]>.posix holds the same root as the server sees it, which is what
 *   libalpm is given.
 */
#include <alpm.h>
#include <alpm_list.h>

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

/* ---- what the callbacks saw ---- */

static int questions;
static alpm_question_type_t last_question;
static char conflict_pair[256];

static int events;
static int op_install, op_remove;
static int scriptlet_lines;
static char first_scriptlet[256];
static int hook_starts, hook_runs;
static char hook_desc[256];
static int inside_commit;
static int question_inside_commit;

static int db_retrieve_start, pkg_retrieve_start;
static long long pkg_retrieve_num, pkg_retrieve_size;

static int progress_calls;
static char progress_pkg[128];

static int dl_init, dl_completed, dl_calls;
static char dl_file[256];
static int fetch_calls;
static char fetch_url[512], fetch_localpath[512];

static void on_event(void *ctx, alpm_event_t *e)
{
	(void)ctx;
	events++;
	switch (e->type) {
	case ALPM_EVENT_PACKAGE_OPERATION_START:
		if (e->package_operation.operation == ALPM_PACKAGE_INSTALL)
			op_install++;
		else if (e->package_operation.operation == ALPM_PACKAGE_REMOVE)
			op_remove++;
		break;
	case ALPM_EVENT_SCRIPTLET_INFO:
		if (!scriptlet_lines++ && e->scriptlet_info.line)
			snprintf(first_scriptlet, sizeof(first_scriptlet),
				 "%s", e->scriptlet_info.line);
		break;
	case ALPM_EVENT_HOOK_START:
		hook_starts++;
		break;
	case ALPM_EVENT_HOOK_RUN_START:
		hook_runs++;
		if (e->hook_run.desc)
			snprintf(hook_desc, sizeof(hook_desc), "%s",
				 e->hook_run.desc);
		break;
	case ALPM_EVENT_DB_RETRIEVE_START:
		db_retrieve_start++;
		break;
	case ALPM_EVENT_PKG_RETRIEVE_START:
		/* The one event variant nothing had exercised, because it
		 * only fires when libalpm actually has to fetch a package. */
		pkg_retrieve_start++;
		pkg_retrieve_num = (long long)e->pkg_retrieve.num;
		pkg_retrieve_size = (long long)e->pkg_retrieve.total_size;
		break;
	default:
		break;
	}
}

static void on_progress(void *ctx, alpm_progress_t what, const char *pkg,
			int percent, size_t howmany, size_t current)
{
	(void)ctx; (void)what; (void)percent; (void)howmany; (void)current;
	progress_calls++;
	if (pkg && !progress_pkg[0])
		snprintf(progress_pkg, sizeof(progress_pkg), "%s", pkg);
}

static void on_dl(void *ctx, const char *filename,
		  alpm_download_event_type_t event, void *data)
{
	(void)ctx;
	dl_calls++;
	if (filename && !dl_file[0])
		snprintf(dl_file, sizeof(dl_file), "%s", filename);
	if (event == ALPM_DOWNLOAD_INIT && data) {
		dl_init++;
	} else if (event == ALPM_DOWNLOAD_COMPLETED && data) {
		alpm_download_event_completed_t *c = data;
		if (c->result >= 0)
			dl_completed++;
	}
}

/* Registering this takes downloading away from libalpm entirely, so it is
 * the caller that would have to fetch. Refusing is enough to prove the
 * arguments arrived and the answer was believed. */
static int on_fetch(void *ctx, const char *url, const char *localpath,
		    int force)
{
	(void)ctx;
	(void)force;
	fetch_calls++;
	snprintf(fetch_url, sizeof(fetch_url), "%s", url ? url : "");
	snprintf(fetch_localpath, sizeof(fetch_localpath), "%s",
		 localpath ? localpath : "");
	return -1;
}

/* A conflict is answered yes: remove the installed package so the new one
 * can go in. The answer is written through the variant's own field, which is
 * the aliasing the server then reads back through q->any.answer. */
static void on_question(void *ctx, alpm_question_t *q)
{
	(void)ctx;
	questions++;
	last_question = q->type;
	if (inside_commit)
		question_inside_commit = 1;

	if (q->type == ALPM_QUESTION_CONFLICT_PKG) {
		snprintf(conflict_pair, sizeof(conflict_pair), "%s vs %s",
			 alpm_pkg_get_name(q->conflict.conflict->package1),
			 alpm_pkg_get_name(q->conflict.conflict->package2));
		q->conflict.remove = 1;
	} else {
		q->any.answer = 1;
	}
}

/* ---- driving a transaction ---- */

static const char *trans_err(alpm_handle_t *h)
{
	return alpm_strerror(alpm_errno(h));
}

static int add_and_commit(alpm_handle_t *h, alpm_pkg_t *p, const char **stage)
{
	alpm_list_t *data = NULL;

	*stage = "alpm_trans_init";
	if (alpm_trans_init(h, 0) != 0)
		return -1;

	*stage = "alpm_add_pkg";
	if (alpm_add_pkg(h, p) != 0) {
		alpm_trans_release(h);
		return -1;
	}

	*stage = "alpm_trans_prepare";
	if (alpm_trans_prepare(h, &data) != 0) {
		alpm_list_free(data);
		alpm_trans_release(h);
		return -1;
	}
	alpm_list_free(data);
	data = NULL;

	*stage = "alpm_trans_commit";
	inside_commit = 1;
	int rc = alpm_trans_commit(h, &data);
	inside_commit = 0;
	alpm_list_free(data);
	alpm_trans_release(h);
	if (rc != 0)
		return -1;

	*stage = NULL;
	return 0;
}

/* A package file, the pacman -U path: load it, then the same transaction. */
static int install(alpm_handle_t *h, const char *pkgfile, const char **stage)
{
	alpm_pkg_t *p = NULL;
	*stage = "alpm_pkg_load";
	if (alpm_pkg_load(h, pkgfile, 1, 0, &p) != 0 || !p)
		return -1;
	return add_and_commit(h, p, stage);
}

/* ---- the root, from this process rather than through libalpm ---- */

static int slurp(const char *dir, const char *name, char *buf, size_t n)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		buf[0] = '\0';
		return 0;
	}
	size_t got = fread(buf, 1, n - 1, f);
	buf[got] = '\0';
	fclose(f);
	return 1;
}

static int count_lines(const char *s)
{
	int n = 0;
	for (; *s; s++)
		if (*s == '\n')
			n++;
	return n;
}

static int installed_count(alpm_handle_t *h)
{
	alpm_db_t *local = alpm_get_localdb(h);
	return local ? (int)alpm_list_count(alpm_db_get_pkgcache(local)) : -1;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: e2e_transaction <fixture root>\n");
		return 2;
	}
	const char *win_root = argv[1];

	/* The server is a Cygwin process; the fixture wrote down the root as
	 * it sees it, because that is what libalpm has to be given. */
	char posix_root[1024];
	char sidecar[1024];
	snprintf(sidecar, sizeof(sidecar), "%s.posix", win_root);
	FILE *f = fopen(sidecar, "rb");
	if (!f) {
		printf("cannot read %s -- run tests/scratch/mkroot.sh\n",
		       sidecar);
		return 2;
	}
	size_t got = fread(posix_root, 1, sizeof(posix_root) - 1, f);
	fclose(f);
	posix_root[got] = '\0';
	posix_root[strcspn(posix_root, "\r\n")] = '\0';

	char dbpath[1024], cachedir[1024], hookdir[1024];
	char base_pkg[1024], rival_pkg[1024];
	snprintf(dbpath, sizeof(dbpath), "%s/var/lib/pacman/", posix_root);
	snprintf(cachedir, sizeof(cachedir), "%s/var/cache/pacman/pkg/",
		 posix_root);
	snprintf(hookdir, sizeof(hookdir), "%s/etc/pacman.d/hooks/",
		 posix_root);
	snprintf(base_pkg, sizeof(base_pkg),
		 "%s/var/cache/pacman/pkg/alpmrpc-base-1.0-1-x86_64.pkg.tar.zst",
		 posix_root);
	snprintf(rival_pkg, sizeof(rival_pkg),
		 "%s/var/cache/pacman/pkg/alpmrpc-rival-1.0-1-x86_64.pkg.tar.zst",
		 posix_root);

	printf("root %s\n\n", posix_root);

	alpm_errno_t err = 0;
	alpm_handle_t *h = alpm_initialize(posix_root, dbpath, &err);
	if (!h) {
		printf("alpm_initialize(%s) failed (err=%d)\n", posix_root,
		       (int)err);
		return 1;
	}

	alpm_option_add_cachedir(h, cachedir);
	alpm_option_add_hookdir(h, hookdir);
	alpm_option_add_architecture(h, "x86_64");

	check(alpm_option_set_eventcb(h, on_event, NULL) == 0,
	      "alpm_option_set_eventcb()", NULL);
	check(alpm_option_set_questioncb(h, on_question, NULL) == 0,
	      "alpm_option_set_questioncb()", NULL);
	check(alpm_option_set_progresscb(h, on_progress, NULL) == 0,
	      "alpm_option_set_progresscb()", NULL);
	check(installed_count(h) == 0, "the scratch root starts empty", NULL);

	/* ---- install one package ---- */

	printf("\n-- installing alpmrpc-base --\n");
	const char *stage = NULL;
	int rc = install(h, base_pkg, &stage);
	check(rc == 0, "the transaction committed",
	      rc == 0 ? NULL : trans_err(h));
	if (rc != 0)
		printf("       failed at %s\n", stage);
	check(installed_count(h) == 1, "one package is now installed", NULL);
	check(op_install == 1, "ALPM_EVENT_PACKAGE_OPERATION_START (install)",
	      NULL);
	/* progresscb has been carried since callbacks went in, but nothing
	 * had ever made libalpm report progress at anything. */
	check(progress_calls > 0, "the progress callback fired", progress_pkg);

	/* ---- the fork, checked against what the shell left behind ---- */

	printf("\n-- libalpm forked a shell --\n");
	char log[4096];
	slurp(win_root, "alpmrpc-scriptlet.log", log, sizeof(log));
	check(strstr(log, "alpmrpc-base post_install") != NULL,
	      "the scriptlet ran, chrooted into the root",
	      "alpmrpc-scriptlet.log");
	check(scriptlet_lines > 0,
	      "and its output arrived as ALPM_EVENT_SCRIPTLET_INFO",
	      first_scriptlet);

	slurp(win_root, "alpmrpc-hook.log", log, sizeof(log));
	check(strstr(log, "hook ran") != NULL, "the post-transaction hook ran",
	      "alpmrpc-hook.log");
	check(hook_starts > 0 && hook_runs > 0,
	      "and was narrated by ALPM_EVENT_HOOK_*", hook_desc);

	/* ---- the conflict, which is a question ---- */

	printf("\n-- installing alpmrpc-rival, which conflicts --\n");
	int before = questions;
	rc = install(h, rival_pkg, &stage);
	check(rc == 0, "the transaction committed",
	      rc == 0 ? NULL : trans_err(h));
	if (rc != 0)
		printf("       failed at %s\n", stage);

	check(questions > before, "a question reached the callback",
	      questions > before ? "first one ever to" : "none fired");
	check(last_question == ALPM_QUESTION_CONFLICT_PKG,
	      "it was ALPM_QUESTION_CONFLICT_PKG", conflict_pair);
	check(question_inside_commit || questions > before,
	      "asked from inside a call this process was waiting on", NULL);
	check(conflict_pair[0] != '\0',
	      "the packages in it are usable handles",
	      "alpm_pkg_get_name through a question");

	/* The answer is the point: saying yes had to reach libalpm before it
	 * would go on, and what it did next is the proof it arrived. */
	check(op_remove == 1, "answering yes removed the conflicting package",
	      NULL);
	check(installed_count(h) == 1, "one package installed, not two", NULL);

	slurp(win_root, "alpmrpc-scriptlet.log", log, sizeof(log));
	check(strstr(log, "alpmrpc-base pre_remove") != NULL,
	      "the remove scriptlet ran too", NULL);
	check(count_lines(log) == 3, "three scriptlet runs in all",
	      "install, remove, install");

	/* ---- the download path ---- */

	printf("\n-- a file:// repo, so downloading happens with no network --\n");
	char server[1024], buf[256];
	snprintf(server, sizeof(server), "file://%s/repo", posix_root);

	check(alpm_option_set_dlcb(h, on_dl, NULL) == 0,
	      "alpm_option_set_dlcb()", NULL);

	alpm_db_t *sync = alpm_register_syncdb(h, "alpmrpc", 0);
	check(sync != NULL, "alpm_register_syncdb()", "alpmrpc");
	check(sync && alpm_db_add_server(sync, server) == 0,
	      "alpm_db_add_server()", server);

	alpm_list_t *dbs = NULL;
	alpm_list_append(&dbs, sync);
	int up = alpm_db_update(h, dbs, 1);
	alpm_list_free(dbs);

	check(up == 0, "alpm_db_update() fetched the database",
	      up == 0 ? NULL : trans_err(h));
	check(db_retrieve_start > 0, "ALPM_EVENT_DB_RETRIEVE_START arrived",
	      NULL);
	snprintf(buf, sizeof(buf), "%d call%s, first for %s", dl_calls,
		 dl_calls == 1 ? "" : "s", dl_file);
	check(dl_calls > 0, "the download callback fired", buf);
	check(dl_init > 0 && dl_completed > 0,
	      "with INIT and COMPLETED, payloads and all", NULL);

	printf("\n-- installing from the repo, which has to fetch first --\n");
	/* alpmrpc-extra is only in the repo, never in the cache, so libalpm
	 * has to download it -- which is the one thing that raises
	 * ALPM_EVENT_PKG_RETRIEVE_START. */
	alpm_pkg_t *extra = alpm_db_get_pkg(sync, "alpmrpc-extra");
	check(extra != NULL, "alpm_db_get_pkg() off the sync db",
	      "alpmrpc-extra");
	if (extra) {
		rc = add_and_commit(h, extra, &stage);
		check(rc == 0, "the transaction committed",
		      rc == 0 ? NULL : trans_err(h));
		if (rc != 0)
			printf("       failed at %s\n", stage);
		snprintf(buf, sizeof(buf), "%lld package%s, %lld bytes",
			 pkg_retrieve_num, pkg_retrieve_num == 1 ? "" : "s",
			 pkg_retrieve_size);
		check(pkg_retrieve_start > 0,
		      "ALPM_EVENT_PKG_RETRIEVE_START arrived", buf);
		check(pkg_retrieve_num == 1 && pkg_retrieve_size > 0,
		      "carrying a count and a size that make sense", NULL);
		check(installed_count(h) == 2, "two packages installed now",
		      NULL);
	}

	printf("\n-- a fetchcb takes downloading away from libalpm --\n");
	check(alpm_option_set_fetchcb(h, on_fetch, NULL) == 0,
	      "alpm_option_set_fetchcb()", NULL);
	dbs = NULL;
	alpm_list_append(&dbs, sync);
	up = alpm_db_update(h, dbs, 1);
	alpm_list_free(dbs);

	check(fetch_calls > 0, "libalpm asked this process to do the download",
	      fetch_url);
	check(strstr(fetch_url, "alpmrpc.db") != NULL,
	      "with the url it wanted fetched", fetch_url);
	check(fetch_localpath[0] == '/',
	      "and a server-side path to put it in", fetch_localpath);
	check(up != 0, "refusing it failed the update, rather than passing",
	      NULL);
	alpm_option_set_fetchcb(h, NULL, NULL);

	printf("\n-- the connection survived all of it --\n");
	check(events > 0, "events were delivered throughout", NULL);
	const char *root = alpm_option_get_root(h);
	check(root != NULL, "an ordinary call still works after committing",
	      root);

	alpm_release(h);
	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
