#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "arpc_client.h"
#include "arpc_b64.h"

/* The caches below are hash tables, and uthash is the whole of one. An add
 * that runs out of memory is not fatal here -- this is a DLL in somebody
 * else's process -- it just means that one thing is not cached. */
#define HASH_NONFATAL_OOM 1
#include <uthash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;         /* made in DllMain, before any stub */
static arpc_identity g_self;            /* who this process is; see arpc_wire.h */
static int g_self_known;
static long long g_next_id = 1;
static long g_conn_refs;                /* libalpm handles alive; under the lock */
static int g_trace;                     /* read once, in DllMain */
static char g_err[256];

/* ------------------------------------------------------------------ lock */

void arpc_enter(void) { EnterCriticalSection(&g_lock); }
void arpc_leave(void) { LeaveCriticalSection(&g_lock); }

/* --------------------------------------------------------------- tracing */

static int tracing(void)
{
	return g_trace;
}

static void set_err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_err, sizeof(g_err), fmt, ap);
	va_end(ap);
	if (tracing())
		fprintf(stderr, "alpmrpc: %s\n", g_err);
}

const char *arpc_last_error(void) { return g_err; }

/* ------------------------------------------------------ locating MSYS2 */

/* This DLL lives at <root>/ucrt64/bin/<name>.dll, so the MSYS2 root is two
 * directories up. That makes the pairing between a client and its server
 * positional rather than configured: no registry, no environment, no
 * install-time wiring. ALPMRPC_ROOT overrides it for out-of-tree testing. */
static int msys_root(char *out, size_t outsz)
{
	DWORD n = GetEnvironmentVariableA("ALPMRPC_ROOT", out, (DWORD)outsz);
	if (n > 0 && n < outsz) {
		/* A trailing separator would hash to a different endpoint than
		 * the server derives, and would escape the closing quote on
		 * its command line. */
		while (n > 1 && (out[n - 1] == '\\' || out[n - 1] == '/'))
			out[--n] = '\0';
		return 1;
	}

	HMODULE self = NULL;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)(void *)&msys_root, &self))
		return 0;

	char path[MAX_PATH];
	n = GetModuleFileNameA(self, path, sizeof(path));
	if (n == 0 || n >= sizeof(path) || !arpc_strip_dirs(path, 3))
		return 0;
	size_t len = strlen(path);
	if (len + 1 > outsz)
		return 0;
	memcpy(out, path, len + 1);
	return 1;
}

/* 1 connected; 0 nothing listening, or busy; -1 something is listening
 * that is not this user's, which is refused for good rather than retried.
 *
 * The name is a rendezvous, not a permission: anyone able to create a pipe
 * of that name first would be who this connects to. So two things before a
 * byte of protocol goes out. The open lets the other end have no more than
 * an anonymous view of this process -- a named-pipe server may otherwise
 * impersonate its client, which is exactly what a squatter would want. And
 * the process at the other end has to be this same user at this same
 * elevation, or the pipe is closed and the caller told. */
static int try_open(const char *name)
{
	HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			       OPEN_EXISTING,
			       SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	if (!arpc_peer_is(h, 0, &g_self)) {
		CloseHandle(h);
		set_err("%s is held by a process that is not this user's at "
			"this elevation; refusing it", name);
		return -1;
	}
	g_pipe = h;
	return 1;
}

static int launch_server(const char *root)
{
	char exe[MAX_PATH * 2];
	DWORD n = GetEnvironmentVariableA("ALPMRPC_SERVER", exe, sizeof(exe));
	if (n == 0 || n >= sizeof(exe))
		snprintf(exe, sizeof(exe), "%s\\usr\\bin\\alpmrpcd.exe", root);

	if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
		set_err("server not found at %s", exe);
		return 0;
	}

	/* Pass the root we resolved rather than letting the server re-derive
	 * it. The two must agree exactly or they hash to different endpoints
	 * and never meet, so there should only be one place that decides. */
	char cmd[MAX_PATH * 4 + 64];
	snprintf(cmd, sizeof(cmd), "\"%s\" --root \"%s\"", exe, root);

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));

	if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
			    CREATE_NO_WINDOW | DETACHED_PROCESS,
			    NULL, NULL, &si, &pi)) {
		set_err("CreateProcess failed (%u)", (unsigned)GetLastError());
		return 0;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	if (tracing())
		fprintf(stderr, "alpmrpc: launched %s\n", exe);
	return 1;
}

