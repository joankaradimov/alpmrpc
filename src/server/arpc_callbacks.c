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
	int log, progress;
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
	} else {
		return arpc_fail(rs, ARPC_E_INVALID_PARAMS,
				 "arpc.set_callback: unknown callback");
	}

	arpc_ret_i64(rs, rc);
	return 0;
}
