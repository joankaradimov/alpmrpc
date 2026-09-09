/* Client-side runtime that generated stubs are written against.
 *
 * Runs inside an arbitrary mingw/clang process. The rule is that nothing from
 * the MSYS2 tree is ever loaded into it -- the only contact with MSYS2 is a
 * pipe. Ordinary mingw libraries are not a problem; libarchive is linked for
 * the mtree stream, the same one a caller would be using.
 *
 * Handles are not proxied objects: an alpm_db_t* on this side is the server's
 * handle id cast to a pointer. Nothing ever dereferences it, NULL maps to id
 * 0 in both directions, and a stale id fails a server-side lookup rather than
 * aliasing live memory.
 */
#ifndef ARPC_CLIENT_H
#define ARPC_CLIENT_H

#include "arpc_json.h"
#include "arpc_wire.h"

/* alpm_list.h is standalone by design and the client links the
 * real implementation, so lists here are genuine alpm lists. */
#include <alpm_list.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARPC_ID(p) ((uint64_t)(uintptr_t)(p))

typedef struct {
	aj_w req;
	aj_doc rsp;
	long long id;           /* the request's, checked against the reply's */
	int result;             /* node index of "result", or -1 */
	int parsed;
} arpc_call;

/* ---- the lock ----
 *
 * One lock, and one rule: a stub holds it for the whole of a call, from the
 * cache lookup that may answer it to the cache store after it, callbacks
 * included, since they arrive inside the call and their own calls nest in
 * the same lock. Nothing below this line locks for itself, so there is
 * nothing to race: two threads calling through this DLL take turns, one
 * whole call at a time. The lock is recursive, which is what lets a
 * callback's calls nest. */
void arpc_enter(void);
void arpc_leave(void);

/* Opens the connection on first use, launching the server if it is not
 * already listening. Returns 0 if the server could not be reached, in which
 * case the generated stub returns its failure value. */
int  arpc_begin(arpc_call *c, const char *method);
void arpc_put_str(arpc_call *c, const char *s);
void arpc_put_i64(arpc_call *c, long long v);
void arpc_put_handle(arpc_call *c, uint64_t id);
int  arpc_invoke(arpc_call *c);
void arpc_end(arpc_call *c);

long long arpc_ret_i64(arpc_call *c);
uint64_t  arpc_ret_handle(arpc_call *c);
long long arpc_out_i64(arpc_call *c, const char *name);

/* Borrowed return: libalpm's contract is that the string stays valid as long
 * as the owning object does, so it is cached against `owner` and released
 * when that owner is. */
const char *arpc_intern_str(arpc_call *c, uint64_t owner, const char *key);
/* Borrowed, but owned by nobody and varying with the arguments --
 * alpm_strerror. libalpm's are static strings, so these are kept for good,
 * one copy per distinct value. */
const char *arpc_intern_static(arpc_call *c);
/* Caller-owned return: a plain malloc'd copy the caller frees, matching the
 * contract of the three libalpm functions that return char*. */
char *arpc_take_str(arpc_call *c);

/* Emitted for overlay-declared creates/destroys, so the connection lives
 * exactly as long as the caller's libalpm handles do. */
void arpc_conn_ref(void);
void arpc_conn_unref(void);

/* What a call did to the caches, emitted after it by kind:
 *   - a handle released: everything cached under it or anything below it
 *     goes, along with its package lists' column caches and its callbacks.
 *     The root in every id (ARPC_ROOT) is what makes "below it" answerable
 *     here without knowing the tree in between;
 *   - a package or db freed: what was cached under that one object goes;
 *   - anything that changes something -- a setter, an add, a transaction:
 *     exactly the results it can have changed, named by the overlay, are
 *     detached, so the next read of one fetches afresh -- but not freed,
 *     because after an append the old list is still valid memory natively
 *     and a caller may be walking it. They go when the handle does.
 * `keys` is a NULL-terminated list of getter names; an entry keyed on a
 * getter and its arguments counts for that getter. Under `owner` alone, or
 * under everything with its root. A column is one batched package field a
 * setter changed, for one package. */
void arpc_purge_root(uint64_t handle);
void arpc_purge_owner(uint64_t owner);
void arpc_detach(uint64_t owner, int whole_root, const char *const *keys);
void arpc_drop_column(uint64_t pkg, const char *field);

/* For the tests: how many cache entries and package groups are live,
 * detached ones included. Zero after the last handle is released. */
size_t arpc_stats_cached(void);

/* ---- lists ----
 *
 * alpm_list_t is transparent: callers walk ->next and call alpm_list_count,
 * alpm_list_free and friends directly. So a list cannot be proxied -- the
 * client materialises a real chain from one array on the wire, which also
 * makes a list cost one round trip instead of one per element.
 */

