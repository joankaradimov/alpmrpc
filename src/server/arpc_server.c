#include "arpc_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- handle table */

typedef struct {
	uint64_t id;            /* 0 = empty slot */
	void *ptr;
	uint64_t owner;
	arpc_handle_tag tag;
} slot;

static slot *g_tab;
static size_t g_cap, g_used;
static uint64_t g_next_id = 1;

static size_t slot_for(slot *tab, size_t cap, uint64_t id)
{
	/* Fibonacci hashing; ids are dense and monotonic so the low bits alone
	 * would cluster badly. */
	size_t mask = cap - 1;
	size_t i = (size_t)((id * 11400714819323198485ULL) >> 32) & mask;
	while (tab[i].id && tab[i].id != id)
		i = (i + 1) & mask;
	return i;
}

static int grow(void)
{
	size_t ncap = g_cap ? g_cap * 2 : 256;
	slot *nt = (slot *)calloc(ncap, sizeof(*nt));
	if (!nt)
		return 0;
	for (size_t i = 0; i < g_cap; i++)
		if (g_tab[i].id)
			nt[slot_for(nt, ncap, g_tab[i].id)] = g_tab[i];
	free(g_tab);
	g_tab = nt;
	g_cap = ncap;
	return 1;
}

uint64_t arpc_handle_put(void *ptr, arpc_handle_tag tag, uint64_t owner)
{
	if (!ptr)
		return 0;                       /* NULL stays NULL across the wire */
	if ((g_used + 1) * 4 >= g_cap * 3 && !grow())
		return 0;

	uint64_t id = g_next_id++;
	size_t i = slot_for(g_tab, g_cap, id);
	g_tab[i].id = id;
	g_tab[i].ptr = ptr;
	g_tab[i].tag = tag;
	g_tab[i].owner = owner;
	g_used++;
	return id;
}

static slot *find(uint64_t id)
{
	if (!id || !g_cap)
		return NULL;
	size_t i = slot_for(g_tab, g_cap, id);
	return g_tab[i].id == id ? &g_tab[i] : NULL;
}

void *arpc_handle_get(uint64_t id, arpc_handle_tag tag)
{
	slot *s = find(id);
	/* A db id passed where a pkg id belongs is a protocol error, not a
	 * cast -- refuse it rather than handing libalpm the wrong pointer. */
	return (s && s->tag == tag) ? s->ptr : NULL;
}

uint64_t arpc_owner_of(uint64_t id)
{
	slot *s = find(id);
	return s ? s->owner : 0;
}

/* Open addressing cannot simply blank a slot: that would cut probe chains.
 * Rebuilding is O(cap) but only happens on release, which is rare. */
static void rebuild_without(int (*drop)(const slot *, void *), void *ctx)
{
	slot *nt = (slot *)calloc(g_cap, sizeof(*nt));
	if (!nt)
		return;
	size_t used = 0;
	for (size_t i = 0; i < g_cap; i++) {
		if (!g_tab[i].id || drop(&g_tab[i], ctx))
			continue;
		nt[slot_for(nt, g_cap, g_tab[i].id)] = g_tab[i];
		used++;
	}
	free(g_tab);
	g_tab = nt;
	g_used = used;
}

static int drop_one(const slot *s, void *ctx) { return s->id == *(uint64_t *)ctx; }
static int drop_owned(const slot *s, void *ctx)
{
	uint64_t o = *(uint64_t *)ctx;
	return s->id == o || s->owner == o;
}

void arpc_handle_drop(uint64_t id)
{
	if (find(id))
		rebuild_without(drop_one, &id);
}

void arpc_handle_drop_owner(uint64_t owner)
{
	if (owner)
		rebuild_without(drop_owned, &owner);
}

void arpc_handle_reset(void)
{
	free(g_tab);
	g_tab = NULL;
	g_cap = g_used = 0;
}

unsigned arpc_handle_live(void) { return (unsigned)g_used; }

/* --------------------------------------------------------------- request */

static int arg_node(arpc_req *rq, int i)
{
	int n = aj_elem(rq->doc, rq->params, i);
	if (n < 0)
		rq->bad = 1;
	return n;
}

