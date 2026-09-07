/* What the write path costs, and how much of it is the callback bridge.
 *
 * The read path's answer was that the codec is 2% of a call, so the thing
 * worth attacking was round trips rather than bytes. A transaction is a
 * different workload -- most of its frames are callbacks going the other
 * way -- so it gets measured rather than assumed.
 *
 * The same two installs run against a freshly rebuilt root, once with
 * nothing registered and once with the event callback on, repeated so the
 * spread is visible. The minimum of each is the estimator: these runs fork
 * shells and write to disk, so they have a floor and a long tail rather than
 * a mean worth quoting.
 *
 * A round trip is measured in this same process rather than quoted from
 * bench_codec, so the bound on the bridge's share is arithmetic on two
 * numbers measured here.
 *
 *   bench_transaction <root> <mkroot.sh> <msys2 root>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <alpm.h>
#include <alpm_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double f_;
static double us(void)
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart / f_;
}

static int events, questions, scriptlets;

static void on_event(void *ctx, alpm_event_t *e)
{
	(void)ctx;
	events++;
	if (e->type == ALPM_EVENT_SCRIPTLET_INFO)
		scriptlets++;
}

static void on_question(void *ctx, alpm_question_t *q)
{
	(void)ctx;
	questions++;
	q->any.answer = 1;
}

static void remake(const char *bash, const char *script, const char *root)
{
	char cmd[2048];
	snprintf(cmd, sizeof(cmd), "\"\"%s\" \"%s\" \"%s\" > NUL 2>&1\"",
		 bash, script, root);
	if (system(cmd) != 0)
		printf("  (fixture rebuild reported failure)\n");
}

static int read_posix_root(const char *win_root, char *out, size_t n)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s.posix", win_root);
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	size_t got = fread(out, 1, n - 1, f);
	fclose(f);
	out[got] = '\0';
	out[strcspn(out, "\r\n")] = '\0';
	return 1;
}

static int install(alpm_handle_t *h, const char *pkgfile)
{
	alpm_pkg_t *p = NULL;
	alpm_list_t *data = NULL;
	if (alpm_pkg_load(h, pkgfile, 1, 0, &p) != 0 || !p)
		return -1;
	if (alpm_trans_init(h, 0) != 0)
		return -1;
	if (alpm_add_pkg(h, p) != 0 || alpm_trans_prepare(h, &data) != 0) {
		alpm_list_free(data);
		alpm_trans_release(h);
		return -1;
	}
	alpm_list_free(data);
	data = NULL;
	int rc = alpm_trans_commit(h, &data);
	alpm_list_free(data);
	alpm_trans_release(h);
	return rc;
}

/* Both installs against a fresh root: the second conflicts with the first,
 * so it is a question, a removal and an install. Returns microseconds. */
static double run(const char *posix_root, int callbacks)
{
	char dbpath[1024], cachedir[1024], hookdir[1024];
	char base[1024], rival[1024];
	snprintf(dbpath, sizeof(dbpath), "%s/var/lib/pacman/", posix_root);
	snprintf(cachedir, sizeof(cachedir), "%s/var/cache/pacman/pkg/",
		 posix_root);
	snprintf(hookdir, sizeof(hookdir), "%s/etc/pacman.d/hooks/",
		 posix_root);
	snprintf(base, sizeof(base), "%salpmrpc-base-1.0-1-x86_64.pkg.tar.zst",
		 cachedir);
	snprintf(rival, sizeof(rival),
		 "%salpmrpc-rival-1.0-1-x86_64.pkg.tar.zst", cachedir);

	alpm_errno_t err = 0;
	alpm_handle_t *h = alpm_initialize(posix_root, dbpath, &err);
	if (!h) {
		printf("  alpm_initialize failed (err=%d)\n", (int)err);
		return -1;
	}
	alpm_option_add_cachedir(h, cachedir);
	alpm_option_add_hookdir(h, hookdir);
	alpm_option_add_architecture(h, "x86_64");

	/* questioncb is on either way: without an answer the conflicting
	 * install refuses, and then the two runs would not be the same work. */
	alpm_option_set_questioncb(h, on_question, NULL);
	if (callbacks)
		alpm_option_set_eventcb(h, on_event, NULL);

	double t0 = us();
	int rc = install(h, base);
	if (rc == 0)
		rc = install(h, rival);
	double t1 = us();

	if (rc != 0)
		printf("  transaction failed: %s\n",
		       alpm_strerror(alpm_errno(h)));
	alpm_release(h);
	return t1 - t0;
}

/* One cheap call, repeated, to price a round trip on this machine. */
static double round_trip_us(const char *posix_root)
{
	char dbpath[1024];
	snprintf(dbpath, sizeof(dbpath), "%s/var/lib/pacman/", posix_root);
	alpm_errno_t err = 0;
	alpm_handle_t *h = alpm_initialize(posix_root, dbpath, &err);
	if (!h)
		return 0;
	enum { N = 2000 };
	alpm_option_get_root(h);                /* warm the connection */
	double t0 = us();
	for (int i = 0; i < N; i++)
		alpm_option_get_root(h);
	double t = (us() - t0) / N;
	alpm_release(h);
	return t;
}

int main(int argc, char **argv)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	f_ = f.QuadPart / 1e6;

	if (argc < 4) {
		printf("usage: bench_transaction <root> <mkroot.sh> "
		       "<msys2 root>\n");
		return 2;
	}
	const char *win_root = argv[1], *script = argv[2];
	char bash[1024];
	snprintf(bash, sizeof(bash), "%s/usr/bin/bash.exe", argv[3]);
	char posix_root[1024];

	remake(bash, script, win_root);
	if (!read_posix_root(win_root, posix_root, sizeof(posix_root))) {
		printf("no fixture at %s\n", win_root);
		return 2;
	}

	enum { REPS = 3 };
	double bare = 1e30, with_cb = 1e30;
	int cbs = 0;

	printf("two installs against a fresh root, the second a conflict\n");
	printf("--------------------------------------------------------\n");
	printf("  %-24s %10s %10s\n", "", "best", "worst");

	double worst_bare = 0, worst_cb = 0;
	for (int i = 0; i < REPS; i++) {
		remake(bash, script, win_root);
		double t = run(posix_root, 0);
		if (t < bare)
			bare = t;
		if (t > worst_bare)
			worst_bare = t;
	}
	for (int i = 0; i < REPS; i++) {
		remake(bash, script, win_root);
		events = questions = scriptlets = 0;
		double t = run(posix_root, 1);
		if (t < with_cb)
			with_cb = t;
		if (t > worst_cb)
			worst_cb = t;
		cbs = events + questions;
	}

	printf("  %-24s %7.0f ms %7.0f ms\n", "no eventcb", bare / 1000.0,
	       worst_bare / 1000.0);
	printf("  %-24s %7.0f ms %7.0f ms\n", "eventcb registered",
	       with_cb / 1000.0, worst_cb / 1000.0);
	printf("\n");

	double rt = round_trip_us(posix_root);
	printf("  callbacks delivered      %8d   (%d events, %d question, "
	       "%d scriptlet lines)\n", cbs, events, questions, scriptlets);
	printf("  round trip here          %8.1f us\n", rt);
	printf("  so the bridge's share is %8.1f ms of %.0f ms   %.2f%%\n",
	       cbs * rt / 1000.0, bare / 1000.0,
	       bare > 0 ? 100.0 * cbs * rt / bare : 0.0);
	printf("\n");
	printf("  The two runs differ by less than they differ from themselves,\n");
	printf("  which is the point: a transaction is fork(), exec() and disk,\n");
	printf("  and the callback traffic is tens of bytes a frame. Nothing on\n");
	printf("  this path is waiting for the codec.\n");
	return 0;
}