static int ensure_connected(void)
{
	if (g_pipe != INVALID_HANDLE_VALUE)
		return 1;

	if (!g_self_known) {
		if (!arpc_process_identity(NULL, &g_self)) {
			set_err("cannot tell which user this process runs as");
			return 0;
		}
		g_self_known = 1;
	}

	char root[MAX_PATH];
	if (!msys_root(root, sizeof(root))) {
		set_err("cannot locate the MSYS2 root from this module's path");
		return 0;
	}

	char name[256];
	if (!arpc_pipe_name(root, name, sizeof(name))) {
		set_err("cannot derive endpoint name");
		return 0;
	}
	int r = try_open(name);
	if (r)
		return r > 0;

	/* Nothing listening: start a server and wait for it to. Two clients
	 * starting at once both do this and get a server each -- which is
	 * what they would have got anyway, since a server takes one
	 * connection at a time -- and the spare one idles out. Nothing here
	 * waits on anybody else. */
	if (launch_server(root)) {
		for (int waited = 0; waited < 10000 && r == 0; waited += 25) {
			Sleep(25);
			r = try_open(name);
		}
	}
	if (r == 0 && !g_err[0])
		set_err("server did not start listening within 10s");
	return r > 0;
}

static void disconnect(void)
{
	if (g_pipe != INVALID_HANDLE_VALUE) {
		CloseHandle(g_pipe);
		g_pipe = INVALID_HANDLE_VALUE;
		if (tracing())
			fprintf(stderr, "alpmrpc: disconnected\n");
	}
}

static void free_groups_of(uint64_t root);
static void drop_column(uint64_t pkg, const char *field);
static size_t count_groups(void);

/* ---------------------------------------------------- borrowed-string cache */

/* One cache for everything libalpm hands back as borrowed: strings and
 * lists both stay valid as long as the object they came from, so both are
 * keyed by (owning handle, function name) and released together when that
 * handle is. The key is the id and the name side by side in one buffer,
 * which is what the table hashes.
 *
 * An entry can be detached: a call that changes things on the server makes
 * what was cached under its handle stale, so the entry stops answering
 * lookups and the next read fetches afresh -- but it is not freed, because
 * a caller may still be holding what it points at, exactly as it could hold
 * libalpm's own list across an append. A detached entry leaves the table
 * for a plain list, since its key is about to be taken by what replaces it,
 * and goes with the handle. */
typedef struct owned {
	char *k;                /* the owner id, then the function name */
	size_t klen;
	uint64_t owner;
	void *value;
	void (*release)(void *);        /* frees value, whatever it is */
	int is_str;             /* interned by value; the rest by identity */
	UT_hash_handle hh;      /* while live */
	struct owned *next;     /* while detached */
} owned;

static owned *g_owned;          /* live, by (owner, key) */
static owned *g_detached;       /* detached, until their root goes */

/* The hash key. Built fresh for a lookup, and kept by an entry added with
 * it. The name is a generated literal, but its address is not relied on:
 * two literals of one name need not share it. */
static char *make_key(uint64_t owner, const char *key, size_t *klen)
{
	size_t n = strlen(key);
	char *k = (char *)malloc(sizeof(owner) + n);
	if (!k)
		return NULL;
	memcpy(k, &owner, sizeof(owner));
	memcpy(k + sizeof(owner), key, n);
	*klen = sizeof(owner) + n;
	return k;
}

static int key_is(const owned *o, const char *key)
{
	size_t n = strlen(key);
	return o->klen == sizeof(o->owner) + n &&
	       !memcmp(o->k + sizeof(o->owner), key, n);
}

/* Does the entry answer for this getter? Its key is the getter's name, or
 * the name and the arguments after a colon -- one group per name. */
