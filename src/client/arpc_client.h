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

/* Diagnostics. ALPMRPC_TRACE=1 dumps every frame to stderr. */
const char *arpc_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
