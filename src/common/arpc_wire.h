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

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible protocol change. It is mixed into the pipe
 * name, so a stale server from an older build is simply never contacted
 * rather than contacted and misunderstood. */
#define ARPC_PROTO_VERSION 1

/* Refuse absurd frames rather than trying to allocate them. */
#define ARPC_MAX_FRAME (64u * 1024u * 1024u)

/* JSON-RPC-ish error codes. Reusing the standard ones where they fit. */
#define ARPC_E_PARSE          -32700
#define ARPC_E_INVALID_REQ    -32600
#define ARPC_E_NO_METHOD      -32601
#define ARPC_E_INVALID_PARAMS -32602
#define ARPC_E_INTERNAL       -32603
#define ARPC_E_BAD_HANDLE     -32000

/* Build the pipe name for a given MSYS2 root, e.g.
 *     \\.\pipe\alpmrpc.1.a3f19c22b40e7761
 * The hash covers the root path, the protocol version and the caller's user
 * SID, so separate MSYS2 installs, protocol versions and users never share an
 * endpoint. Returns 1 on success. */
int arpc_pipe_name(const char *msys_root, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif
