#include "arpc_wire.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long fnv1a(unsigned long long h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ULL;
	}
	return h;
}

/* ---- identity ---- */

/* One piece of token information, in a LocalAlloc'd buffer the caller
 * frees, or NULL. */
static void *token_info(HANDLE tok, TOKEN_INFORMATION_CLASS what)
{
	DWORD need = 0;
	GetTokenInformation(tok, what, NULL, 0, &need);
	void *buf = need ? LocalAlloc(LPTR, need) : NULL;
	if (buf && !GetTokenInformation(tok, what, buf, need, &need)) {
		LocalFree(buf);
		buf = NULL;
	}
	return buf;
}

int arpc_process_identity(void *process, arpc_identity *out)
{
	memset(out, 0, sizeof(*out));
	HANDLE tok = NULL;
	if (!OpenProcessToken(process ? (HANDLE)process : GetCurrentProcess(),
			      TOKEN_QUERY, &tok))
		return 0;

	int ok = 0;
	TOKEN_USER *tu = (TOKEN_USER *)token_info(tok, TokenUser);
	if (tu) {
		LPSTR s = NULL;
		if (ConvertSidToStringSidA(tu->User.Sid, &s) && s) {
			size_t n = strlen(s);
			if (n < sizeof(out->sid)) {
				memcpy(out->sid, s, n + 1);
				ok = 1;
			}
			LocalFree(s);
		}
		LocalFree(tu);
	}

	if (ok) {
		ok = 0;
		TOKEN_MANDATORY_LABEL *tml =
			(TOKEN_MANDATORY_LABEL *)token_info(tok, TokenIntegrityLevel);
		if (tml) {
			PSID sid = tml->Label.Sid;
			DWORD last = *GetSidSubAuthorityCount(sid) - 1;
			out->integrity = (uint32_t)*GetSidSubAuthority(sid, last);
			ok = 1;
			LocalFree(tml);
		}
	}
	CloseHandle(tok);
	return ok;
}

int arpc_peer_is(void *pipe, int server_side, const arpc_identity *ours)
{
	DWORD pid = 0;
	BOOL got = server_side
		? GetNamedPipeClientProcessId((HANDLE)pipe, &pid)
		: GetNamedPipeServerProcessId((HANDLE)pipe, &pid);
	if (!got)
		return 0;
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return 0;
	arpc_identity theirs;
	int same = arpc_process_identity(proc, &theirs) &&
		   strcmp(theirs.sid, ours->sid) == 0 &&
		   theirs.integrity == ours->integrity;
	CloseHandle(proc);
	return same;
}

int arpc_pipe_name(const char *msys_root, char *out, size_t outsz)
{
	if (!msys_root || !out)
		return 0;

	/* Case- and separator-insensitive: C:\dev\msys64 and c:/dev/msys64
	 * must hash the same, or client and server miss each other. */
	unsigned long long h = 14695981039346656037ULL;
	for (const char *p = msys_root; *p; p++) {
		char c = *p;
		if (c == '/')
			c = '\\';
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		h = fnv1a(h, &c, 1);
	}

	unsigned ver = ARPC_PROTO_VERSION;
	h = fnv1a(h, "|", 1);
	h = fnv1a(h, &ver, sizeof(ver));

	/* The user and the elevation: separate worlds, separate endpoints.
	 * Without them there is no name, since one without them would be a
	 * name two users could share. */
	arpc_identity me;
	if (!arpc_process_identity(NULL, &me))
		return 0;
	h = fnv1a(h, "|", 1);
	h = fnv1a(h, me.sid, strlen(me.sid));
	h = fnv1a(h, "|", 1);
	h = fnv1a(h, &me.integrity, sizeof(me.integrity));

	int n = snprintf(out, outsz, "\\\\.\\pipe\\alpmrpc.%u.%016llx",
			 ARPC_PROTO_VERSION, h);
	return n > 0 && (size_t)n < outsz;
}

int arpc_strip_dirs(char *path, int n)
{
	for (; n > 0; n--) {
		char *a = strrchr(path, '\\');
		char *b = strrchr(path, '/');
		if (b > a)
			a = b;
		if (!a)
			return 0;
		*a = '\0';
	}
	return 1;
}

/* ---- framing ---- */

static int io_exact(void *h, void *ev, void *buf, unsigned n, int writing)
{
	char *p = (char *)buf;
	while (n) {
		DWORD did = 0;
		BOOL ok;
		if (ev) {
			/* The synchronous shape of an overlapped I/O: start it,
			 * then wait on its event for the result. */
			OVERLAPPED ov;
			memset(&ov, 0, sizeof(ov));
			ov.hEvent = (HANDLE)ev;
			ok = writing ? WriteFile((HANDLE)h, p, n, NULL, &ov)
				     : ReadFile((HANDLE)h, p, n, NULL, &ov);
			if (!ok && GetLastError() != ERROR_IO_PENDING)
				return 0;
			ok = GetOverlappedResult((HANDLE)h, &ov, &did, TRUE);
		} else {
			ok = writing ? WriteFile((HANDLE)h, p, n, &did, NULL)
				     : ReadFile((HANDLE)h, p, n, &did, NULL);
		}
		if (!ok || did == 0)
			return 0;
		p += did;
		n -= did;
	}
	return 1;
}

int arpc_send_frame(void *h, void *ev, const char *buf, size_t len)
{
	unsigned char hdr[4];
	hdr[0] = (unsigned char)(len & 0xFF);
	hdr[1] = (unsigned char)((len >> 8) & 0xFF);
	hdr[2] = (unsigned char)((len >> 16) & 0xFF);
	hdr[3] = (unsigned char)((len >> 24) & 0xFF);
	return io_exact(h, ev, hdr, 4, 1) &&
	       io_exact(h, ev, (void *)buf, (unsigned)len, 1);
}

char *arpc_recv_frame(void *h, void *ev, size_t *len_out)
{
	*len_out = 0;
	unsigned char hdr[4];
	if (!io_exact(h, ev, hdr, 4, 0))
		return NULL;
	unsigned len = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
		       ((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
	if (len == 0 || len > ARPC_MAX_FRAME) {
		*len_out = len;         /* refused, and the caller can say so */
		return NULL;
	}
	char *buf = (char *)malloc(len + 1);
	if (!buf)
		return NULL;
	if (!io_exact(h, ev, buf, len, 0)) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	*len_out = len;
	return buf;
}
