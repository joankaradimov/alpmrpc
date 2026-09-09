#include "arpc_server.h"
#include "arpc_b64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- handle table
 *
 * Two indexes over one set of slots: by id, which is how the wire refers to
 * an object, and by (pointer, tag), so that the same libalpm object always
 * gets the same id. The second is what keeps pointer identity across the
 * wire -- the package alpm_db_get_pkg() returns is the one in the pkgcache
 * list, and callers compare them -- and it is also what stops the table
 * growing by a slot per accessor call.
 *
 * Objects form a tree: a package belongs to its db, a db to its handle, a
 * changelog cursor to its package. Dropping a node drops what is under it.
 * The root of that tree travels in the id's high bits (ARPC_ROOT), so the
 * client can tell which handle any id belongs to without being told.
 */

typedef struct {
	uint64_t id;            /* 0 = empty slot */
	void *ptr;
	uint64_t owner;
	arpc_handle_tag tag;
	void (*release)(void *); /* adopted: freed when the slot is dropped */
} slot;

static slot *g_tab;             /* open-addressed, by id */
static size_t *g_byptr;         /* open-addressed, by (ptr, tag): slot + 1 */
static size_t g_cap, g_used;
static uint64_t g_next_seq = 1;
static uint64_t g_next_root = 1;

static size_t mix(uint64_t x, size_t mask)
{
	/* Fibonacci hashing; ids are dense and monotonic so the low bits alone
	 * would cluster badly. */
	return (size_t)((x * 11400714819323198485ULL) >> 32) & mask;
}

static size_t slot_for(slot *tab, size_t cap, uint64_t id)
{
	size_t mask = cap - 1;
	size_t i = mix(id, mask);
	while (tab[i].id && tab[i].id != id)
		i = (i + 1) & mask;
	return i;
}

/* The g_byptr entry for (ptr, tag): the one pointing at its slot, or the
 * empty one where it would go. */
static size_t ptr_for(slot *tab, size_t *byptr, size_t cap, const void *p,
		      arpc_handle_tag tag)
{
	size_t mask = cap - 1;
	size_t i = mix((uint64_t)(uintptr_t)p ^ ((uint64_t)tag << 56), mask);
	while (byptr[i]) {
		const slot *s = &tab[byptr[i] - 1];
		if (s->ptr == p && s->tag == tag)
			break;
		i = (i + 1) & mask;
	}
	return i;
}

static void index_ptr(slot *tab, size_t *byptr, size_t cap, size_t si)
{
	byptr[ptr_for(tab, byptr, cap, tab[si].ptr, tab[si].tag)] = si + 1;
}

/* Rebuild both indexes at `ncap`, keeping the slots `keep` accepts, or all
 * of them when it is NULL. Open addressing cannot simply blank a slot --
 * that would cut probe chains -- so a drop is a rebuild. It is O(cap), and
 * happens when something is released, which is rare next to a lookup. */
static int rebuild(size_t ncap, int (*keep)(const slot *, void *), void *ctx)
{
	slot *nt = (slot *)calloc(ncap, sizeof(*nt));
	size_t *np = (size_t *)calloc(ncap, sizeof(*np));
	if (!nt || !np) {
		free(nt);
		free(np);
		return 0;
	}
	size_t used = 0;
	for (size_t i = 0; i < g_cap; i++) {
		if (!g_tab[i].id)
			continue;
		if (keep && !keep(&g_tab[i], ctx)) {
			/* An adopted object is freed here and nowhere else.
			 * What it frees in turn -- a conflict's copies of its
			 * packages -- has slots of its own that this same
			 * pass drops, and nothing below reads through a slot's
			 * pointer, so the order does not matter. */
			if (g_tab[i].release)
				g_tab[i].release(g_tab[i].ptr);
			continue;
		}
		size_t si = slot_for(nt, ncap, g_tab[i].id);
		nt[si] = g_tab[i];
		index_ptr(nt, np, ncap, si);
		used++;
	}
	free(g_tab);
	free(g_byptr);
	g_tab = nt;
	g_byptr = np;
	g_cap = ncap;
	g_used = used;
	return 1;
}

static slot *find(uint64_t id)
{
	if (!id || !g_cap)
		return NULL;
	size_t i = slot_for(g_tab, g_cap, id);
	return g_tab[i].id == id ? &g_tab[i] : NULL;
}

static slot *find_ptr(const void *p, arpc_handle_tag tag)
{
	if (!p || !g_cap)
		return NULL;
	size_t i = ptr_for(g_tab, g_byptr, g_cap, p, tag);
	return g_byptr[i] ? &g_tab[g_byptr[i] - 1] : NULL;
}

/* Is `id` `o` itself, or somewhere below it? Chains are short -- handle,
 * db, package, cursor -- and the bound only guards against a cycle that
 * cannot happen. */
static int under(uint64_t id, uint64_t o)
{
	for (int depth = 0; id && depth < 8; depth++) {
		if (id == o)
			return 1;
		slot *s = find(id);
		id = s ? s->owner : 0;
	}
	return 0;
}