static int key_names(const owned *o, const char *getter)
{
	size_t n = strlen(getter);
	const char *k = o->k + sizeof(o->owner);
	size_t klen = o->klen - sizeof(o->owner);
	return klen >= n && !memcmp(k, getter, n) &&
	       (klen == n || k[n] == ':');
}

static void release_owned(owned *o)
{
	if (o->release)
		o->release(o->value);
	free(o->k);
	free(o);
}

static owned *find_owned(uint64_t owner, const char *key)
{
	/* The lookup key is an id and a getter's name, with its arguments
	 * after a colon: short, so it is built on the stack, with the heap
	 * for the odd long one. */
	char stack[sizeof(owner) + 128];
	size_t n = strlen(key);
	char *k = stack;
	if (sizeof(owner) + n > sizeof(stack) &&
	    !(k = (char *)malloc(sizeof(owner) + n)))
		return NULL;
	memcpy(k, &owner, sizeof(owner));
	memcpy(k + sizeof(owner), key, n);
	owned *o;
	HASH_FIND(hh, g_owned, k, sizeof(owner) + n, o);
	if (k != stack)
		free(k);
	return o;
}

/* A new live entry under (owner, key), or NULL if it could not be made. */
static owned *add_owned(uint64_t owner, const char *key)
{
	owned *o = (owned *)calloc(1, sizeof(*o));
	if (!o)
		return NULL;
	o->k = make_key(owner, key, &o->klen);
	if (o->k) {
		o->owner = owner;
		HASH_ADD_KEYPTR(hh, g_owned, o->k, o->klen, o);
	}
	if (!o->k || !o->hh.tbl) {      /* out of memory: not cached, then */
		free(o->k);
		free(o);
		return NULL;
	}
	return o;
}

char *arpc_dup(const char *s)
{
	return s ? _strdup(s) : NULL;
}

void arpc_free_list(alpm_list_t *l, alpm_list_fn_free elem_free)
{
	if (!l)
		return;
	if (elem_free)
		alpm_list_free_inner(l, elem_free);
	alpm_list_free(l);
}

static const char *intern(uint64_t owner, const char *key, const char *value)
{
	owned *p = find_owned(owner, key);
	if (p && p->is_str) {
		if (value && p->value && !strcmp((char *)p->value, value))
			return (char *)p->value;
		/* Changed under a live entry, which no declared mutator did.
		 * The old string may be in a caller's hands, so it is detached
		 * rather than freed, like everything else that changes. */
		HASH_DEL(g_owned, p);
		p->next = g_detached;
		g_detached = p;
	}
	/* Detached by an invalidation but unchanged since: take it back, so
	 * the pointer a caller already holds stays the one this returns, as
	 * libalpm's would. */
	for (owned **pp = &g_detached; *pp; pp = &(*pp)->next) {
		owned *q = *pp;
		if (!q->is_str || q->owner != owner || !key_is(q, key) ||
		    !value || !q->value || strcmp((char *)q->value, value))
			continue;
		HASH_ADD_KEYPTR(hh, g_owned, q->k, q->klen, q);
		if (!q->hh.tbl)
			continue;       /* out of memory: it stays detached */
		*pp = q->next;
		q->next = NULL;
		return (char *)q->value;
	}
	owned *n = add_owned(owner, key);
	if (!n)
		return NULL;
	n->value = arpc_dup(value);
	n->release = free;
	n->is_str = 1;
	return (char *)n->value;
}

int arpc_cached(uint64_t owner, const char *key, void **out)
{
	owned *p = find_owned(owner, key);
	int found = p && !p->is_str;
	*out = found ? p->value : NULL;
	return found;
}

void *arpc_cache(uint64_t owner, const char *key, void *value,
		 void (*release)(void *))
{
	owned *p = find_owned(owner, key);
	if (p && !p->is_str) {
		/* Cached while this call was in flight: a callback it raised
		 * fetched the same thing, and whoever asked may hold that
		 * one. It is the one both get; the newcomer goes. */
		release(value);
		return p->value;
	}
	p = add_owned(owner, key);
	if (p) {
		p->value = value;
		p->release = release;
	}
	return value;
}

