/* Minimal JSON reader/writer for the alpm-rpc wire protocol.
 *
 * Deliberately dependency-free: the client half of this project ships as a
 * drop-in libalpm shim into an unrelated process, so it must not drag a JSON
 * library (or anything else) in with it. Scope is the full JSON grammar we
 * actually put on the wire -- objects, arrays, strings, integers, null, bool.
 * Floating point is not used by the protocol and is not accepted.
 */
#ifndef ARPC_JSON_H
#define ARPC_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- reader ---- */

typedef enum {
	AJ_NULL, AJ_BOOL, AJ_NUM, AJ_STR, AJ_ARR, AJ_OBJ
} aj_type;

typedef struct {
	aj_type type;
	long long num;          /* AJ_NUM, AJ_BOOL */
	const char *str;        /* AJ_STR: NUL-terminated, owned by aj_doc */
	const char *key;        /* member name when inside an object, else NULL */
	int first_child;        /* AJ_ARR/AJ_OBJ: index into doc->nodes, or -1 */
	int next_sibling;       /* index into doc->nodes, or -1 */
} aj_node;

typedef struct {
	aj_node *nodes;
	int count, cap;
	char *sbuf;             /* decoded string storage */
	size_t slen, scap;
	int ok;
} aj_doc;

/* Parse `text` (need not be NUL-terminated). Returns 1 on success.
 * Node 0 is the root. Call aj_free() either way. */
int  aj_parse(aj_doc *d, const char *text, size_t len);
void aj_free(aj_doc *d);

/* -1 when absent or when the parent is the wrong container type. */
int  aj_member(const aj_doc *d, int obj, const char *key);
int  aj_elem(const aj_doc *d, int arr, int index);
int  aj_count(const aj_doc *d, int container);

/* Iterate a container. aj_elem() is O(n) -- it walks from the start -- so a
 * loop over it is O(n^2); at 1214 elements that measured 918us against 1.6us
 * for these. Prefer them for anything that visits every element:
 *     for (int e = aj_first(d, arr); e >= 0; e = aj_next(d, e))
 */
int  aj_first(const aj_doc *d, int container);
int  aj_next(const aj_doc *d, int node);

/* Typed accessors; each takes a default used when the node is missing or
 * of the wrong type, so callers need no separate presence check. */
long long   aj_i64(const aj_doc *d, int node, long long dflt);
const char *aj_str(const aj_doc *d, int node, const char *dflt);
int         aj_is_null(const aj_doc *d, int node);

/* ---- writer ---- */

typedef struct {
	char *buf;
	size_t len, cap;
	int err;                /* sticky: set on allocation failure */
	int need_comma;
} aj_w;

void ajw_init(aj_w *w);
void ajw_free(aj_w *w);

/* Pre-size the buffer when the caller already knows roughly how much is
 * coming, so a large document does not walk up through every power of two.
 * Returns 0 on allocation failure (w->err is then set). */
int  ajw_reserve(aj_w *w, size_t bytes);

/* Append already-formatted JSON verbatim: no escaping, no separator logic.
 * For splicing one writer's output into another. */
void ajw_raw(aj_w *w, const char *s, size_t n);

void ajw_obj_begin(aj_w *w);
void ajw_obj_end(aj_w *w);
void ajw_arr_begin(aj_w *w);
void ajw_arr_end(aj_w *w);
void ajw_key(aj_w *w, const char *key);
void ajw_str(aj_w *w, const char *s);   /* NULL emits null */
void ajw_i64(aj_w *w, long long v);
void ajw_null(aj_w *w);

#ifdef __cplusplus
}
#endif

#endif