uint64_t arpc_handle_put(void *ptr, arpc_handle_tag tag, uint64_t owner)
{
	if (!ptr)
		return 0;                       /* NULL stays NULL across the wire */

	slot *have = find_ptr(ptr, tag);
	if (have) {
		/* Seen before: same object, same id. If it was first filed
		 * under an ancestor of this owner -- a package met in an event
		 * before its db was ever listed -- re-file it under the closer
		 * one, so that dropping the db drops it too. */
		if (owner && owner != have->owner && under(owner, have->owner))
			have->owner = owner;
		return have->id;
	}

	if ((g_used + 1) * 4 >= g_cap * 3 &&
	    !rebuild(g_cap ? g_cap * 2 : 256, NULL, NULL))
		return 0;

	uint64_t root;
	if (owner)
		root = ARPC_ROOT(owner);
	else if (tag == ARPC_H_HANDLE)
		root = g_next_root++;
	else
		root = 0;                       /* nobody's; lives until exit */
	uint64_t id = (root << ARPC_ROOT_SHIFT) | g_next_seq++;

	size_t i = slot_for(g_tab, g_cap, id);
	g_tab[i].id = id;
	g_tab[i].ptr = ptr;
	g_tab[i].tag = tag;
	g_tab[i].owner = owner;
	g_tab[i].release = NULL;
	index_ptr(g_tab, g_byptr, g_cap, i);
	g_used++;
	return id;
}

uint64_t arpc_handle_adopt(void *ptr, uint64_t owner, void (*release)(void *))
{
	uint64_t id = arpc_handle_put(ptr, ARPC_H_NONE, owner);
	slot *s = find(id);
	if (s)
		s->release = release;
	return id;
}

void *arpc_handle_get(uint64_t id, arpc_handle_tag tag)
{
	slot *s = find(id);
	/* A db id passed where a pkg id belongs is a protocol error, not a
	 * cast -- refuse it rather than handing libalpm the wrong pointer. */
	return (s && s->tag == tag) ? s->ptr : NULL;
}

uint64_t arpc_ancestor(uint64_t id, arpc_handle_tag tag)
{
	uint64_t top = id;
	for (int depth = 0; id && depth < 8; depth++) {
		slot *s = find(id);
		if (!s)
			break;
		if (tag != ARPC_H_NONE && s->tag == tag)
			return id;
		top = id;
		id = s->owner;
	}
	return top;
}

static int keep_not(const slot *s, void *ctx)
{
	return s->id != *(uint64_t *)ctx;
}

static int keep_outside(const slot *s, void *ctx)
{
	return !under(s->id, *(uint64_t *)ctx);
}

static int keep_outside_or_self(const slot *s, void *ctx)
{
	uint64_t o = *(uint64_t *)ctx;
	return s->id == o || !under(s->id, o);
}

static void drop_matching(int (*keep)(const slot *, void *), void *ctx)
{
	if (g_cap)
		rebuild(g_cap, keep, ctx);
}

void arpc_handle_drop(uint64_t id)
{
	if (find(id))
		drop_matching(keep_not, &id);
}

void arpc_handle_drop_owner(uint64_t owner)
{
	if (owner) {
		drop_matching(keep_outside, &owner);
		arpc_cb_purge(owner);
	}
}

void arpc_handle_drop_under(uint64_t owner)
{
	if (owner)
		drop_matching(keep_outside_or_self, &owner);
}

void arpc_handle_drop_ptr(void *ptr, arpc_handle_tag tag)
{
	slot *s = find_ptr(ptr, tag);
	if (s)
		arpc_handle_drop_owner(s->id);
}