const char *arpc_arg_str(arpc_req *rq, int i)
{
	int n = arg_node(rq, i);
	if (n < 0)
		return NULL;
	if (aj_is_null(rq->doc, n))
		return NULL;            /* explicit null is a legitimate NULL char* */
	const char *s = aj_str(rq->doc, n, NULL);
	if (!s)
		rq->bad = 1;
	return s;
}

long long arpc_arg_i64(arpc_req *rq, int i)
{
	int n = arg_node(rq, i);
	if (n < 0)
		return 0;
	const aj_node *nd = &rq->doc->nodes[n];
	if (nd->type != AJ_NUM && nd->type != AJ_BOOL) {
		rq->bad = 1;
		return 0;
	}
	return nd->num;
}

uint64_t arpc_arg_id(arpc_req *rq, int i)
{
	int n = arg_node(rq, i);
	if (n < 0)
		return 0;
	if (aj_is_null(rq->doc, n))
		return 0;
	return (uint64_t)aj_i64(rq->doc, n, 0);
}

void *arpc_arg_handle(arpc_req *rq, int i, arpc_handle_tag tag)
{
	uint64_t id = arpc_arg_id(rq, i);
	if (!id)
		return NULL;            /* NULL handle is valid input to libalpm */
	void *p = arpc_handle_get(id, tag);
	if (!p)
		rq->bad = 1;            /* non-zero id that resolves to nothing */
	return p;
}

int arpc_req_bad(const arpc_req *rq) { return rq->bad; }
void arpc_req_mark_bad(arpc_req *rq) { rq->bad = 1; }

/* ---- raw node access, used by generated list code ---- */

int arpc_arg_node(arpc_req *rq, int i)
{
	return arg_node(rq, i);
}

int arpc_node_is_null(const arpc_req *rq, int n)
{
	return aj_is_null(rq->doc, n);
}

int arpc_node_count(const arpc_req *rq, int n)
{
	return aj_count(rq->doc, n);
}

int arpc_node_elem(const arpc_req *rq, int arr, int k)
{
	return aj_elem(rq->doc, arr, k);
}

int arpc_node_member(const arpc_req *rq, int obj, const char *key)
{
	return aj_member(rq->doc, obj, key);
}

long long arpc_node_i64(const arpc_req *rq, int n)
{
	return aj_i64(rq->doc, n, 0);
}

char *arpc_node_strdup(const arpc_req *rq, int n)
{
	const char *s = aj_str(rq->doc, n, NULL);
	if (!s)
		return NULL;
	size_t len = strlen(s) + 1;
	char *out = (char *)malloc(len);
	if (out)
		memcpy(out, s, len);
	return out;
}

/* -------------------------------------------------------------- response */

static void ensure_obj(arpc_res *rs)
{
	if (!rs->out.buf) {
		ajw_init(&rs->out);
		ajw_obj_begin(&rs->out);
	}
}

static void set_ret(arpc_res *rs)
{
	ensure_obj(rs);
	ajw_key(&rs->out, "ret");
	rs->has_ret = 1;
}

void arpc_ret_null(arpc_res *rs)              { set_ret(rs); ajw_null(&rs->out); }
void arpc_ret_i64(arpc_res *rs, long long v)  { set_ret(rs); ajw_i64(&rs->out, v); }
void arpc_ret_str(arpc_res *rs, const char *s){ set_ret(rs); ajw_str(&rs->out, s); }

void arpc_ret_handle(arpc_res *rs, uint64_t id)
{
	set_ret(rs);
	ajw_i64(&rs->out, (long long)id);
}

void arpc_ret_begin(arpc_res *rs)
{
	set_ret(rs);
}

aj_w *arpc_res_writer(arpc_res *rs)
{
	ensure_obj(rs);
	return &rs->out;
}

void arpc_out_i64(arpc_res *rs, const char *name, long long v)
{
	ensure_obj(rs);
	if (!rs->has_ret)
		arpc_ret_null(rs);      /* keep "ret" first for readable traces */
	ajw_key(&rs->out, name);
	ajw_i64(&rs->out, v);
}

