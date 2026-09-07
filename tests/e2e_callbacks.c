/* Callbacks fire on the server, inside a call this process is waiting on.
 *
 * The check that matters is not that a callback runs, but that it runs *at
 * the right time*: the server is blocked inside libalpm, the client is
 * blocked waiting for that call's reply, and the callback has to be
 * delivered and answered in between. If the frame loop were wrong this would
 * deadlock rather than fail, so the test is written to make that visible.
 */
#include <alpm.h>
#include <alpm_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int log_calls;
static char last_msg[512];
static alpm_loglevel_t last_level;
static void *last_ctx;
static int inside_call;
static int fired_during_call;

static int event_calls;
static int event_during_call;
static alpm_event_type_t last_event;
static char last_dbname[256];
static int saw_database_missing;

static void check(int cond, const char *what, const char *detail)
{
	printf("%-6s %-42s %s\n", cond ? "ok" : "FAIL", what,
	       detail ? detail : "");
	if (!cond)
		failures++;
}

static void my_log(void *ctx, alpm_loglevel_t level, const char *fmt,
		   va_list args)
{
	log_calls++;
	last_level = level;
	last_ctx = ctx;
	if (inside_call)
		fired_during_call = 1;
	vsnprintf(last_msg, sizeof(last_msg), fmt, args);
}

static void my_event(void *ctx, alpm_event_t *e)
{
	(void)ctx;
	event_calls++;
	last_event = e->type;
	if (inside_call)
		event_during_call = 1;
	if (e->type == ALPM_EVENT_DATABASE_MISSING) {
		saw_database_missing = 1;
		snprintf(last_dbname, sizeof(last_dbname), "%s",
			 e->database_missing.dbname ? e->database_missing.dbname
						    : "(null)");
	}
}

int main(void)
{
	alpm_errno_t err = 0;
	alpm_handle_t *h = alpm_initialize("/", "/var/lib/pacman/", &err);
	if (!h) {
		printf("alpm_initialize failed (err=%d)\n", (int)err);
		return 1;
	}

	printf("-- registering --\n");
	int marker = 0;
	int rc = alpm_option_set_logcb(h, my_log, &marker);
	check(rc == 0, "alpm_option_set_logcb()", NULL);
	check(alpm_option_get_logcb(h) == my_log,
	      "alpm_option_get_logcb() round-trips the pointer", NULL);

	printf("\n-- a callback delivered mid-call --\n");
	/* alpm_logaction makes libalpm log, which calls back into this
	 * process while the server is still inside the call. */
	inside_call = 1;
	alpm_option_set_logcb(h, my_log, &marker);
	/* Registering a syncdb for a repo with no servers makes libalpm log,
	 * and does so from inside the call. */
	alpm_db_t *db = alpm_register_syncdb(h, "alpmrpc-test", 0);
	(void)db;
	alpm_option_get_root(h);        /* a plain call, to flush any queue */
	inside_call = 0;

	char buf[64];
	snprintf(buf, sizeof(buf), "%d call%s", log_calls,
		 log_calls == 1 ? "" : "s");
	check(log_calls > 0, "the log callback fired", buf);
	if (log_calls > 0) {
		check(fired_during_call,
		      "and fired while a call was outstanding",
		      "nested, not queued");
		check(last_ctx == &marker, "ctx reached the callback intact",
		      NULL);
		check(last_msg[0] != '\0', "message text survived formatting",
		      last_msg);
	}

	printf("\n-- unregistering stops delivery --\n");
	int before = log_calls;
	rc = alpm_option_set_logcb(h, NULL, NULL);
	check(rc == 0, "alpm_option_set_logcb(NULL)", NULL);
	alpm_register_syncdb(h, "alpmrpc-test-2", 0);
	alpm_option_get_root(h);
	snprintf(buf, sizeof(buf), "%d -> %d", before, log_calls);
	check(log_calls == before, "no further calls after unregistering", buf);

	printf("\n-- an event carries its union payload --\n");
	/* A sync db registered for a repo whose database was never downloaded
	 * makes libalpm raise ALPM_EVENT_DATABASE_MISSING when something asks
	 * for its packages. That is the cheapest real event that carries a
	 * payload rather than only a type, so it is what checks the union
	 * member actually crosses. */
	rc = alpm_option_set_eventcb(h, my_event, &marker);
	check(rc == 0, "alpm_option_set_eventcb()", "trampoline installed");
	check(alpm_option_get_eventcb(h) == my_event,
	      "alpm_option_get_eventcb() round-trips the pointer", NULL);

	inside_call = 1;
	alpm_db_t *missing = alpm_register_syncdb(h, "alpmrpc-absent", 0);
	alpm_list_t *nothing = alpm_db_get_pkgcache(missing);
	(void)nothing;
	inside_call = 0;

	snprintf(buf, sizeof(buf), "%d event%s, last type %d", event_calls,
		 event_calls == 1 ? "" : "s", (int)last_event);
	check(event_calls > 0, "the event callback fired", buf);
	check(event_during_call, "and fired while a call was outstanding",
	      "nested, not queued");
	check(saw_database_missing, "ALPM_EVENT_DATABASE_MISSING arrived",
	      NULL);
	check(saw_database_missing
	      && !strcmp(last_dbname, "alpmrpc-absent"),
	      "its dbname came through the union", last_dbname);

	printf("\n-- callbacks that are not marshalled yet refuse --\n");
	/* Registering and then silently never firing would be worse than
	 * failing here, so these report failure rather than pretending. */
	check(alpm_option_set_dlcb(h, NULL, NULL) == -1,
	      "alpm_option_set_dlcb() reports failure", "not yet marshalled");
	check(alpm_option_set_fetchcb(h, NULL, NULL) == -1,
	      "alpm_option_set_fetchcb() reports failure", NULL);

	printf("\n-- questions install a real trampoline --\n");
	/* This returned -1 before questioncb was carried. It succeeding is what
	 * says the server registered a trampoline with libalpm. Whether a
	 * question then *fires* needs libalpm to have something to ask, which a
	 * read-only test cannot arrange -- see the note in the README. */
	rc = alpm_option_set_questioncb(h, (alpm_cb_question)0, NULL);
	check(rc == 0, "alpm_option_set_questioncb()", "trampoline installed");

	printf("\n-- ordinary calls still work around callbacks --\n");
	const char *root = alpm_option_get_root(h);
	check(root && root[0] == '/', "alpm_option_get_root()", root);
	alpm_db_t *local = alpm_get_localdb(h);
	alpm_list_t *cache = alpm_db_get_pkgcache(local);
	snprintf(buf, sizeof(buf), "%zu packages", alpm_list_count(cache));
	check(cache != NULL, "pkgcache still lists", buf);

	alpm_release(h);
	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
