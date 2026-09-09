/* Framing and endpoint naming shared by the msys server and the mingw client.
 *
 * Wire format: a 4-byte little-endian payload length followed by that many
 * bytes of UTF-8 JSON. The pipe is opened in message mode, so the length
 * prefix is redundant there -- it is kept so the same framing works over a
 * byte stream (a socket, a log replay, a test harness reading from a file).
 */
#ifndef ARPC_WIRE_H
#define ARPC_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible protocol change. It is mixed into the pipe
 * name, so a stale server from an older build is simply never contacted
 * rather than contacted and misunderstood. */
/* 2: the callback wire names are derived from their setters now that both
 * sides generate them, so the download callback travels as "dl" rather than
 * "download". */
/* 3: ids carry their root handle in their high bits, the same object always
 * gets the same id, and an out-list whose element type depends on the errno
 * travels with that errno beside it. */
#define ARPC_PROTO_VERSION 3

/* An id carries the root it belongs to -- the alpm_handle_t at the top of
 * its ownership chain -- in its high bits. Either side can then tell which
 * handle any object belongs to from the id alone, which is what lets the
 * client release a handle's caches without being told the tree in between.
 * The low bits are a global sequence, so ids stay unique and are never
 * reused. Root 0 is nobody's: an object filed there lives until the server
 * exits. */
#define ARPC_ROOT_SHIFT 40
#define ARPC_ROOT(id) ((uint64_t)(id) >> ARPC_ROOT_SHIFT)

/* Refuse absurd frames rather than trying to allocate them. */
#define ARPC_MAX_FRAME (64u * 1024u * 1024u)

/* JSON-RPC-ish error codes. Reusing the standard ones where they fit. */
#define ARPC_E_PARSE          -32700
#define ARPC_E_INVALID_REQ    -32600
#define ARPC_E_NO_METHOD      -32601
#define ARPC_E_INVALID_PARAMS -32602
#define ARPC_E_INTERNAL       -32603

/* ---- identity ----
 *
 * Who a process is, for the purpose of this pipe: the user it runs as and
 * the integrity level it runs at. The second matters because an elevated
 * process and an unelevated one of the same user share a SID, and a bridge
 * that installs packages must not let the unelevated one drive the elevated
 * one. Both go into the endpoint name, so those worlds never meet, and both
 * are checked against the process at the other end before a byte of
 * protocol crosses -- the name is only a rendezvous, not a permission. */
typedef struct {
	char sid[256];          /* the user, as a string SID */
	/* The label's RID: 0x2000 medium, 0x3000 high. Fixed width, because it
	 * goes into the endpoint hash and this header is compiled by both
	 * toolchains -- a long is 8 bytes to Cygwin and 4 to mingw. */
	uint32_t integrity;
} arpc_identity;

/* `process` is a HANDLE open with PROCESS_QUERY_LIMITED_INFORMATION, or NULL
 * for this process. Returns 1 on success. */
int arpc_process_identity(void *process, arpc_identity *out);

/* Whether the process at the other end of `pipe` is `ours`. `server_side`
 * says which end this is, since Windows asks differently. Anything that
 * cannot be established counts as not ours -- a process that cannot even be
 * opened is another user's. */
int arpc_peer_is(void *pipe, int server_side, const arpc_identity *ours);

/* Build the pipe name for a given MSYS2 root, e.g.
 *     \\.\pipe\alpmrpc.3.a3f19c22b40e7761
 * The hash covers the root path, the protocol version and the caller's
 * identity, so separate MSYS2 installs, protocol versions, users and
 * elevations never share an endpoint. Returns 1 on success, and 0 if the
 * identity cannot be read: a name that did not include it would be one
 * that two users could share. */
int arpc_pipe_name(const char *msys_root, char *out, size_t outsz);

/* Cut the last `n` path components off `path` in place, either separator
 * counting: <root>/usr/bin/alpmrpcd.exe and <root>/ucrt64/bin/libalpm-14.dll
 * are both <root> after three. Returns 0 if there were not that many. */
int arpc_strip_dirs(char *path, int n);

/* ---- framing ----
 *
 * Both sides read and write frames the same way, so it is done here, once.
 * The handles are Windows HANDLEs, typed void * so that this header needs no
 * windows.h. `ev` is the event an overlapped handle completes its I/O on,
 * or NULL for a synchronous one: the server's pipe is overlapped so that
 * waiting for a client can time out, and a handle opened that way has to be
 * read and written that way too, while the client's is plain. */
int   arpc_send_frame(void *h, void *ev, const char *buf, size_t len);
/* A malloc'd, NUL-terminated frame with its length in *len, or NULL: with
 * *len 0 when the connection is gone, or the refused size when the frame
 * was empty or larger than ARPC_MAX_FRAME. */
char *arpc_recv_frame(void *h, void *ev, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
