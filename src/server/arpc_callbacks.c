/* Server half of the callback bridge: the transport, and nothing else.
 *
 * libalpm calls back from inside a call the client is waiting on. The
 * trampolines it actually calls are generated -- a payload is a struct whose
 * fields the model has, and which member a tag selects is stated in the
 * overlay where it can be checked against the enum. What is left here is the
 * part that is about the pipe rather than about any particular callback:
 * which callbacks a client asked for, and sending one and waiting.
 *
 * That waiting is deliberate, not a limitation. A question has to be answered
 * before the transaction that asked it can continue, so the reply has to
 * arrive before the trampoline returns. Doing it on the same thread and the
 * same connection is what keeps that ordering honest -- and keeps the server
 * single-threaded, which is what keeps libalpm's fork() on the path Cygwin
 * supports.
 */
#include "arpc_server.h"

#include <alpm.h>

#include <stdlib.h>
#include <string.h>

/* The connection currently being served. One at a time, so a global is the
 * honest representation rather than a shortcut. */
static arpc_conn *g_conn;
static long long g_seq = 1;

void arpc_cb_set_conn(arpc_conn *c) { g_conn = c; }

/* Which callbacks the client has asked for, per handle. libalpm always gets
 * our trampoline; this says whether to bother sending anything. One bit per
 * kind, which is why the generator refuses to emit more than 32. */
typedef struct cbreg {
	struct cbreg *next;
	uint64_t handle;
	uint32_t enabled;
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

int arpc_cb_wanted(uint64_t handle, int kind)
{
	cbreg *r = reg_for(handle, 0);
	return r && (r->enabled & (1u << kind));
}

/* Send one callback frame and wait for the answer. Returns the reply's "ret"
 * or `dflt` if the connection failed -- a dead pipe must not wedge libalpm
 * mid-transaction. */
long long arpc_cb_send(int kind, uint64_t handle, aj_w *args, long long dflt)
{
	if (!g_conn)
		return dflt;

	long long seq = g_seq++;

	aj_w w;
	ajw_init(&w);
	ajw_obj_begin(&w);
	ajw_key(&w, "cb");     ajw_str(&w, arpc_cb_kinds[kind].name);
	ajw_key(&w, "seq");    ajw_i64(&w, seq);
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
	if (aj_parse(&d, reply, strlen(reply))) {
		/* Callbacks nest: one can ask libalpm something, and that can
		 * raise another. So the answer is matched to the question
		 * rather than assumed to be the next thing along -- a
		 * mismatch means the pipe is a frame out of step, and taking
		 * the number anyway would answer one question with another's
		 * reply. */
		if (aj_i64(&d, aj_member(&d, 0, "cbseq"), -1) == seq)
			r = aj_i64(&d, aj_member(&d, 0, "ret"), dflt);
	}
	aj_free(&d);
	free(reply);
	return r;
}

/* ---- arpc.set_callback ---- */

int arpc_cb_set(arpc_req *rq, arpc_res *rs)
{
	uint64_t hid = arpc_arg_id(rq, 0);
	alpm_handle_t *h = (alpm_handle_t *)arpc_arg_handle(rq, 0,
							   ARPC_H_HANDLE);
	const char *which = arpc_arg_str(rq, 1);
	int enabled = (int)arpc_arg_i64(rq, 2);

	if (arpc_req_bad(rq) || !which)
		return arpc_fail(rs, ARPC_E_INVALID_PARAMS,
				 "arpc.set_callback: bad arguments");

	int kind = -1;
	for (int i = 0; arpc_cb_kinds[i].name; i++)
		if (!strcmp(arpc_cb_kinds[i].name, which))
			kind = i;
	if (kind < 0)
		return arpc_fail(rs, ARPC_E_INVALID_PARAMS,
				 "arpc.set_callback: unknown callback");

	cbreg *r = reg_for(hid, 1);
	if (!r)
		return arpc_fail(rs, ARPC_E_INTERNAL, "out of memory");

	if (enabled)
		r->enabled |= (1u << kind);
	else
		r->enabled &= ~(1u << kind);

	/* The handle id travels as libalpm's ctx pointer, since that is the
	 * one thing libalpm hands back to a trampoline untouched. */
	arpc_ret_i64(rs, arpc_cb_kinds[kind].install(h, enabled,
						     (void *)(uintptr_t)hid));
	return 0;
}
