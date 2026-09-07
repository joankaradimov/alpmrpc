/* Server-side runtime that generated dispatch code is written against.
 *
 * Everything here is called only from arpc_dispatch.c (generated) and
 * main.c. Keeping the surface small is deliberate: it is the contract
 * emit.py targets, so changing it means changing the emitter.
 */
#ifndef ARPC_SERVER_H
#define ARPC_SERVER_H

#include "arpc_json.h"
#include "arpc_wire.h"
#include "arpc_handle_tags.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- request ---- */

typedef struct {
	const aj_doc *doc;
	int params;             /* node index of the params array */
	int bad;                /* sticky: an accessor saw the wrong type */
} arpc_req;

/* Each returns a benign default and sets rq->bad on type mismatch, so a
 * handler can pull every argument and check once. */
const char *arpc_arg_str(arpc_req *rq, int i);
long long   arpc_arg_i64(arpc_req *rq, int i);
void       *arpc_arg_handle(arpc_req *rq, int i, arpc_handle_tag tag);
uint64_t    arpc_arg_id(arpc_req *rq, int i);
int         arpc_req_bad(const arpc_req *rq);
void        arpc_req_mark_bad(arpc_req *rq);

/* Raw node access. Generated list code walks the request tree directly
 * rather than going through a typed accessor per element. */
int         arpc_arg_node(arpc_req *rq, int i);
int         arpc_node_is_null(const arpc_req *rq, int n);
int         arpc_node_count(const arpc_req *rq, int n);
int         arpc_node_elem(const arpc_req *rq, int arr, int k);
int         arpc_node_member(const arpc_req *rq, int obj,
                             const char *key);
long long   arpc_node_i64(const arpc_req *rq, int n);
char       *arpc_node_strdup(const arpc_req *rq, int n);

/* ---- response ---- */

typedef struct {
	aj_w out;               /* the "result" object, built incrementally */
	int has_ret;
	int failed;
	int code;
	char msg[256];
} arpc_res;

void arpc_ret_null(arpc_res *rs);
void arpc_ret_i64(arpc_res *rs, long long v);
void arpc_ret_str(arpc_res *rs, const char *s);
void arpc_ret_handle(arpc_res *rs, uint64_t id);
void arpc_out_i64(arpc_res *rs, const char *name, long long v);

/* Open the "ret" slot and hand back the writer, so generated code can emit a
 * composite value (an array, an object) straight into the response instead of
 * building it somewhere else and copying it in. */
void  arpc_ret_begin(arpc_res *rs);
aj_w *arpc_res_writer(arpc_res *rs);
int  arpc_fail(arpc_res *rs, int code, const char *msg);

/* ---- handle table ----
 *
 * Ids are monotonic and never reused, so a stale id can never alias a live
 * object -- it simply misses. Lookup is an open-addressed hash, not a linear
 * scan: a full pkgcache walk puts thousands of live handles in this table.
 */
uint64_t arpc_handle_put(void *ptr, arpc_handle_tag tag, uint64_t owner);
void    *arpc_handle_get(uint64_t id, arpc_handle_tag tag);
uint64_t arpc_owner_of(uint64_t id);
void     arpc_handle_drop(uint64_t id);
/* Drops `owner` and everything it owns. Called when an alpm_handle_t is
 * released, since every db/pkg derived from it dies with it. */
void     arpc_handle_drop_owner(uint64_t owner);
void     arpc_handle_reset(void);
unsigned arpc_handle_live(void);

/* ---- dispatch (table is generated) ---- */

typedef int (*arpc_handler)(arpc_req *, arpc_res *);

typedef struct {
	const char *name;
	arpc_handler fn;
} arpc_method;

extern const arpc_method arpc_methods[];

/* Set once a client has asked the server to exit. The accept loop checks it
 * after each connection closes, so an in-flight call always completes. */
int arpc_shutdown_requested(void);

/* Parses one request frame and produces one response frame.
 * Returns a malloc'd NUL-terminated JSON response; caller frees. */
char *arpc_handle_frame(const char *req, size_t len);

#ifdef __cplusplus
}
#endif

#endif