void arpc_handle_reset(void)
{
	for (size_t i = 0; i < g_cap; i++)
		if (g_tab[i].id && g_tab[i].release)
			g_tab[i].release(g_tab[i].ptr);
	free(g_tab);
	free(g_byptr);
	g_tab = NULL;
	g_byptr = NULL;
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

/* The first element of a list argument, as an id. Read without marking the
 * request bad: an empty or absent list is a legitimate argument, and this
 * is only asked after the arguments have been checked. */
uint64_t arpc_list_owner(arpc_req *rq, int i)
{
	int arr = aj_elem(rq->doc, rq->params, i);
	int e = aj_first(rq->doc, arr);
	return e >= 0 ? (uint64_t)aj_i64(rq->doc, e, 0) : 0;
}

/* ---- raw node access, used by generated list code ---- */

int arpc_arg_node(arpc_req *rq, int i)
{
	return arg_node(rq, i);
}

int arpc_node_is_null(const arpc_req *rq, int n)
{
	return aj_is_null(rq->doc, n);
}

int arpc_node_first(const arpc_req *rq, int arr)
{
	return aj_first(rq->doc, arr);
}

int arpc_node_next(const arpc_req *rq, int node)
{
	return aj_next(rq->doc, node);
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
void arpc_ret_path(arpc_res *rs, const char *s)
{
	set_ret(rs);
	arpc_ajw_path(&rs->out, s);
}

void arpc_ret_handle(arpc_res *rs, uint64_t id)
{
	set_ret(rs);
	ajw_i64(&rs->out, (long long)id);
}

void arpc_ret_bytes(arpc_res *rs, const unsigned char *b, size_t n)
{
	char *enc = b ? arpc_b64_encode(b, n) : NULL;
	set_ret(rs);
	ajw_str(&rs->out, enc);         /* NULL stays null, not "" */
	free(enc);
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

aj_w *arpc_out_writer(arpc_res *rs, const char *name)
{
	ensure_obj(rs);
	if (!rs->has_ret)
		arpc_ret_null(rs);      /* keep "ret" first for readable traces */
	ajw_key(&rs->out, name);
	return &rs->out;
}

/* A byte buffer arrives base64, because a JSON string stops at the first NUL
 * and a signature is full of them. Malformed text marks the request bad
 * rather than handing libalpm a buffer of the wrong length. */
unsigned char *arpc_arg_bytes(arpc_req *rq, int i, size_t *n)
{
	*n = 0;
	const char *s = arpc_arg_str(rq, i);
	if (!s)
		return NULL;
	unsigned char *b = arpc_b64_decode(s, n);
	if (!b)
		rq->bad = 1;
	return b;
}

void arpc_out_bytes(arpc_res *rs, const char *name, const unsigned char *b,
		    size_t n)
{
	char *enc = b ? arpc_b64_encode(b, n) : NULL;
	ensure_obj(rs);
	if (!rs->has_ret)
		arpc_ret_null(rs);
	ajw_key(&rs->out, name);
	ajw_str(&rs->out, enc);         /* NULL stays null, not "" */
	free(enc);
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

/* The form this connection's paths take, "win32" or "posix". A connection
 * that never says gets the server's own, POSIX, which is what every client
 * got before there was anything to say. */
static int arpc_set_path_style(arpc_req *rq, arpc_res *rs)
{
	const char *style = arpc_arg_str(rq, 0);
	if (arpc_req_bad(rq) || !style ||
	    (strcmp(style, "win32") && strcmp(style, "posix")))
		return arpc_fail(rs, ARPC_E_INVALID_PARAMS,
				 "arpc.set_path_style: win32 or posix");
	arpc_paths_set(!strcmp(style, "win32"));
	arpc_ret_i64(rs, 0);
	return 0;
}

char *arpc_handle_frame(const char *req, size_t len)
{
	aj_doc d;
	if (!aj_parse(&d, req, len)) {
		aj_free(&d);
		return reply_error(0, ARPC_E_PARSE, "malformed JSON");
	}
	return arpc_handle_parsed(&d);
}

static int method_cmp(const void *key, const void *elem)
{
	return strcmp((const char *)key, ((const arpc_method *)elem)->name);
}

char *arpc_handle_parsed(aj_doc *d)
{
	long long id = aj_i64(d, aj_member(d, 0, "id"), 0);
	const char *method = aj_str(d, aj_member(d, 0, "method"), NULL);
	int params = aj_member(d, 0, "params");

	if (!method) {
		aj_free(d);
		return reply_error(id, ARPC_E_INVALID_REQ, "missing method");
	}

	/* Infrastructure, not a libalpm call, so it lives here rather than in
	 * the generated table. Exiting after the current client disconnects is
	 * what the idle timer would do anyway -- this only brings it forward,
	 * which is what lets a rebuild replace the binary. */
	if (!strcmp(method, "arpc.shutdown")) {
		g_shutdown_requested = 1;
		aj_free(d);
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

	/* Infrastructure methods are not libalpm calls, so they are not in the
	 * generated table. They still go through the same req/res path. */
	static const arpc_method builtins[] = {
		{ "arpc.set_path_style", arpc_set_path_style },
		{ "arpc.set_callback", arpc_cb_set },
		{ "arpc.mtree", arpc_mtree_get },
		{ NULL, NULL }
	};

	const arpc_method *m = NULL;
	for (const arpc_method *k = builtins; k->name; k++) {
		if (!strcmp(k->name, method)) {
			m = k;
			break;
		}
	}
	if (!m)
		m = (const arpc_method *)bsearch(method, arpc_methods,
						 arpc_method_count,
						 sizeof(*arpc_methods),
						 method_cmp);
	if (!m) {
		char buf[256];
		snprintf(buf, sizeof(buf), "no such method: %s", method);
		aj_free(d);
		return reply_error(id, ARPC_E_NO_METHOD, buf);
	}

	arpc_req rq = { d, params, 0 };
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
		ajw_obj_end(&w);
		out = finish(&w);
	}

	ajw_free(&rs.out);
	aj_free(d);
	return out ? out : reply_error(id, ARPC_E_INTERNAL, "out of memory");
}
