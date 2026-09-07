/* Client half of the callback bridge.
 *
 * libalpm runs on the server, but the callbacks a caller registers are
 * function pointers in *this* process. A pointer cannot cross the wire, so
 * the server installs its own trampoline with libalpm and calls back up the
 * pipe when it fires; this file holds the real pointers and dispatches to
 * them.
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

/* One registration per handle. libalpm keeps callbacks per alpm_handle_t, so
 * this mirrors that rather than keeping one global set. */
typedef struct reg {
	struct reg *next;
	uint64_t handle;
	alpm_cb_log log;          void *log_ctx;
	alpm_cb_progress progress; void *progress_ctx;
	alpm_cb_event event;      void *event_ctx;
	alpm_cb_question question; void *question_ctx;
	alpm_cb_download dl;      void *dl_ctx;
	alpm_cb_fetch fetch;      void *fetch_ctx;
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

/* ---- the setters and getters ---- */

#define SETTER(fn, field, type, wire)                                        \
	int fn(alpm_handle_t *handle, type cb, void *ctx)                    \
	{                                                                    \
		reg *r = reg_for(ARPC_ID(handle), 1);                        \
		if (!r)                                                      \
			return -1;                                           \
		r->field = cb;                                               \
		r->field##_ctx = ctx;                                        \
		return set_remote(ARPC_ID(handle), wire, cb != NULL);        \
	}

#define GETTER(fn, field, type)                                              \
	type fn(alpm_handle_t *handle)                                       \
	{                                                                    \
		reg *r = reg_for(ARPC_ID(handle), 0);                        \
		return r ? r->field : NULL;                                  \
	}

SETTER(alpm_option_set_logcb, log, alpm_cb_log, "log")
GETTER(alpm_option_get_logcb, log, alpm_cb_log)

SETTER(alpm_option_set_progresscb, progress, alpm_cb_progress, "progress")
GETTER(alpm_option_get_progresscb, progress, alpm_cb_progress)

/* Not yet marshalled. The pointer is stored so the getter round-trips, but
 * the server is told not to install a trampoline -- and the setter reports
 * failure, because a callback that is registered and then silently never
 * fires is a worse outcome than one that refuses up front. */
#define UNIMPLEMENTED_SETTER(fn, field, type)                                \
	int fn(alpm_handle_t *handle, type cb, void *ctx)                    \
	{                                                                    \
		reg *r = reg_for(ARPC_ID(handle), 1);                        \
		if (r) {                                                     \
			r->field = cb;                                       \
			r->field##_ctx = ctx;                                \
		}                                                            \
		return -1;                                                   \
	}

UNIMPLEMENTED_SETTER(alpm_option_set_eventcb, event, alpm_cb_event)
GETTER(alpm_option_get_eventcb, event, alpm_cb_event)

UNIMPLEMENTED_SETTER(alpm_option_set_questioncb, question, alpm_cb_question)
GETTER(alpm_option_get_questioncb, question, alpm_cb_question)

UNIMPLEMENTED_SETTER(alpm_option_set_dlcb, dl, alpm_cb_download)
GETTER(alpm_option_get_dlcb, dl, alpm_cb_download)

UNIMPLEMENTED_SETTER(alpm_option_set_fetchcb, fetch, alpm_cb_fetch)
GETTER(alpm_option_get_fetchcb, fetch, alpm_cb_fetch)

/* ---- dispatch ---- */

/* alpm_cb_log wants a va_list, and there is no portable way to build one
 * except by being variadic. The server already did the formatting, so this
 * passes it straight through as the whole format string. */
static void log_trampoline(alpm_cb_log cb, void *ctx, alpm_loglevel_t lvl,
			   const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	cb(ctx, lvl, fmt, ap);
	va_end(ap);
}

/* The server formatted the message already: alpm_cb_log takes a va_list, and
 * a va_list cannot be marshalled. Handing the callback a pre-formatted "%s"
 * gives it the text it would have produced anyway. */
static void call_log(reg *r, const aj_doc *d, int args)
{
	if (!r || !r->log)
		return;
	alpm_loglevel_t lvl =
		(alpm_loglevel_t)aj_i64(d, aj_member(d, args, "level"), 0);
	const char *msg = aj_str(d, aj_member(d, args, "msg"), "");
	log_trampoline(r->log, r->log_ctx, lvl, "%s", msg);
}

static void call_progress(reg *r, const aj_doc *d, int args)
{
	if (!r || !r->progress)
		return;
	r->progress(r->progress_ctx,
		    (alpm_progress_t)aj_i64(d, aj_member(d, args, "progress"), 0),
		    aj_str(d, aj_member(d, args, "pkg"), ""),
		    (int)aj_i64(d, aj_member(d, args, "percent"), 0),
		    (size_t)aj_i64(d, aj_member(d, args, "howmany"), 0),
		    (size_t)aj_i64(d, aj_member(d, args, "current"), 0));
}

void arpc_dispatch_callback(const aj_doc *d, aj_w *reply)
{
	const char *which = aj_str(d, aj_member(d, 0, "cb"), "");
	long long seq = aj_i64(d, aj_member(d, 0, "seq"), 0);
	uint64_t handle = (uint64_t)aj_i64(d, aj_member(d, 0, "handle"), 0);
	int args = aj_member(d, 0, "args");

	reg *r = reg_for(handle, 0);

	if (!strcmp(which, "log"))
		call_log(r, d, args);
	else if (!strcmp(which, "progress"))
		call_progress(r, d, args);
	/* An unknown callback is answered rather than ignored: the server is
	 * blocked waiting, and a silent drop would deadlock the transaction. */

	ajw_obj_begin(reply);
	ajw_key(reply, "cbseq");
	ajw_i64(reply, seq);
	ajw_key(reply, "ret");
	ajw_i64(reply, 0);
	ajw_obj_end(reply);
}
