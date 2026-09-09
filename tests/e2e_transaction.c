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

#include <archive.h>
#include <archive_entry.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not libalpm API: the bridge's own count of what it has cached, so the
 * test can see that a release leaves nothing behind. */
extern size_t arpc_stats_cached(void);

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
static int remove_pkgs_questions;
static char remove_pkgs_first[128];

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
	} else if (q->type == ALPM_QUESTION_REMOVE_PKGS) {
		/* A package whose dependencies cannot be resolved: libalpm
		 * offers to drop it from the transaction and carry on. Saying
		 * no is what makes prepare fail with the unsatisfied
		 * dependency in data, which is what that test is after. The
		 * list carries the package as a usable handle. */
		remove_pkgs_questions++;
		alpm_list_t *pkgs = q->remove_pkgs.packages;
		snprintf(remove_pkgs_first, sizeof(remove_pkgs_first), "%s",
			 pkgs && pkgs->data
				 ? alpm_pkg_get_name((alpm_pkg_t *)pkgs->data)
				 : "(none)");
		q->remove_pkgs.skip = 0;
	} else {
		q->any.answer = 1;
	}
}

/* ---- driving a transaction ---- */

static const char *trans_err(alpm_handle_t *h)
{
	return alpm_strerror(alpm_errno(h));
}

/* What alpm_trans_get_add() named once the target was added: it is a
 * borrowed list, and one transaction's must not be served for the next. */
static char last_target[128];

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
	alpm_list_t *add = alpm_trans_get_add(h);
	snprintf(last_target, sizeof(last_target), "%s",
		 add && add->data ? alpm_pkg_get_name((alpm_pkg_t *)add->data)
				  : "(none)");

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

/* A transaction that is expected to be refused. Loads the files, adds them,
 * and runs it as far as `stage` says; what comes back in `data`, and which
 * errno explains it, is the caller's to check. The transaction is left open
 * so that the data can be read while its packages are alive. */