/* Raw response access, for generated materialisers. */
const aj_doc *arpc_doc(const arpc_call *c);
int           arpc_ret_node(const arpc_call *c);
int           arpc_out_node(const arpc_call *c, const char *name);

/* strdup that tolerates NULL, matching libalpm's use of NULL for absent. */
char *arpc_dup(const char *s);

void arpc_free_list(alpm_list_t *l, alpm_list_fn_free elem_free);

/* Borrowed lists and structs are cached against their owner, because libalpm
 * returns the same pointer for repeated calls and callers may still hold an
 * earlier one. The lookup happens before the call, so a repeat costs no
 * round trip. A cached NULL is an answer -- an empty list -- not a miss, so
 * the lookup says whether it found one rather than what. Caching hands back
 * the value the cache holds: the one just made, unless a callback fetched
 * the same thing while the call was in flight, in which case the callback's,
 * and the newcomer is released -- a repeat has to be the same pointer even
 * then. `release` frees a value of this kind, and is what the entry uses
 * when its owner goes. */
int   arpc_cached(uint64_t owner, const char *key, void **out);
void *arpc_cache(uint64_t owner, const char *key, void *value,
                 void (*release)(void *));

/* Raw request-writer access, for generated struct serialisers. */
void arpc_put_null(arpc_call *c);
void arpc_obj_begin(arpc_call *c);
void arpc_obj_end(arpc_call *c);
void arpc_key(arpc_call *c, const char *key);

/* Bytes rather than text -- a signature, a changelog chunk. A JSON string
 * stops at the first NUL, so these travel base64; see arpc_b64.h. The encode
 * and its free happen inside arpc_put_bytes so a generated stub has no
 * temporary to clean up on its failure paths. arpc_out_bytes hands back a
 * malloc'd buffer, or NULL if the field was null or malformed. */
void arpc_put_bytes(arpc_call *c, const unsigned char *b, size_t n);
unsigned char *arpc_out_bytes(arpc_call *c, const char *name, size_t *n);
unsigned char *arpc_ret_bytes(arpc_call *c, size_t *n);

/* List parameters, serialised from the caller's own list. */
void arpc_put_str_list(arpc_call *c, const alpm_list_t *l);
void arpc_put_handle_list(arpc_call *c, const alpm_list_t *l);

/* ---- batched package fields ----
 *
 * Reading eight fields of 1150 packages one accessor call at a time is 9200
 * round trips. Instead a materialised package list registers itself as a
 * group, and the first read of any field fetches that field for the whole
 * group in one call. Fields nobody reads are never fetched.
 *
 * A package that belongs to no group (one from alpm_db_get_pkg, say) gets a
 * group of one, so the same path also gives it per-field caching.
 */
void arpc_pkg_group_register(const alpm_list_t *pkgs);
const char *arpc_pkg_field_str(uint64_t id, const char *field);
long long   arpc_pkg_field_i64(uint64_t id, const char *field);

/* ---- callbacks ----
 *
 * A callback fires on the server, inside a call this client is waiting on,
 * and is answered before that call can continue. arpc_invoke's frame loop
 * hands each one here. */
void arpc_dispatch_callback(const aj_doc *d, aj_w *reply);
void arpc_callbacks_purge(uint64_t handle);

/* The setters, getters and dispatchers are generated, one per callback.
 * What stays here is the registry of the caller's own function pointers --
 * which never leave this process -- and telling the server whether to
 * install a trampoline at all.
 *
 * Function pointers are held as void (*)(void) rather than void *: any
 * function pointer converts to any other and back, which is not true of an
 * object pointer. */
int   arpc_cb_register(uint64_t handle, int kind, const char *wire,
                       void (*fn)(void), void *ctx);
void (*arpc_cb_fn(uint64_t handle, int kind))(void);
void *arpc_cb_ctx(uint64_t handle, int kind);

/* alpm_cb_log takes a va_list and there is no portable way to build one
 * except by being variadic, so the one trampoline that needs it lives here.
 * The server already formatted the text; this hands it over as a literal
 * format string, which is what it would have produced anyway. */
void arpc_cb_log_via(void (*fn)(void), void *ctx, int level, const char *msg);

typedef struct {
	const char *name;
	long long (*call)(void (*fn)(void), void *ctx, const aj_doc *d,
	                  int args);
} arpc_cb_kind;

extern const arpc_cb_kind arpc_cb_kinds[];

/* Diagnostics. ALPMRPC_TRACE=1 dumps every frame to stderr. */
const char *arpc_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