int arpc_fail(arpc_res *rs, int code, const char *msg)
{
	rs->failed = 1;
	rs->code = code;
	snprintf(rs->msg, sizeof(rs->msg), "%s", msg ? msg : "error");
	return -1;
}

/* -------------------------------------------------------------- dispatch */

static char *finish(aj_w *w)
{
	char *s = w->buf;
	if (w->err) {
		free(s);
		return NULL;
	}
	w->buf = NULL;
	ajw_free(w);
	return s;
}

static char *reply_error(long long id, int code, const char *msg)
{
	aj_w w;
	ajw_init(&w);
	ajw_obj_begin(&w);
	ajw_key(&w, "id");    ajw_i64(&w, id);
	ajw_key(&w, "error"); ajw_obj_begin(&w);
	ajw_key(&w, "code");  ajw_i64(&w, code);
	ajw_key(&w, "message"); ajw_str(&w, msg);
	ajw_obj_end(&w);
	ajw_obj_end(&w);
	return finish(&w);
}

static int g_shutdown_requested;

int arpc_shutdown_requested(void) { return g_shutdown_requested; }

char *arpc_handle_frame(const char *req, size_t len)
{
	aj_doc d;
	if (!aj_parse(&d, req, len)) {
		aj_free(&d);
		return reply_error(0, ARPC_E_PARSE, "malformed JSON");
	}

	long long id = aj_i64(&d, aj_member(&d, 0, "id"), 0);
	const char *method = aj_str(&d, aj_member(&d, 0, "method"), NULL);
	int params = aj_member(&d, 0, "params");

	if (!method) {
		aj_free(&d);
		return reply_error(id, ARPC_E_INVALID_REQ, "missing method");
	}

	/* Infrastructure, not a libalpm call, so it lives here rather than in
	 * the generated table. Exiting after the current client disconnects is
	 * what the idle timer would do anyway -- this only brings it forward,
	 * which is what lets a rebuild replace the binary. */
	if (!strcmp(method, "arpc.shutdown")) {
		g_shutdown_requested = 1;
		aj_free(&d);
		aj_w w;
		ajw_init(&w);
		ajw_obj_begin(&w);
		ajw_key(&w, "id");     ajw_i64(&w, id);
		ajw_key(&w, "result"); ajw_obj_begin(&w);
		ajw_key(&w, "ret");    ajw_i64(&w, 0);
		ajw_obj_end(&w);
		ajw_obj_end(&w);
		return finish(&w);
	}

	const arpc_method *m = NULL;
	for (const arpc_method *k = arpc_methods; k->name; k++) {
		if (!strcmp(k->name, method)) {
			m = k;
			break;
		}
	}
	if (!m) {
		char buf[256];
		snprintf(buf, sizeof(buf), "no such method: %s", method);
		aj_free(&d);
		return reply_error(id, ARPC_E_NO_METHOD, buf);
	}

	arpc_req rq = { &d, params, 0 };
	arpc_res rs;
	memset(&rs, 0, sizeof(rs));

	m->fn(&rq, &rs);

	char *out;
	if (rs.failed) {
		out = reply_error(id, rs.code, rs.msg);
	} else {
		ensure_obj(&rs);
		if (!rs.has_ret)
			arpc_ret_null(&rs);
		ajw_obj_end(&rs.out);

		aj_w w;
		ajw_init(&w);
		ajw_obj_begin(&w);
		ajw_key(&w, "id");     ajw_i64(&w, id);
		/* Splice the handler's object in verbatim. It is already valid
		 * JSON, so it needs no re-encoding -- and for a large result
		 * this is the difference between one memcpy and a bounds check
		 * per byte. */
		ajw_reserve(&w, rs.out.len + 2);
		ajw_key(&w, "result");
		ajw_raw(&w, rs.out.buf, rs.out.len);
		w.need_comma = 1;
		ajw_obj_end(&w);
		out = finish(&w);
	}

	ajw_free(&rs.out);
	aj_free(&d);
	return out ? out : reply_error(id, ARPC_E_INTERNAL, "out of memory");
}
