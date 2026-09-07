#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "arpc_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static LONG g_lock_ready;
static long long g_next_id = 1;
static long g_conn_refs;
static int g_trace = -1;
static char g_err[256];

/* --------------------------------------------------------------- tracing */

static int tracing(void)
{
	if (g_trace < 0) {
		char buf[8];
		DWORD n = GetEnvironmentVariableA("ALPMRPC_TRACE", buf, sizeof(buf));
		g_trace = (n > 0 && buf[0] && buf[0] != '0');
	}
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

static void lock_init_once(void)
{
	if (InterlockedCompareExchange(&g_lock_ready, 1, 0) == 0) {
		InitializeCriticalSection(&g_lock);
		InterlockedExchange(&g_lock_ready, 2);
	}
	while (InterlockedCompareExchange(&g_lock_ready, 2, 2) != 2)
		Sleep(0);
}

/* ------------------------------------------------------ locating MSYS2 */

/* This DLL lives at <root>/ucrt64/bin/<name>.dll, so the MSYS2 root is two
 * directories up. That makes the pairing between a client and its server
 * positional rather than configured: no registry, no environment, no
 * install-time wiring. ALPMRPC_ROOT overrides it for out-of-tree testing. */
static int msys_root(char *out, size_t outsz)
{
	DWORD n = GetEnvironmentVariableA("ALPMRPC_ROOT", out, (DWORD)outsz);
	if (n > 0 && n < outsz)
		return 1;

	HMODULE self = NULL;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)(void *)&msys_root, &self))
		return 0;

	char path[MAX_PATH];
	n = GetModuleFileNameA(self, path, sizeof(path));
	if (n == 0 || n >= sizeof(path))
		return 0;

	for (int up = 0; up < 3; up++) {
		char *a = strrchr(path, '\\');
		char *b = strrchr(path, '/');
		if (b > a)
			a = b;
		if (!a)
			return 0;
		*a = '\0';
	}
	size_t len = strlen(path);
	if (len + 1 > outsz)
		return 0;
	memcpy(out, path, len + 1);
	return 1;
}

