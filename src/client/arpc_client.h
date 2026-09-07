/* Client-side runtime that generated stubs are written against.
 *
 * Runs inside an arbitrary mingw/clang process, so it depends on nothing but
 * kernel32 and advapi32. It never loads anything from the MSYS2 tree into
 * this process -- the only contact with MSYS2 is a pipe.
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
	int result;             /* node index of "result", or -1 */
	int parsed;
	int held_lock;
} arpc_call;

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
/* Caller-owned return: a plain malloc'd copy the caller frees, matching the
 * contract of the three libalpm functions that return char*. */
char *arpc_take_str(arpc_call *c);

/* Emitted for overlay-declared creates/destroys, so the connection lives
 * exactly as long as the caller's libalpm handles do. */
void arpc_conn_ref(void);
void arpc_conn_unref(void);
void arpc_purge_owner(uint64_t owner);

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

/* strdup that tolerates NULL, matching libalpm's use of NULL for absent. */
char *arpc_dup(const char *s);

void arpc_free_list(alpm_list_t *l, alpm_list_fn_free elem_free);

/* Borrowed lists are cached against their owner, because libalpm returns the
 * same pointer for repeated calls and callers may still hold an earlier one.
 * The lookup happens before the call, so a repeat costs no round trip. */
alpm_list_t *arpc_cached_list(uint64_t owner, const char *key);
void arpc_cache_list(uint64_t owner, const char *key, alpm_list_t *list,
                     alpm_list_fn_free elem_free);

/* Raw request-writer access, for generated struct serialisers. */
void arpc_put_null(arpc_call *c);
void arpc_obj_begin(arpc_call *c);
void arpc_obj_end(arpc_call *c);
void arpc_key(arpc_call *c, const char *key);

/* A borrowed struct return, cached against its owner exactly like a borrowed
 * list: same lifetime rule, same pre-call lookup. */
void *arpc_cached_ptr(uint64_t owner, const char *key);
void  arpc_cache_ptr(uint64_t owner, const char *key, void *ptr,
                     void (*release)(void *));

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

/* Diagnostics. ALPMRPC_TRACE=1 dumps every frame to stderr. */
const char *arpc_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