/* Release every live entry, then every detached one, that `drop` accepts. */
static void purge_where(int (*drop)(const owned *, uint64_t), uint64_t arg)
{
	owned *o, *tmp;
	HASH_ITER(hh, g_owned, o, tmp) {
		if (drop(o, arg)) {
			HASH_DEL(g_owned, o);
			release_owned(o);
		}
	}
	owned **pp = &g_detached;
	while (*pp) {
		if (drop(*pp, arg)) {
			owned *dead = *pp;
			*pp = dead->next;
			release_owned(dead);
		} else {
			pp = &(*pp)->next;
		}
	}
}

static int owned_by(const owned *o, uint64_t owner)
{
	return o->owner == owner;
}

static int under_root(const owned *o, uint64_t root)
{
	return ARPC_ROOT(o->owner) == root;
}

/* A package or db freed: what was cached under that one object goes, and
 * nothing else. The column caches are left alone -- a freed package's
 * entries in them are dead weight, not a hazard, and rebuilding every
 * column for the rest of its list would turn one free into a round trip
 * per field per package. */
void arpc_purge_owner(uint64_t owner)
{
	purge_where(owned_by, owner);
}

/* A handle released: everything under it, detached or not. */
void arpc_purge_root(uint64_t handle)
{
	uint64_t root = ARPC_ROOT(handle);
	purge_where(under_root, root);
	free_groups_of(root);
	arpc_callbacks_purge(handle);
}

/* Something changed on the server, and these are the cached results it can
 * have changed: detach them, so the next read of one fetches afresh. */
void arpc_detach(uint64_t owner, int whole_root, const char *const *keys)
{
	uint64_t root = ARPC_ROOT(owner);
	owned *o, *tmp;
	HASH_ITER(hh, g_owned, o, tmp) {
		if (whole_root ? ARPC_ROOT(o->owner) != root : o->owner != owner)
			continue;
		for (const char *const *k = keys; *k; k++) {
			if (key_names(o, *k)) {
				HASH_DEL(g_owned, o);
				o->next = g_detached;
				g_detached = o;
				break;
			}
		}
	}
}

void arpc_drop_column(uint64_t pkg, const char *field)
{
	drop_column(pkg, field);
}

/* Called from outside any stub, so it takes the lock itself. */
size_t arpc_stats_cached(void)
{
	arpc_enter();
	size_t n = HASH_COUNT(g_owned) + count_groups();
	for (owned *p = g_detached; p; p = p->next)
		n++;
	arpc_leave();
	return n;
}

/* Strings that belong to nobody -- alpm_strerror's, alpm_version's -- are
 * static in libalpm, and are kept for good here: one copy per value. */
typedef struct sstr {
	struct sstr *next;
	char *s;
} sstr;

static sstr *g_static;

const char *arpc_intern_static(arpc_call *c)
{
	const char *s = aj_str(&c->rsp, aj_member(&c->rsp, c->result, "ret"),
			       NULL);
	if (!s)
		return NULL;
	for (sstr *p = g_static; p; p = p->next)
		if (!strcmp(p->s, s))
			return p->s;
	sstr *n = (sstr *)calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->s = _strdup(s);
	if (!n->s) {
		free(n);
		return NULL;
	}
	n->next = g_static;
	g_static = n;
	return n->s;
}


/* ------------------------------------------------- batched package fields */

typedef struct column {
	struct column *next;
	const char *field;      /* generated literal; static lifetime */
	char **str;             /* one per member, or NULL for a numeric column */
	long long *num;
} column;

typedef struct pkg_group {
	struct pkg_group *next;
	uint64_t *ids;          /* member order, as the caller sees the list */
	size_t n;
	column *cols;
} pkg_group;

/* Which group a package is in, and where in it: one entry per package id.
 * A package that is in two lists belongs to whichever was registered
 * first; a column holds the same values either way. */
typedef struct {
	uint64_t id;
	pkg_group *g;
	size_t slot;
	UT_hash_handle hh;
} pkg_slot;

static pkg_group *g_groups;
static pkg_slot *g_slots;

static void free_column(column *c, size_t n)
{
	if (c->str) {
		for (size_t i = 0; i < n; i++)
			free(c->str[i]);
		free(c->str);
	}
	free(c->num);
	free(c);
}

static void drop_columns(pkg_group *g)
{
	while (g->cols) {
		column *c = g->cols;
		g->cols = c->next;
		free_column(c, g->n);
	}
}

static void free_group(pkg_group *g)
{
	drop_columns(g);
	free(g->ids);
	free(g);
}

/* Every id in a group came off one list, filed under one owner, so they
 * share a root; the first one speaks for all. */
static uint64_t group_root(const pkg_group *g)
{
	return ARPC_ROOT(g->ids[0]);
}

static void free_groups_of(uint64_t root)
{
	pkg_group **pp = &g_groups;
	while (*pp) {
		if (group_root(*pp) == root) {
			pkg_group *dead = *pp;
			*pp = dead->next;
			free_group(dead);
		} else {
			pp = &(*pp)->next;
		}
	}
	pkg_slot *s, *tmp;
	HASH_ITER(hh, g_slots, s, tmp) {
		if (ARPC_ROOT(s->id) == root) {
			HASH_DEL(g_slots, s);
			free(s);
		}
	}
}

/* One batched field of one package's group, which a setter changed:
 * alpm_pkg_set_reason. The overlay only ever names a numeric column for
 * this -- a caller may hold the pointer it got from a string one, which
 * natively lives as long as the package -- so nothing handed out is freed;
 * the group's other members merely read that field afresh too. */
static void drop_column(uint64_t pkg, const char *field)
{
	pkg_slot *s;
	HASH_FIND(hh, g_slots, &pkg, sizeof(pkg), s);
	if (!s)
		return;
	for (column **pp = &s->g->cols; *pp; pp = &(*pp)->next) {
		if (!strcmp((*pp)->field, field)) {
			column *c = *pp;
			*pp = c->next;
			free_column(c, s->g->n);
			return;
		}
	}
}

static size_t count_groups(void)
{
	size_t n = 0;
	for (pkg_group *g = g_groups; g; g = g->next)
		n++;
	return n;
}

/* A group for these ids, in this order. A member that already has a group
 * keeps it, and if every member does, this group would never be asked and
 * is not made: the same object always gets the same id, so a list fetched
 * again after an invalidation is one this has seen, columns and all. */
static pkg_group *group_new(const uint64_t *ids, size_t n)
{
	pkg_group *g = (pkg_group *)calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->ids = (uint64_t *)malloc((n ? n : 1) * sizeof(*g->ids));
	if (!g->ids) {
		free(g);
		return NULL;
	}
	memcpy(g->ids, ids, n * sizeof(*ids));
	g->n = n;

	size_t placed = 0;
	for (size_t i = 0; i < n; i++) {
		pkg_slot *s;
		HASH_FIND(hh, g_slots, &ids[i], sizeof(ids[i]), s);
		if (s)
			continue;
		s = (pkg_slot *)calloc(1, sizeof(*s));
		if (!s)
			continue;
		s->id = ids[i];
		s->g = g;
		s->slot = i;
		HASH_ADD(hh, g_slots, id, sizeof(s->id), s);
		if (!s->hh.tbl) {       /* out of memory: that one goes unbatched */
			free(s);
			continue;
		}
		placed++;
	}
	if (!placed) {
		free_group(g);
		return NULL;
	}
	g->next = g_groups;
	g_groups = g;
	return g;
}

void arpc_pkg_group_register(const alpm_list_t *pkgs)
{
	size_t n = 0;
	for (const alpm_list_t *i = pkgs; i; i = i->next)
		n++;
	if (n == 0)
		return;

	uint64_t *ids = (uint64_t *)calloc(n, sizeof(*ids));
	if (!ids)
		return;
	size_t k = 0;
	for (const alpm_list_t *i = pkgs; i; i = i->next)
		ids[k++] = ARPC_ID(i->data);

	group_new(ids, n);
	free(ids);
}

/* One round trip: this field, for every member of the group. */
static column *fetch_column(pkg_group *g, const char *field, int want_str)
{
	arpc_call c;
	if (!arpc_begin(&c, "arpc.pkg_fields"))
		return NULL;

	ajw_arr_begin(&c.req);
	/* Member order, so the response lines up with the slots directly. */
	for (size_t i = 0; i < g->n; i++)
		ajw_i64(&c.req, (long long)g->ids[i]);
	ajw_arr_end(&c.req);
	arpc_put_str(&c, field);

	if (!arpc_invoke(&c)) {
		arpc_end(&c);
		return NULL;
	}

	column *col = (column *)calloc(1, sizeof(*col));
	if (!col) {
		arpc_end(&c);
		return NULL;
	}
	col->field = field;
	if (want_str)
		col->str = (char **)calloc(g->n, sizeof(*col->str));
	else
		col->num = (long long *)calloc(g->n, sizeof(*col->num));

	int arr = arpc_ret_node(&c);
	const aj_doc *d = arpc_doc(&c);
	/* One walk, not one restart per element: indexing this by aj_elem() is
	 * quadratic, and a column is as long as the package list. */
	size_t i = 0;
	for (int e = aj_first(d, arr); e >= 0 && i < g->n;
	     e = aj_next(d, e), i++) {
		if (want_str)
			col->str[i] = arpc_dup(aj_str(d, e, NULL));
		else
			col->num[i] = aj_i64(d, e, 0);
	}
	arpc_end(&c);

	col->next = g->cols;
	g->cols = col;
	return col;
}

static column *column_for(pkg_group *g, const char *field, int want_str)
{
	for (column *c = g->cols; c; c = c->next)
		if (!strcmp(c->field, field))
			return c;
	return fetch_column(g, field, want_str);
}

/* Resolve a package to its group, creating a group of one if it belongs to
 * no list. Called with the lock held. */
static pkg_group *group_for_pkg(uint64_t id, size_t *slot)
{
	pkg_slot *s;
	HASH_FIND(hh, g_slots, &id, sizeof(id), s);
	if (s) {
		*slot = s->slot;
		return s->g;
	}
	pkg_group *g = group_new(&id, 1);
	if (g)
		*slot = 0;
	return g;
}

/* These two are a batched accessor's whole stub, so they take the lock. */
const char *arpc_pkg_field_str(uint64_t id, const char *field)
{
	if (!id)
		return NULL;
	arpc_enter();
	size_t slot = 0;
	const char *r = NULL;
	pkg_group *g = group_for_pkg(id, &slot);
	if (g) {
		column *c = column_for(g, field, 1);
		if (c && c->str)
			r = c->str[slot];
	}
	arpc_leave();
	return r;
}

long long arpc_pkg_field_i64(uint64_t id, const char *field)
{
	if (!id)
		return 0;
	arpc_enter();
	size_t slot = 0;
	long long r = 0;
	pkg_group *g = group_for_pkg(id, &slot);
	if (g) {
		column *c = column_for(g, field, 0);
		if (c && c->num)
			r = c->num[slot];
	}
	arpc_leave();
	return r;
}

/* ------------------------------------------------- connection refcounting */

void arpc_conn_ref(void)
{
	g_conn_refs++;
}

void arpc_conn_unref(void)
{
	if (--g_conn_refs > 0)
		return;
	/* Last libalpm handle is gone: drop the pipe. The server sees the
	 * broken pipe, and exits on its own once its idle timer expires. */
	disconnect();
}

/* ------------------------------------------------------------- framed I/O */

/* The framing is shared with the server (arpc_wire.c). What is this side's
 * own is that a failure ends the connection and says why. */
static int send_frame(const char *buf, size_t len)
{
	if (!arpc_send_frame(g_pipe, NULL, buf, len)) {
		set_err("write failed (%u)", (unsigned)GetLastError());
		disconnect();
		return 0;
	}
	return 1;
}

static char *recv_frame(void)
{
	size_t len = 0;
	char *f = arpc_recv_frame(g_pipe, NULL, &len);
	if (!f) {
		if (len)
			set_err("server sent a %zu byte frame", len);
		else
			set_err("read failed (%u)", (unsigned)GetLastError());
		disconnect();
	}
	return f;
}

/* Send a request and read frames until the reply to it arrives.
 *
 * What arrives in between are callbacks. libalpm calls back from inside the
 * call we are waiting on -- a question during a commit, progress during an
 * install -- so the server sends those up the same pipe and blocks until we
 * answer. This loop is what makes that work: it is not an optimisation, it
 * is the only shape in which a synchronous callback can be served. */
static char *transact(const char *req, size_t len)
{
	if (!send_frame(req, len))
		return NULL;

	for (;;) {
		char *f = recv_frame();
		if (!f)
			return NULL;

		aj_doc d;
		if (!aj_parse(&d, f, strlen(f))) {
			aj_free(&d);
			free(f);
			set_err("malformed frame from server");
			disconnect();
			return NULL;
		}
		int is_cb = aj_member(&d, 0, "cb") >= 0;
		if (!is_cb) {
			aj_free(&d);
			return f;               /* the reply we were waiting for */
		}

		if (tracing())
			fprintf(stderr, "alpmrpc <cb %s\n", f);

		aj_w reply;
		ajw_init(&reply);
		arpc_dispatch_callback(&d, &reply);
		aj_free(&d);
		free(f);

		if (reply.err || !send_frame(reply.buf, reply.len)) {
			ajw_free(&reply);
			return NULL;
		}
		ajw_free(&reply);
	}
}

/* ------------------------------------------------------------- call cycle */

int arpc_begin(arpc_call *c, const char *method)
{
	memset(c, 0, sizeof(*c));
	c->result = -1;

	if (!ensure_connected())
		return 0;

	c->id = g_next_id++;
	ajw_init(&c->req);
	ajw_obj_begin(&c->req);
	ajw_key(&c->req, "id");
	ajw_i64(&c->req, c->id);
	ajw_key(&c->req, "method");
	ajw_str(&c->req, method);
	ajw_key(&c->req, "params");
	ajw_arr_begin(&c->req);
	return 1;
}

void arpc_put_null(arpc_call *c)                  { ajw_null(&c->req); }
void arpc_obj_begin(arpc_call *c)                 { ajw_obj_begin(&c->req); }
void arpc_obj_end(arpc_call *c)                   { ajw_obj_end(&c->req); }
void arpc_key(arpc_call *c, const char *key)      { ajw_key(&c->req, key); }

void arpc_put_str(arpc_call *c, const char *s)    { ajw_str(&c->req, s); }
void arpc_put_i64(arpc_call *c, long long v)      { ajw_i64(&c->req, v); }
void arpc_put_handle(arpc_call *c, uint64_t id)   { ajw_i64(&c->req, (long long)id); }

/* Bytes, not text: a signature has NULs in it and a JSON string does not
 * survive them, so it travels base64. The encoding is done and freed here so
 * a generated stub has no temporary to clean up on its failure paths. */
void arpc_put_bytes(arpc_call *c, const unsigned char *b, size_t n)
{
	if (!b) {
		ajw_null(&c->req);
		return;
	}
	char *enc = arpc_b64_encode(b, n);
	ajw_str(&c->req, enc);
	free(enc);
}

unsigned char *arpc_ret_bytes(arpc_call *c, size_t *n)
{
	*n = 0;
	const char *s = aj_str(&c->rsp, aj_member(&c->rsp, c->result, "ret"),
			       NULL);
	return s ? arpc_b64_decode(s, n) : NULL;
}

unsigned char *arpc_out_bytes(arpc_call *c, const char *name, size_t *n)
{
	*n = 0;
	const char *s = aj_str(&c->rsp, aj_member(&c->rsp, c->result, name),
			       NULL);
	return s ? arpc_b64_decode(s, n) : NULL;
}

void arpc_put_str_list(arpc_call *c, const alpm_list_t *l)
{
	if (!l) {
		ajw_null(&c->req);      /* NULL and empty are distinct on the wire */
		return;
	}
	ajw_arr_begin(&c->req);
	for (; l; l = l->next)
		ajw_str(&c->req, (const char *)l->data);
	ajw_arr_end(&c->req);
}