static int try_open(const char *name)
{
	HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			       OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
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
	if (try_open(name))
		return 1;

	/* Two clients starting at once must not both spawn a server. The
	 * loser of the mutex waits and then finds the winner's server. */
	char mname[320];
	snprintf(mname, sizeof(mname), "Local\\alpmrpc.launch.%s",
		 name + sizeof("\\\\.\\pipe\\") - 1);
	HANDLE mtx = CreateMutexA(NULL, FALSE, mname);
	if (mtx)
		WaitForSingleObject(mtx, 10000);

	int ok = try_open(name);          /* recheck under the mutex */
	if (!ok && launch_server(root)) {
		for (int waited = 0; waited < 10000; waited += 25) {
			Sleep(25);
			if (try_open(name)) {
				ok = 1;
				break;
			}
		}
	}
	if (mtx) {
		ReleaseMutex(mtx);
		CloseHandle(mtx);
	}
	if (!ok && !g_err[0])
		set_err("server did not start listening within 10s");
	return ok;
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

/* ---------------------------------------------------- borrowed-string cache */

/* One cache for everything libalpm hands back as borrowed: strings and
 * lists both stay valid as long as the object they came from, so both are
 * keyed by (owning handle, function name) and released together when that
 * handle is. */
typedef struct owned {
	struct owned *next;
	uint64_t owner;
	const char *key;        /* generated literal; static lifetime */
	void *value;
	alpm_list_fn_free elem_free;
	int is_list;
} owned;

static owned *g_owned;

static void release_owned(owned *o)
{
	if (o->is_list)
		arpc_free_list((alpm_list_t *)o->value, o->elem_free);
	else
		free(o->value);
	free(o);
}

static owned *find_owned(uint64_t owner, const char *key)
{
	for (owned *p = g_owned; p; p = p->next)
		if (p->owner == owner && !strcmp(p->key, key))
			return p;
	return NULL;
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
	if (p && !p->is_list) {
		if (value && p->value && !strcmp((char *)p->value, value))
			return (char *)p->value;
		free(p->value);
		p->value = arpc_dup(value);
		return (char *)p->value;
	}
	owned *n = (owned *)calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->owner = owner;
	n->key = key;
	n->value = arpc_dup(value);
	n->next = g_owned;
	g_owned = n;
	return (char *)n->value;
}

alpm_list_t *arpc_cached_list(uint64_t owner, const char *key)
{
	lock_init_once();
	EnterCriticalSection(&g_lock);
	owned *p = find_owned(owner, key);
	alpm_list_t *r = (p && p->is_list) ? (alpm_list_t *)p->value : NULL;
	LeaveCriticalSection(&g_lock);
	return r;
}

void arpc_cache_list(uint64_t owner, const char *key, alpm_list_t *list,
		     alpm_list_fn_free elem_free)
{
	lock_init_once();
	EnterCriticalSection(&g_lock);
	owned *p = find_owned(owner, key);
	if (p && p->is_list) {
		/* Only reachable if two threads raced past the pre-call
		 * lookup. Keep the newcomer and drop the loser. */
		arpc_free_list((alpm_list_t *)p->value, p->elem_free);
		p->value = list;
		p->elem_free = elem_free;
	} else {
		owned *n = (owned *)calloc(1, sizeof(*n));
		if (n) {
			n->owner = owner;
			n->key = key;
			n->value = list;
			n->elem_free = elem_free;
			n->is_list = 1;
			n->next = g_owned;
			g_owned = n;
		}
	}
	LeaveCriticalSection(&g_lock);
}

void arpc_purge_owner(uint64_t owner)
{
	lock_init_once();
	EnterCriticalSection(&g_lock);
	owned **pp = &g_owned;
	while (*pp) {
		if ((*pp)->owner == owner) {
			owned *dead = *pp;
			*pp = dead->next;
			release_owned(dead);
		} else {
			pp = &(*pp)->next;
		}
	}
	LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------- connection refcounting */

void arpc_conn_ref(void)
{
	InterlockedIncrement(&g_conn_refs);
}

void arpc_conn_unref(void)
{
	if (InterlockedDecrement(&g_conn_refs) > 0)
		return;
	/* Last libalpm handle is gone: drop the pipe. The server sees the
	 * broken pipe, and exits on its own once its idle timer expires. */
	lock_init_once();
	EnterCriticalSection(&g_lock);
	disconnect();
	LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------------------- framed I/O */

static int io_exact(HANDLE h, void *buf, DWORD n, int writing)
{
	char *p = (char *)buf;
	while (n) {
		DWORD did = 0;
		BOOL ok = writing ? WriteFile(h, p, n, &did, NULL)
				  : ReadFile(h, p, n, &did, NULL);
		if (!ok || did == 0)
			return 0;
		p += did;
		n -= did;
	}
	return 1;
}

static char *transact(const char *req, size_t len)
{
	unsigned char hdr[4];
	hdr[0] = (unsigned char)(len & 0xFF);
	hdr[1] = (unsigned char)((len >> 8) & 0xFF);
	hdr[2] = (unsigned char)((len >> 16) & 0xFF);
	hdr[3] = (unsigned char)((len >> 24) & 0xFF);

	if (!io_exact(g_pipe, hdr, 4, 1) ||
	    !io_exact(g_pipe, (void *)req, (DWORD)len, 1)) {
		set_err("write failed (%u)", (unsigned)GetLastError());
		disconnect();
		return NULL;
	}
	if (!io_exact(g_pipe, hdr, 4, 0)) {
		set_err("read failed (%u)", (unsigned)GetLastError());
		disconnect();
		return NULL;
	}
	unsigned rlen = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
			((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
	if (rlen == 0 || rlen > ARPC_MAX_FRAME) {
		set_err("server sent a %u byte frame", rlen);
		disconnect();
		return NULL;
	}
	char *buf = (char *)malloc(rlen + 1);
	if (!buf)
		return NULL;
	if (!io_exact(g_pipe, buf, rlen, 0)) {
		free(buf);
		set_err("short read");
		disconnect();
		return NULL;
	}
	buf[rlen] = '\0';
	return buf;
}

/* ------------------------------------------------------------- call cycle */

int arpc_begin(arpc_call *c, const char *method)
{
	memset(c, 0, sizeof(*c));
	c->result = -1;

	lock_init_once();
	EnterCriticalSection(&g_lock);
	c->held_lock = 1;

	if (!ensure_connected()) {
		LeaveCriticalSection(&g_lock);
		c->held_lock = 0;
		return 0;
	}

	ajw_init(&c->req);
	ajw_obj_begin(&c->req);
	ajw_key(&c->req, "id");
	ajw_i64(&c->req, g_next_id++);
	ajw_key(&c->req, "method");
	ajw_str(&c->req, method);
	ajw_key(&c->req, "params");
	ajw_arr_begin(&c->req);
	return 1;
}

void arpc_put_str(arpc_call *c, const char *s)    { ajw_str(&c->req, s); }
void arpc_put_i64(arpc_call *c, long long v)      { ajw_i64(&c->req, v); }
void arpc_put_handle(arpc_call *c, uint64_t id)   { ajw_i64(&c->req, (long long)id); }

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
	if (c->held_lock) {
		LeaveCriticalSection(&g_lock);
		c->held_lock = 0;
	}
}

const aj_doc *arpc_doc(const arpc_call *c)
{
	return &c->rsp;
}

int arpc_ret_node(const arpc_call *c)
{
	return aj_member(&c->rsp, c->result, "ret");
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

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst;
	(void)reserved;
	if (reason == DLL_PROCESS_DETACH)
		disconnect();
	return TRUE;
}
