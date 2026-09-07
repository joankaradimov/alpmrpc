/* Server half of the callback bridge.
 *
 * libalpm calls back from inside a call the client is waiting on. These
 * trampolines are what it actually calls; they serialise the arguments, send
 * them up the pipe as a callback frame, and block until the client answers.
 *
 * That blocking is deliberate, not a limitation. A question has to be
 * answered before the transaction that asked it can continue, so the reply
 * has to arrive before the trampoline returns. Doing it on the same thread
 * and the same connection is what keeps that ordering honest -- and keeps
 * the server single-threaded, which is what keeps libalpm's fork() on the
 * path Cygwin supports.
 */
#include "arpc_server.h"

#include <alpm.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The connection currently being served. One at a time, so a global is the
 * honest representation rather than a shortcut. */
static arpc_conn *g_conn;
static long long g_seq = 1;

void arpc_cb_set_conn(arpc_conn *c) { g_conn = c; }

/* Which callbacks the client has asked for, per handle. libalpm always gets
 * our trampoline; this says whether to bother sending anything. */
typedef struct cbreg {
	struct cbreg *next;
	uint64_t handle;
	int log, progress, event, question;
} cbreg;

static cbreg *g_regs;

static cbreg *reg_for(uint64_t handle, int create)
{
	for (cbreg *r = g_regs; r; r = r->next)
		if (r->handle == handle)
			return r;
	if (!create)
		return NULL;
	cbreg *r = (cbreg *)calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->handle = handle;
	r->next = g_regs;
	g_regs = r;
	return r;
}

void arpc_cb_purge(uint64_t handle)
{
	cbreg **pp = &g_regs;
	while (*pp) {
		if ((*pp)->handle == handle) {
			cbreg *dead = *pp;
			*pp = dead->next;
			free(dead);
		} else {
			pp = &(*pp)->next;
		}
	}
}

/* The handle a trampoline belongs to travels as libalpm's ctx pointer, since
 * that is the one thing libalpm hands back to us untouched. */
static uint64_t ctx_handle(void *ctx)
{
	return (uint64_t)(uintptr_t)ctx;
}

/* Send one callback frame and wait for the answer. Returns the reply's "ret"
 * or `dflt` if the connection failed -- a dead pipe must not wedge libalpm
 * mid-transaction. */
static long long cb_call(const char *which, uint64_t handle, aj_w *args,
			 long long dflt)
{
	if (!g_conn)
		return dflt;

	aj_w w;
	ajw_init(&w);
	ajw_obj_begin(&w);
	ajw_key(&w, "cb");     ajw_str(&w, which);
	ajw_key(&w, "seq");    ajw_i64(&w, g_seq++);
	ajw_key(&w, "handle"); ajw_i64(&w, (long long)handle);
	ajw_key(&w, "args");
	if (args && args->buf)
		ajw_raw(&w, args->buf, args->len);
	else
		ajw_null(&w);
	ajw_obj_end(&w);

	char *reply = NULL;
	int ok = !w.err && arpc_conn_exchange(g_conn, w.buf, w.len, &reply);
	ajw_free(&w);
	if (!ok || !reply)
		return dflt;

	aj_doc d;
	long long r = dflt;
	if (aj_parse(&d, reply, strlen(reply)))
		r = aj_i64(&d, aj_member(&d, 0, "ret"), dflt);
	aj_free(&d);
	free(reply);
	return r;
}

/* ---- trampolines ---- */

static void tr_log(void *ctx, alpm_loglevel_t level, const char *fmt,
		   va_list ap)
{
	uint64_t h = ctx_handle(ctx);
	cbreg *r = reg_for(h, 0);
	if (!r || !r->log)
		return;

	/* A va_list cannot cross the wire, so it is consumed here and the
	 * formatted text is what travels. The client hands its callback the
	 * result as a literal format string, which is what it would have
	 * produced anyway. */
	char buf[4096];
	vsnprintf(buf, sizeof(buf), fmt, ap);

	aj_w a;
	ajw_init(&a);
	ajw_obj_begin(&a);
	ajw_key(&a, "level"); ajw_i64(&a, (long long)level);
	ajw_key(&a, "msg");   ajw_str(&a, buf);
	ajw_obj_end(&a);
	cb_call("log", h, &a, 0);
	ajw_free(&a);
}

static void tr_progress(void *ctx, alpm_progress_t progress, const char *pkg,
			int percent, size_t howmany, size_t current)
{
	uint64_t h = ctx_handle(ctx);
	cbreg *r = reg_for(h, 0);
	if (!r || !r->progress)
		return;

	aj_w a;
	ajw_init(&a);
	ajw_obj_begin(&a);
	ajw_key(&a, "progress"); ajw_i64(&a, (long long)progress);
	ajw_key(&a, "pkg");      ajw_str(&a, pkg);
	ajw_key(&a, "percent");  ajw_i64(&a, percent);
	ajw_key(&a, "howmany");  ajw_i64(&a, (long long)howmany);
	ajw_key(&a, "current");  ajw_i64(&a, (long long)current);
	ajw_obj_end(&a);
	cb_call("progress", h, &a, 0);
	ajw_free(&a);
}

static void tr_event(void *ctx, alpm_event_t *e);
static void tr_question(void *ctx, alpm_question_t *q);

/* ---- arpc.set_callback ---- */

int arpc_cb_set(arpc_req *rq, arpc_res *rs)
{
	uint64_t hid = arpc_arg_id(rq, 0);
	alpm_handle_t *h = (alpm_handle_t *)arpc_arg_handle(rq, 0, ARPC_H_HANDLE);
	const char *which = arpc_arg_str(rq, 1);
	int enabled = (int)arpc_arg_i64(rq, 2);

	if (arpc_req_bad(rq) || !which)
		return arpc_fail(rs, ARPC_E_INVALID_PARAMS,
				 "arpc.set_callback: bad arguments");

	cbreg *r = reg_for(hid, 1);
	if (!r)
		return arpc_fail(rs, ARPC_E_INTERNAL, "out of memory");

	int rc = 0;
	if (!strcmp(which, "log")) {
		r->log = enabled;
		rc = alpm_option_set_logcb(h, enabled ? tr_log : NULL,
					   (void *)(uintptr_t)hid);
	} else if (!strcmp(which, "progress")) {
		r->progress = enabled;
		rc = alpm_option_set_progresscb(h, enabled ? tr_progress : NULL,
						(void *)(uintptr_t)hid);
	} else if (!strcmp(which, "event")) {
		r->event = enabled;
		rc = alpm_option_set_eventcb(h, enabled ? tr_event : NULL,
					     (void *)(uintptr_t)hid);
	} else if (!strcmp(which, "question")) {
		r->question = enabled;
		rc = alpm_option_set_questioncb(h, enabled ? tr_question : NULL,
						(void *)(uintptr_t)hid);
	} else {
		return arpc_fail(rs, ARPC_E_INVALID_PARAMS,
				 "arpc.set_callback: unknown callback");
	}

	arpc_ret_i64(rs, rc);
	return 0;
}

/* ---- payload helpers ----
 *
 * Written out by hand rather than reusing the generated marshallers: those
 * are static to the generated file, and exporting them to reach a handful of
 * fixed shapes would widen the client DLL's export surface for no benefit.
 * The round-trip tests are what keep these honest.
 *
 * A pointer inside an event or a question travels as a handle id like any
 * other, so the caller can hand it straight to alpm_pkg_get_*. NULL maps to
 * id 0 and back, which is what carries "no oldpkg" on an install.
 */

static void put_pkg_cb(aj_w *w, alpm_pkg_t *p, uint64_t owner)
{
	ajw_i64(w, (long long)arpc_handle_put(p, ARPC_H_PKG, owner));
}

static void put_depend_cb(aj_w *w, const alpm_depend_t *d)
{
	if (!d) {
		ajw_null(w);
		return;
	}
	ajw_obj_begin(w);
	ajw_key(w, "name");      ajw_str(w, d->name);
	ajw_key(w, "version");   ajw_str(w, d->version);
	ajw_key(w, "desc");      ajw_str(w, d->desc);
	ajw_key(w, "name_hash"); ajw_i64(w, (long long)d->name_hash);
	ajw_key(w, "mod");       ajw_i64(w, (long long)d->mod);
	ajw_obj_end(w);
}

static void put_pkglist_cb(aj_w *w, const alpm_list_t *l, uint64_t owner)
{
	if (!l) {
		ajw_null(w);
		return;
	}
	ajw_arr_begin(w);
	for (; l; l = l->next)
		put_pkg_cb(w, (alpm_pkg_t *)l->data, owner);
	ajw_arr_end(w);
}

static void put_conflict_cb(aj_w *w, const alpm_conflict_t *c, uint64_t owner)
{
	if (!c) {
		ajw_null(w);
		return;
	}
	ajw_obj_begin(w);
	ajw_key(w, "package1"); put_pkg_cb(w, c->package1, owner);
	ajw_key(w, "package2"); put_pkg_cb(w, c->package2, owner);
	ajw_key(w, "reason");
	put_depend_cb(w, c->reason);
	ajw_obj_end(w);
}

/* ---- events ----
 *
 * alpm_event_t is a union and the type alone selects the member, so getting
 * that mapping wrong means reading a pointer out of a member libalpm never
 * filled. Sending nothing extra costs a caller a field; reading the wrong
 * member costs the server. So a payload is carried only where the mapping is
 * established, and every other type carries just its type -- which for most
 * of them is all libalpm raises anyway.
 *
 * Where each mapping comes from:
 *
 *   package_operation, optdep_removal, scriptlet_info, database_missing,
 *   pacnew_created, pacsave_created   alpm.h names the struct for these
 *                                     types in its own doc comments.
 *   hook, hook_run, pkg_retrieve      not documented in the header, so they
 *                                     are taken from what pacman's own
 *                                     frontend reads for those types --
 *                                     libalpm must fill whatever the
 *                                     reference reader consumes.
 *
 * HOOK_DONE and HOOK_RUN_DONE carry the same payload as their START, because
 * libalpm's hook.c raises them by reusing the struct it already populated and
 * only reassigning .type. Its frontend happens to read them on START alone,
 * which is a display choice, not a limit on what is there.
 *
 * alpm_event_pkgdownload_t is in the union and is marshalled by nothing: no
 * event type in libalpm 14 selects it, the ALPM_EVENT_PKGDOWNLOAD_* types
 * having gone when downloads moved to the download callback. It is left
 * alone rather than guessed at a home for.
 *
 * An event has no answer -- alpm_cb_event returns void -- but it is still an
 * exchange, like log and progress. Making it one-way would buy a round trip
 * per event and cost the ordering guarantee: an event and a question raised
 * from the same operation have to reach the caller in the order libalpm
 * raised them, and one shared blocking path is what makes that true without
 * the frame loop having to know which is which.
 */

static void tr_event(void *ctx, alpm_event_t *e)
{
	uint64_t h = ctx_handle(ctx);
	cbreg *r = reg_for(h, 0);
	if (!r || !r->event || !e)
		return;

	aj_w a;
	ajw_init(&a);
	ajw_obj_begin(&a);
	ajw_key(&a, "type");
	ajw_i64(&a, (long long)e->type);

	switch (e->type) {
	case ALPM_EVENT_PACKAGE_OPERATION_START:
	case ALPM_EVENT_PACKAGE_OPERATION_DONE:
		ajw_key(&a, "operation");
		ajw_i64(&a, (long long)e->package_operation.operation);
		ajw_key(&a, "oldpkg");
		put_pkg_cb(&a, e->package_operation.oldpkg, h);
		ajw_key(&a, "newpkg");
		put_pkg_cb(&a, e->package_operation.newpkg, h);
		break;
	case ALPM_EVENT_OPTDEP_REMOVAL:
		ajw_key(&a, "pkg");
		put_pkg_cb(&a, e->optdep_removal.pkg, h);
		ajw_key(&a, "optdep");
		put_depend_cb(&a, e->optdep_removal.optdep);
		break;
	case ALPM_EVENT_SCRIPTLET_INFO:
		ajw_key(&a, "line"); ajw_str(&a, e->scriptlet_info.line);
		break;
	case ALPM_EVENT_DATABASE_MISSING:
		ajw_key(&a, "dbname"); ajw_str(&a, e->database_missing.dbname);
		break;
	case ALPM_EVENT_PACNEW_CREATED:
		ajw_key(&a, "from_noupgrade");
		ajw_i64(&a, e->pacnew_created.from_noupgrade);
		ajw_key(&a, "oldpkg");
		put_pkg_cb(&a, e->pacnew_created.oldpkg, h);
		ajw_key(&a, "newpkg");
		put_pkg_cb(&a, e->pacnew_created.newpkg, h);
		ajw_key(&a, "file"); ajw_str(&a, e->pacnew_created.file);
		break;
	case ALPM_EVENT_PACSAVE_CREATED:
		ajw_key(&a, "oldpkg");
		put_pkg_cb(&a, e->pacsave_created.oldpkg, h);
		ajw_key(&a, "file"); ajw_str(&a, e->pacsave_created.file);
		break;
	case ALPM_EVENT_HOOK_START:
	case ALPM_EVENT_HOOK_DONE:
		ajw_key(&a, "when"); ajw_i64(&a, (long long)e->hook.when);
		break;
	case ALPM_EVENT_HOOK_RUN_START:
	case ALPM_EVENT_HOOK_RUN_DONE:
		/* desc is genuinely optional -- a hook without a Description
		 * has none -- so it travels as null and arrives as NULL. */
		ajw_key(&a, "name"); ajw_str(&a, e->hook_run.name);
		ajw_key(&a, "desc"); ajw_str(&a, e->hook_run.desc);
		ajw_key(&a, "position");
		ajw_i64(&a, (long long)e->hook_run.position);
		ajw_key(&a, "total");
		ajw_i64(&a, (long long)e->hook_run.total);
		break;
	case ALPM_EVENT_PKG_RETRIEVE_START:
		ajw_key(&a, "num");
		ajw_i64(&a, (long long)e->pkg_retrieve.num);
		ajw_key(&a, "total_size");
		ajw_i64(&a, (long long)e->pkg_retrieve.total_size);
		break;
	default:
		break;          /* any: nothing beyond the type */
	}
	ajw_obj_end(&a);

	cb_call("event", h, &a, 0);
	ajw_free(&a);
}