void arpc_put_handle_list(arpc_call *c, const alpm_list_t *l)
{
	if (!l) {
		ajw_null(&c->req);
		return;
	}
	ajw_arr_begin(&c->req);
	for (; l; l = l->next)
		ajw_i64(&c->req, (long long)ARPC_ID(l->data));
	ajw_arr_end(&c->req);
}

int arpc_invoke(arpc_call *c)
{
	ajw_arr_end(&c->req);
	ajw_obj_end(&c->req);
	if (c->req.err) {
		set_err("out of memory building request");
		return 0;
	}
	if (tracing())
		fprintf(stderr, "alpmrpc --> %s\n", c->req.buf);

	char *rsp = transact(c->req.buf, c->req.len);
	if (!rsp)
		return 0;
	if (tracing())
		fprintf(stderr, "alpmrpc <-- %s\n", rsp);

	int ok = aj_parse(&c->rsp, rsp, strlen(rsp));
	free(rsp);
	if (!ok) {
		set_err("malformed response");
		return 0;
	}
	c->parsed = 1;

	/* The exchange is strictly ordered, so the reply is the one to this
	 * request -- unless the pipe is a frame out of step, in which case
	 * every answer from here on would be to the wrong question. The
	 * server checks its callbacks' replies the same way. */
	if (aj_i64(&c->rsp, aj_member(&c->rsp, 0, "id"), -1) != c->id) {
		set_err("reply to request %lld arrived out of step",
			aj_i64(&c->rsp, aj_member(&c->rsp, 0, "id"), -1));
		disconnect();
		return 0;
	}

	int err = aj_member(&c->rsp, 0, "error");
	if (err >= 0) {
		set_err("%s (%lld)",
			aj_str(&c->rsp, aj_member(&c->rsp, err, "message"), "error"),
			aj_i64(&c->rsp, aj_member(&c->rsp, err, "code"), 0));
		return 0;
	}
	c->result = aj_member(&c->rsp, 0, "result");
	return c->result >= 0;
}

void arpc_end(arpc_call *c)
{
	ajw_free(&c->req);
	if (c->parsed)
		aj_free(&c->rsp);
}

const aj_doc *arpc_doc(const arpc_call *c)
{
	return &c->rsp;
}

int arpc_ret_node(const arpc_call *c)
{
	return aj_member(&c->rsp, c->result, "ret");
}

int arpc_out_node(const arpc_call *c, const char *name)
{
	return aj_member(&c->rsp, c->result, name);
}

long long arpc_ret_i64(arpc_call *c)
{
	return aj_i64(&c->rsp, aj_member(&c->rsp, c->result, "ret"), 0);
}

uint64_t arpc_ret_handle(arpc_call *c)
{
	return (uint64_t)aj_i64(&c->rsp, aj_member(&c->rsp, c->result, "ret"), 0);
}

long long arpc_out_i64(arpc_call *c, const char *name)
{
	return aj_i64(&c->rsp, aj_member(&c->rsp, c->result, name), 0);
}

const char *arpc_intern_str(arpc_call *c, uint64_t owner, const char *key)
{
	const char *s = aj_str(&c->rsp, aj_member(&c->rsp, c->result, "ret"), NULL);
	if (!s)
		return NULL;
	return intern(owner, key, s);
}

char *arpc_take_str(arpc_call *c)
{
	const char *s = aj_str(&c->rsp, aj_member(&c->rsp, c->result, "ret"), NULL);
	return s ? _strdup(s) : NULL;
}

/* The lock is made here, before any thread can reach a stub, which is the
 * one place that holds without a guard of its own; the trace switch is read
 * here for the same reason. */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		InitializeCriticalSection(&g_lock);
		char buf[8];
		DWORD n = GetEnvironmentVariableA("ALPMRPC_TRACE", buf,
						  sizeof(buf));
		g_trace = n > 0 && buf[0] && buf[0] != '0';
	} else if (reason == DLL_PROCESS_DETACH) {
		disconnect();
		DeleteCriticalSection(&g_lock);
	}
	return TRUE;
}