static int refuse(alpm_handle_t *h, const char **files, int nfiles,
		  int commit, alpm_list_t **data)
{
	*data = NULL;
	if (alpm_trans_init(h, 0) != 0)
		return -1;
	for (int i = 0; i < nfiles; i++) {
		alpm_pkg_t *p = NULL;
		if (alpm_pkg_load(h, files[i], 1, 0, &p) != 0 || !p
		    || alpm_add_pkg(h, p) != 0)
			return -1;
	}
	int rc = alpm_trans_prepare(h, data);
	if (rc != 0 || !commit)
		return rc;
	alpm_list_free(*data);
	*data = NULL;
	return alpm_trans_commit(h, data);
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
	char needy_pkg[1024], alien_pkg[1024], squatter_pkg[1024];
	snprintf(needy_pkg, sizeof(needy_pkg),
		 "%s/var/cache/pacman/pkg/alpmrpc-needy-1.0-1-x86_64.pkg.tar.zst",
		 posix_root);
	snprintf(alien_pkg, sizeof(alien_pkg),
		 "%s/var/cache/pacman/pkg/alpmrpc-alien-1.0-1-alien.pkg.tar.zst",
		 posix_root);
	snprintf(squatter_pkg, sizeof(squatter_pkg),
		 "%s/var/cache/pacman/pkg/"
		 "alpmrpc-squatter-1.0-1-x86_64.pkg.tar.zst", posix_root);

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
	check(!strcmp(last_target, "alpmrpc-base"),
	      "alpm_trans_get_add() named the target", last_target);
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

	/* ---- the file list, which is a counted array ---- */

	printf("\n-- the package's file list --\n");
	char buf[256];
	alpm_db_t *localdb = alpm_get_localdb(h);
	alpm_pkg_t *installed = localdb ?
		alpm_db_get_pkg(localdb, "alpmrpc-base") : NULL;
	check(installed != NULL, "alpm_db_get_pkg() off the local db",
	      "alpmrpc-base");

	alpm_filelist_t *fl = installed ? alpm_pkg_get_files(installed) : NULL;
	snprintf(buf, sizeof(buf), "%zu entries", fl ? fl->count : 0);
	check(fl != NULL && fl->count > 1,
	      "alpm_pkg_get_files() brought the whole array", buf);
	/* The reason this record was refused for so long is that materialising
	 * it field by field yields exactly one element and looks fine. So the
	 * count is the test, and so is the last entry differing from the
	 * first. */
	if (fl && fl->count > 1) {
		check(fl->files[fl->count - 1].name != NULL
		      && strcmp(fl->files[0].name,
				fl->files[fl->count - 1].name) != 0,
		      "the last entry is not the first one repeated",
		      fl->files[fl->count - 1].name);
	}

	const char *owned = "usr/share/alpmrpc-scratch/alpmrpc-base.txt";
	alpm_file_t *found = alpm_filelist_contains(fl, owned);
	check(found != NULL, "alpm_filelist_contains() finds a file it owns",
	      owned);
	if (found) {
		check(found >= fl->files && found < fl->files + fl->count,
		      "and returns a pointer into the caller's own array",
		      "not a copy");
		snprintf(buf, sizeof(buf), "%lld bytes", (long long)found->size);
		check(found->size > 0, "with the rest of the entry filled in",
		      buf);
	}
	check(alpm_filelist_contains(fl, "usr/share/alpmrpc-scratch/nope.txt")
	      == NULL, "and does not find one it does not", NULL);
	check(fl != NULL && alpm_pkg_get_files(installed) == fl,
	      "a repeat call is the same pointer", "borrowed, so cached");
	check(localdb && alpm_get_localdb(h) == localdb,
	      "and so is the db itself, asked for again",
	      "the same object is the same id");
	alpm_list_t *held = localdb ? alpm_db_get_pkgcache(localdb) : NULL;
	check(held && alpm_list_count(held) == 1
	      && (alpm_pkg_t *)held->data == installed,
	      "the pkgcache lists that same package object", NULL);

	/* ---- the one batched field a setter changes ---- */

	printf("\n-- a batched field that a setter changes --\n");
	/* The install reason is the one package field a setter changes. Its
	 * column is what alpm_pkg_set_reason drops, and nothing else is. */
	check(installed
	      && alpm_pkg_get_reason(installed) == ALPM_PKG_REASON_EXPLICIT,
	      "alpm_pkg_get_reason() reads explicit", NULL);
	int src = installed
		? alpm_pkg_set_reason(installed, ALPM_PKG_REASON_DEPEND) : -1;
	check(src == 0, "alpm_pkg_set_reason(depend)",
	      src == 0 ? NULL : trans_err(h));
	check(installed
	      && alpm_pkg_get_reason(installed) == ALPM_PKG_REASON_DEPEND,
	      "and the column re-reads it", "that column was dropped");
	check(localdb && alpm_db_get_pkgcache(localdb) == held,
	      "and nothing else was touched", "the pkgcache is the same list");

	/* ---- the refusals, each with a differently typed list ----
	 *
	 * The header's comment on alpm_trans_prepare names alpm_depmissing_t,
	 * but what `data` holds depends on why the call failed: pacman's own
	 * reader switches on alpm_errno() to tell, and so must this bridge.
	 * Reading a conflict as a depmissing is a crash, so each kind is
	 * driven for real. The packages that provoke them are in the fixture
	 * for no other reason. */

	printf("\n-- refused: a missing dependency --\n");
	{
		const char *files[] = { needy_pkg };
		alpm_list_t *data = NULL;
		int prc = refuse(h, files, 1, 0, &data);
		check(remove_pkgs_questions == 1,
		      "libalpm first asked ALPM_QUESTION_REMOVE_PKGS",
		      remove_pkgs_first);
		check(!strcmp(remove_pkgs_first, "alpmrpc-needy"),
		      "naming the package, as a usable handle in a list", NULL);
		check(prc != 0 && alpm_errno(h) == ALPM_ERR_UNSATISFIED_DEPS,
		      "refusing that, prepare fails with "
		      "ALPM_ERR_UNSATISFIED_DEPS", trans_err(h));
		check(alpm_list_count(data) == 1,
		      "and data lists the one missing dependency", NULL);
		if (data) {
			alpm_depmissing_t *m = (alpm_depmissing_t *)data->data;
			check(m->target && !strcmp(m->target, "alpmrpc-needy"),
			      "as an alpm_depmissing_t naming the target",
			      m->target);
			check(m->depend && m->depend->name
			      && !strcmp(m->depend->name, "alpmrpc-missing"),
			      "and what it wanted",
			      m->depend ? m->depend->name : NULL);
			for (alpm_list_t *l = data; l; l = l->next)
				alpm_depmissing_free((alpm_depmissing_t *)l->data);
			alpm_list_free(data);
		}
		alpm_trans_release(h);
	}

	printf("\n-- refused: conflicting targets --\n");
	/* Both packages that conflict by name in one transaction: neither can
	 * be dropped in favour of the other, so this is an error rather than
	 * a question, and data holds alpm_conflict_t. */
	{
		const char *files[] = { base_pkg, rival_pkg };
		alpm_list_t *data = NULL;
		int prc = refuse(h, files, 2, 0, &data);
		check(prc != 0 && alpm_errno(h) == ALPM_ERR_CONFLICTING_DEPS,
		      "prepare fails with ALPM_ERR_CONFLICTING_DEPS",
		      trans_err(h));
		check(alpm_list_count(data) == 1,
		      "and data lists the one conflict", NULL);
		if (data) {
			alpm_conflict_t *cf = (alpm_conflict_t *)data->data;
			const char *n1 = cf->package1
				? alpm_pkg_get_name(cf->package1) : NULL;
			const char *n2 = cf->package2
				? alpm_pkg_get_name(cf->package2) : NULL;
			snprintf(buf, sizeof(buf), "%s vs %s", n1 ? n1 : "?",
				 n2 ? n2 : "?");
			check(n1 && n2 && strstr(n1, "alpmrpc-")
			      && strstr(n2, "alpmrpc-"),
			      "as an alpm_conflict_t whose packages are usable",
			      buf);
			/* libalpm makes copies of the packages for a conflict
			 * it hands to its caller, so these are not the objects
			 * that were added; they are readable until the
			 * conflict is freed, which for the copies on the
			 * server side means until the handle is released. */
			check(n1 && n2
			      && ((!strcmp(n1, "alpmrpc-rival")
				   && !strcmp(n2, "alpmrpc-base"))
				  || (!strcmp(n1, "alpmrpc-base")
				      && !strcmp(n2, "alpmrpc-rival"))),
			      "and they are the two that were added",
			      "as copies made for the caller");
			check(cf->reason && cf->reason->name
			      && !strcmp(cf->reason->name, "alpmrpc-base"),
			      "with the depend that says so as the reason",
			      cf->reason ? cf->reason->name : NULL);
			for (alpm_list_t *l = data; l; l = l->next)
				alpm_conflict_free((alpm_conflict_t *)l->data);
			alpm_list_free(data);
		}
		alpm_trans_release(h);
	}

	printf("\n-- refused: a foreign architecture --\n");
	{
		const char *files[] = { alien_pkg };
		alpm_list_t *data = NULL;
		int prc = refuse(h, files, 1, 0, &data);
		check(prc != 0 && alpm_errno(h) == ALPM_ERR_PKG_INVALID_ARCH,
		      "prepare fails with ALPM_ERR_PKG_INVALID_ARCH",
		      trans_err(h));
		check(alpm_list_count(data) == 1,
		      "and data lists the one package", NULL);
		if (data) {
			const char *s = (const char *)data->data;
			check(s && !strcmp(s, "alpmrpc-alien-1.0-1-alien"),
			      "as a plain string, name-version-arch", s);
			alpm_list_free_inner(data, free);
			alpm_list_free(data);
		}
		alpm_trans_release(h);
	}

	printf("\n-- refused: a file an installed package owns --\n");
	/* alpmrpc-squatter ships the file alpmrpc-base already has. Nothing
	 * conflicts by name, so prepare passes and it is the commit that
	 * refuses, with alpm_fileconflict_t in data. */
	{
		const char *files[] = { squatter_pkg };
		alpm_list_t *data = NULL;
		int crc = refuse(h, files, 1, 1, &data);
		check(crc != 0 && alpm_errno(h) == ALPM_ERR_FILE_CONFLICTS,
		      "commit fails with ALPM_ERR_FILE_CONFLICTS",
		      trans_err(h));
		check(alpm_list_count(data) == 1,
		      "and data lists the one file conflict", NULL);
		if (data) {
			alpm_fileconflict_t *fc =
				(alpm_fileconflict_t *)data->data;
			check(fc->target
			      && !strcmp(fc->target, "alpmrpc-squatter"),
			      "as an alpm_fileconflict_t naming the target",
			      fc->target);
			check(fc->file && strstr(fc->file, "alpmrpc-base.txt"),
			      "and the file", fc->file);
			check(fc->ctarget
			      && !strcmp(fc->ctarget, "alpmrpc-base"),
			      "and who owns it", fc->ctarget);
			for (alpm_list_t *l = data; l; l = l->next)
				alpm_fileconflict_free(
					(alpm_fileconflict_t *)l->data);
			alpm_list_free(data);
		}
		alpm_trans_release(h);
		check(installed_count(h) == 1, "and nothing was installed",
		      NULL);
	}

	/* ---- the conflict, which is a question ---- */

	printf("\n-- installing alpmrpc-rival, which conflicts --\n");
	int before = questions;
	rc = install(h, rival_pkg, &stage);
	check(rc == 0, "the transaction committed",
	      rc == 0 ? NULL : trans_err(h));
	if (rc != 0)
		printf("       failed at %s\n", stage);
	check(!strcmp(last_target, "alpmrpc-rival"),
	      "alpm_trans_get_add() named this transaction's target",
	      last_target);

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

	/* The pkgcache fetched before the commit is a borrowed list. libalpm
	 * rewrote it; a re-read has to see that, and the earlier list has to
	 * stay readable, because natively it would be the same memory. */
	alpm_list_t *now = localdb ? alpm_db_get_pkgcache(localdb) : NULL;
	check(now && now != held,
	      "alpm_db_get_pkgcache() re-read after the commit",
	      "not the list from before it");
	check(now && now->data
	      && !strcmp(alpm_pkg_get_name((alpm_pkg_t *)now->data),
			 "alpmrpc-rival"),
	      "and it lists what is installed now",
	      now && now->data ? alpm_pkg_get_name((alpm_pkg_t *)now->data)
			       : NULL);
	check(held && alpm_list_count(held) == 1,
	      "the list from before is still readable memory",
	      "detached, not freed");
	/* A numeric field, because those are re-read after a commit; a string
	 * a caller already holds is kept, as libalpm's would be. */
	check(alpm_pkg_get_isize(installed) == 0,
	      "the package the commit removed no longer resolves",
	      "its id was dropped, so a fresh read misses");

	slurp(win_root, "alpmrpc-scriptlet.log", log, sizeof(log));
	check(strstr(log, "alpmrpc-base pre_remove") != NULL,
	      "the remove scriptlet ran too", NULL);
	check(count_lines(log) == 3, "three scriptlet runs in all",
	      "install, remove, install");

	/* ---- the download path ---- */

	printf("\n-- a file:// repo, so downloading happens with no network --\n");
	char server[1024];
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

	printf("\n-- alpm_fetch_pkgurl fetches by url --\n");
	/* alpmrpc-spare exists only in the repo, so this is a real fetch
	 * rather than a lookup of something already in the cache. */
	char url[1200];
	snprintf(url, sizeof(url),
		 "%s/alpmrpc-spare-1.0-1-x86_64.pkg.tar.zst", server);
	alpm_list_t *urls = NULL, *fetched = NULL;
	alpm_list_append(&urls, url);
	rc = alpm_fetch_pkgurl(h, urls, &fetched);
	alpm_list_free(urls);

	check(rc == 0, "alpm_fetch_pkgurl()", rc == 0 ? NULL : trans_err(h));
	snprintf(buf, sizeof(buf), "%zu path%s", alpm_list_count(fetched),
		 alpm_list_count(fetched) == 1 ? "" : "s");
	check(alpm_list_count(fetched) == 1, "one local path came back", buf);
	if (fetched) {
		const char *got = (const char *)fetched->data;
		check(got && strstr(got, "alpmrpc-spare") != NULL,
		      "naming the file it fetched", got);
	}
	/* The overlay says caller-owned, so this is the documented idiom. */
	alpm_list_free_inner(fetched, free);
	alpm_list_free(fetched);

	printf("\n-- alpm_logaction formats here and writes there --\n");
	/* libalpm writes nothing without a logfile configured; pacman sets
	 * one from its config, and there is no config here. */
	char logfile[1024];
	snprintf(logfile, sizeof(logfile), "%s/var/log/pacman.log", posix_root);
	check(alpm_option_set_logfile(h, logfile) == 0,
	      "alpm_option_set_logfile()", logfile);

	/* The %% matters: the client formats it to a literal %, and if the
	 * server then used that text as a format string rather than as an
	 * argument to one, this is where it would go wrong. */
	rc = alpm_logaction(h, "alpmrpc", "fetched %d file%s, %d%% done\n",
			    1, "", 50);
	check(rc == 0, "alpm_logaction()", NULL);
	slurp(win_root, "var/log/pacman.log", log, sizeof(log));
	check(strstr(log, "fetched 1 file, 50% done") != NULL,
	      "arguments formatted, and the % survived as a %",
	      "var/log/pacman.log");

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

	/* ---- what a later reader sees of what was installed ----
	 *
	 * A second handle on the same root, because libalpm attaches neither a
	 * changelog reader nor an mtree reader to a package its local db cache
	 * got from a transaction in this session -- both stay NULL until the
	 * db is read from disk again. That is what a fresh handle does, and it
	 * is what the next pacman to run would see. Nothing about it is
	 * peculiar to this bridge; a native caller finds the same. */

	printf("\n-- a fresh handle re-reads what was installed --\n");
	alpm_errno_t err2 = 0;
	alpm_handle_t *h2 = alpm_initialize(posix_root, dbpath, &err2);
	check(h2 != NULL, "a second alpm_initialize() on the same root", NULL);

	alpm_db_t *local2 = h2 ? alpm_get_localdb(h2) : NULL;
	alpm_pkg_t *rival = local2 ?
		alpm_db_get_pkg(local2, "alpmrpc-rival") : NULL;
	check(rival != NULL, "alpm_db_get_pkg() off its local db",
	      "alpmrpc-rival");

	printf("\n-- the changelog, through a cursor --\n");
	void *cl = rival ? alpm_pkg_changelog_open(rival) : NULL;
	check(cl != NULL, "alpm_pkg_changelog_open()",
	      "an id, not a pointer this process could follow");
	if (cl) {
		/* Deliberately small, so the whole file cannot arrive in one
		 * read and the cursor has to be where it was left. */
		char chunk[16];
		char whole[1024];
		size_t total = 0, got;
		int reads = 0;

		while ((got = alpm_pkg_changelog_read(chunk, sizeof(chunk),
						      rival, cl)) > 0) {
			reads++;
			if (total + got < sizeof(whole)) {
				memcpy(whole + total, chunk, got);
				total += got;
			}
			if (reads > 200)
				break;
		}
		whole[total] = '\0';

		snprintf(buf, sizeof(buf), "%zu bytes over %d reads", total,
			 reads);
		check(reads > 1, "it took more than one read", buf);
		check(strstr(whole, "first release of alpmrpc-rival") != NULL,
		      "and the text came back whole", NULL);
		check(strstr(whole, "and a third") != NULL,
		      "including the last line, so nothing was lost between "
		      "reads", NULL);

		check(alpm_pkg_changelog_close(rival, cl) == 0,
		      "alpm_pkg_changelog_close()", NULL);
		/* The id is dropped when it closes, so this misses a lookup
		 * rather than reaching a cursor libalpm has already freed. */
		check(alpm_pkg_changelog_read(chunk, sizeof(chunk), rival, cl)
		      == 0,
		      "reading a closed cursor gets nothing", "the id is gone");
	}

	printf("\n-- the mtree, with this process's own libarchive --\n");
	struct archive *mt = rival ? alpm_pkg_mtree_open(rival) : NULL;
	check(mt != NULL, "alpm_pkg_mtree_open()", "a real struct archive");
	if (mt) {
		struct archive_entry *ent = NULL;
		int entries = 0, rc2, saw_dir = 0;
		long long txt_size = -1;
		char last_path[512] = "";

		while ((rc2 = alpm_pkg_mtree_next(rival, mt, &ent)) == 0) {
			const char *path = archive_entry_pathname(ent);
			entries++;
			if (!path)
				continue;
			snprintf(last_path, sizeof(last_path), "%s", path);
			if (!strcmp(path, "./usr/share/alpmrpc-scratch/"
					  "alpmrpc-rival.txt"))
				txt_size = (long long)archive_entry_size(ent);
			if (!strcmp(path, "./usr/share")
			    && archive_entry_filetype(ent) == AE_IFDIR)
				saw_dir = 1;
			if (entries > 100)
				break;
		}

		snprintf(buf, sizeof(buf), "%d entries, last %s", entries,
			 last_path);
		check(entries > 1, "it walked the whole listing", buf);
		check(rc2 == 1, "and stopped at the end, not on an error",
		      NULL);

		/* Read with libarchive's own accessors off an entry libarchive
		 * built, which is why the stream travels rather than a set of
		 * fields picked out here. */
		snprintf(buf, sizeof(buf), "%lld bytes", txt_size);
		check(txt_size == 20, "a file's size survived the round trip",
		      buf);
		check(saw_dir, "and so did a directory's type", "./usr/share");

		check(alpm_pkg_mtree_close(rival, mt) == 0,
		      "alpm_pkg_mtree_close()", NULL);
	}

	/* ---- a signature, which is a counted array of embedded keys ---- */

	printf("\n-- checking a package signature --\n");
	/* The fixture signs every package with a throwaway key and leaves the
	 * public half in the root's gpgdir, which is the shape pacman-key
	 * would have left. */
	char fpr[128];
	slurp(win_root, "signing-key", fpr, sizeof(fpr));
	fpr[strcspn(fpr, "\r\n")] = '\0';

	char gpgdir[1024];
	snprintf(gpgdir, sizeof(gpgdir), "%s/etc/pacman.d/gnupg/", posix_root);
	check(h2 && alpm_option_set_gpgdir(h2, gpgdir) == 0,
	      "alpm_option_set_gpgdir()", NULL);

	alpm_pkg_t *sp = NULL;
	if (h2)
		alpm_pkg_load(h2, base_pkg, 1, ALPM_SIG_PACKAGE, &sp);
	check(sp != NULL, "alpm_pkg_load() with signatures required",
	      sp ? NULL : trans_err(h2));

	if (sp) {
		alpm_siglist_t sl;
		int src = alpm_pkg_check_pgp_signature(sp, &sl);
		check(src == 0, "alpm_pkg_check_pgp_signature()",
		      src == 0 ? NULL : trans_err(h2));

		snprintf(buf, sizeof(buf), "%zu result%s", sl.count,
			 sl.count == 1 ? "" : "s");
		check(sl.count == 1, "the siglist is an array, not one entry",
		      buf);

		if (sl.count == 1) {
			alpm_sigresult_t *r0 = &sl.results[0];
			check(r0->status == ALPM_SIGSTATUS_VALID,
			      "the signature is valid", NULL);
			check(r0->validity == ALPM_SIGVALIDITY_FULL,
			      "and fully trusted", NULL);
			/* key is embedded by value, not through a pointer --
			 * the field kind the generator learned for this. */
			check(r0->key.fingerprint
			      && !strcmp(r0->key.fingerprint, fpr),
			      "the key's fingerprint crossed", r0->key.fingerprint);
			check(r0->key.email
			      && !strcmp(r0->key.email, "nobody@example.invalid"),
			      "and the rest of the key with it", r0->key.uid);
			/* The one field that does not cross. NULL here is the
			 * stated answer, not an accident: it is the server's
			 * gpgme key object. */
			check(r0->key.data == NULL,
			      "the gpgme key itself did not", "stated, not silent");
		}

		check(alpm_siglist_cleanup(&sl) == 0, "alpm_siglist_cleanup()",
		      NULL);
		check(sl.count == 0 && sl.results == NULL,
		      "which emptied it, so a second call is harmless", NULL);
		alpm_pkg_free(sp);
	}

	printf("\n-- and a database signature --\n");
	/* Registered demanding a signature this time, so the update fetches
	 * alpmrpc.db.sig alongside the db and there is something to check. */
	alpm_db_t *sdb = h2 ?
		alpm_register_syncdb(h2, "alpmrpc", ALPM_SIG_DATABASE) : NULL;
	if (sdb)
		alpm_db_add_server(sdb, server);

	alpm_list_t *dbs2 = NULL;
	alpm_list_append(&dbs2, sdb);
	int up2 = sdb ? alpm_db_update(h2, dbs2, 1) : -1;
	alpm_list_free(dbs2);
	check(up2 == 0, "alpm_db_update() with a signature required",
	      up2 == 0 ? NULL : trans_err(h2));

	if (up2 == 0) {
		alpm_siglist_t dsl;
		int drc = alpm_db_check_pgp_signature(sdb, &dsl);
		check(drc == 0, "alpm_db_check_pgp_signature()",
		      drc == 0 ? NULL : trans_err(h2));
		snprintf(buf, sizeof(buf), "%zu result%s", dsl.count,
			 dsl.count == 1 ? "" : "s");
		check(dsl.count == 1 && dsl.results[0].key.fingerprint
		      && !strcmp(dsl.results[0].key.fingerprint, fpr),
		      "signed by the same key", buf);
		alpm_siglist_cleanup(&dsl);
	}

	if (h2)
		alpm_release(h2);

	printf("\n-- the connection survived all of it --\n");
	check(events > 0, "events were delivered throughout", NULL);
	const char *root = alpm_option_get_root(h);
	check(root != NULL, "an ordinary call still works after committing",
	      root);
	check(fl && fl->count > 1 && fl->files[0].name,
	      "and a list borrowed before the commits is still readable",
	      "the handle it belongs to is still open");

	alpm_release(h);
	size_t left = arpc_stats_cached();
	snprintf(buf, sizeof(buf), "%zu left", left);
	check(left == 0, "the last release left nothing cached", buf);
	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
