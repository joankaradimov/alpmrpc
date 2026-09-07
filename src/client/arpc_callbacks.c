/* Client half of the callback bridge: the transport, and nothing else.
 *
 * libalpm runs on the server, but the callbacks a caller registers are
 * function pointers in *this* process. A pointer cannot cross the wire, so
 * the server installs its own trampoline with libalpm and calls back up the
 * pipe when it fires. The setters, the getters and the per-callback
 * dispatchers are generated; what is left here is the registry those
 * pointers live in, telling the server whether to install a trampoline, and
 * routing an arriving frame to the right dispatcher.
 *
 * Callbacks arrive while a call is outstanding -- that is the whole point,
 * since a question has to be answered before the commit that asked it can
 * continue -- so the dispatch happens inside arpc_invoke's frame loop rather
 * than on a thread of its own. That keeps the server single-threaded, which
 * is what keeps libalpm's fork() safe under Cygwin.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "arpc_client.h"

#include <alpm.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* One registration per handle, because libalpm keeps callbacks per
 * alpm_handle_t. Fixed width rather than generated: the emitter refuses to
 * emit more kinds than this holds. */
#define ARPC_CB_SLOTS 32

typedef struct reg {
	struct reg *next;
	uint64_t handle;
	void (*fn[ARPC_CB_SLOTS])(void);
	void *ctx[ARPC_CB_SLOTS];
} reg;

static reg *g_regs;

static reg *reg_for(uint64_t handle, int create)
{
	for (reg *r = g_regs; r; r = r->next)
		if (r->handle == handle)
			return r;
	if (!create)
		return NULL;
	reg *r = (reg *)calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->handle = handle;
	r->next = g_regs;
	g_regs = r;
	return r;
}

void arpc_callbacks_purge(uint64_t handle)
{
	reg **pp = &g_regs;
	while (*pp) {
		if ((*pp)->handle == handle) {
			reg *dead = *pp;
			*pp = dead->next;
			free(dead);
		} else {
			pp = &(*pp)->next;
		}
	}
}

/* Tell the server whether to install its trampoline for this callback. */
static int set_remote(uint64_t handle, const char *which, int enabled)
{
	arpc_call c;
	if (!arpc_begin(&c, "arpc.set_callback"))
		return -1;
	arpc_put_handle(&c, handle);
	arpc_put_str(&c, which);
	arpc_put_i64(&c, enabled);
	if (!arpc_invoke(&c)) {
		arpc_end(&c);
		return -1;
	}
	int r = (int)arpc_ret_i64(&c);
	arpc_end(&c);
	return r;
}

int arpc_cb_register(uint64_t handle, int kind, const char *wire,
		     void (*fn)(void), void *ctx)
{
	reg *r = reg_for(handle, 1);
	if (!r || kind < 0 || kind >= ARPC_CB_SLOTS)
		return -1;
	r->fn[kind] = fn;
	r->ctx[kind] = ctx;
	return set_remote(handle, wire, fn != NULL);
}

void (*arpc_cb_fn(uint64_t handle, int kind))(void)
{
	reg *r = reg_for(handle, 0);
	return (r && kind >= 0 && kind < ARPC_CB_SLOTS) ? r->fn[kind] : NULL;
}

void *arpc_cb_ctx(uint64_t handle, int kind)
{
	reg *r = reg_for(handle, 0);
	return (r && kind >= 0 && kind < ARPC_CB_SLOTS) ? r->ctx[kind] : NULL;
}

/* alpm_cb_log wants a va_list, and there is no portable way to build one
 * except by being variadic. The server already did the formatting, so this
 * passes the result straight through as the whole format string -- the text
 * it would have produced anyway. */
static void log_shim(alpm_cb_log cb, void *ctx, alpm_loglevel_t lvl,
		     const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	cb(ctx, lvl, fmt, ap);
	va_end(ap);
}

void arpc_cb_log_via(void (*fn)(void), void *ctx, int level, const char *msg)
{
	log_shim((alpm_cb_log)fn, ctx, (alpm_loglevel_t)level, "%s",
		 msg ? msg : "");
}

/* ---- dispatch ---- */

void arpc_dispatch_callback(const aj_doc *d, aj_w *reply)
{
	const char *which = aj_str(d, aj_member(d, 0, "cb"), "");
	long long seq = aj_i64(d, aj_member(d, 0, "seq"), 0);
	uint64_t handle = (uint64_t)aj_i64(d, aj_member(d, 0, "handle"), 0);
	int args = aj_member(d, 0, "args");

	long long ret = 0;
	for (int i = 0; arpc_cb_kinds[i].name; i++) {
		if (strcmp(arpc_cb_kinds[i].name, which))
			continue;
		void (*fn)(void) = arpc_cb_fn(handle, i);
		if (fn)
			ret = arpc_cb_kinds[i].call(fn, arpc_cb_ctx(handle, i),
						    d, args);
		break;
	}
	/* An unknown callback is answered rather than ignored: the server is
	 * blocked waiting, and a silent drop would deadlock the transaction. */

	ajw_obj_begin(reply);
	ajw_key(reply, "cbseq");
	ajw_i64(reply, seq);
	ajw_key(reply, "ret");
	ajw_i64(reply, ret);
	ajw_obj_end(reply);
}