/* ---- questions ----
 *
 * A question is the reason the callback transport has to block: libalpm is
 * part-way through a transaction and cannot continue until the caller says
 * whether to replace a package, remove a conflict, or trust a key.
 *
 * The variants all begin {alpm_question_type_t type; int <answer>; ...}, so
 * whatever the caller sets is readable through q->any.answer regardless of
 * which one arrived. That is the same aliasing libalpm relies on when it
 * documents `any` as always safe to access.
 */

static void tr_question(void *ctx, alpm_question_t *q)
{
	uint64_t h = ctx_handle(ctx);
	cbreg *r = reg_for(h, 0);
	if (!r || !r->question || !q)
		return;

	aj_w a;
	ajw_init(&a);
	ajw_obj_begin(&a);
	ajw_key(&a, "type");
	ajw_i64(&a, (long long)q->type);

	switch (q->type) {
	case ALPM_QUESTION_INSTALL_IGNOREPKG:
		ajw_key(&a, "pkg");
		ajw_i64(&a, (long long)arpc_handle_put(q->install_ignorepkg.pkg,
						       ARPC_H_PKG, h));
		break;
	case ALPM_QUESTION_REPLACE_PKG:
		ajw_key(&a, "oldpkg");
		ajw_i64(&a, (long long)arpc_handle_put(q->replace.oldpkg,
						       ARPC_H_PKG, h));
		ajw_key(&a, "newpkg");
		ajw_i64(&a, (long long)arpc_handle_put(q->replace.newpkg,
						       ARPC_H_PKG, h));
		ajw_key(&a, "newdb");
		ajw_i64(&a, (long long)arpc_handle_put(q->replace.newdb,
						       ARPC_H_DB, h));
		break;
	case ALPM_QUESTION_CONFLICT_PKG:
		ajw_key(&a, "conflict");
		put_conflict_cb(&a, q->conflict.conflict, h);
		break;
	case ALPM_QUESTION_CORRUPTED_PKG:
		ajw_key(&a, "filepath"); ajw_str(&a, q->corrupted.filepath);
		ajw_key(&a, "reason");
		ajw_i64(&a, (long long)q->corrupted.reason);
		break;
	case ALPM_QUESTION_REMOVE_PKGS:
		ajw_key(&a, "packages");
		put_pkglist_cb(&a, q->remove_pkgs.packages, h);
		break;
	case ALPM_QUESTION_SELECT_PROVIDER:
		ajw_key(&a, "providers");
		put_pkglist_cb(&a, q->select_provider.providers, h);
		ajw_key(&a, "depend");
		put_depend_cb(&a, q->select_provider.depend);
		break;
	case ALPM_QUESTION_IMPORT_KEY:
		ajw_key(&a, "uid");         ajw_str(&a, q->import_key.uid);
		ajw_key(&a, "fingerprint");
		ajw_str(&a, q->import_key.fingerprint);
		break;
	default:
		break;          /* any: nothing beyond the type */
	}
	ajw_obj_end(&a);

	/* The default on a failed exchange is the answer libalpm would get from
	 * a frontend that declined: say no rather than silently agreeing to
	 * replace or remove something. */
	long long answer = cb_call("question", h, &a, 0);
	ajw_free(&a);

	q->any.answer = (int)answer;
}
