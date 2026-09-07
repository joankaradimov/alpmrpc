#include "arpc_wire.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>

#include <stdio.h>
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

/* The SID keeps two users on one machine from colliding on a pipe name.
 * It is defence in depth only -- the server also puts an owner-only DACL on
 * the pipe, which is what actually enforces the boundary. */
static int current_user_sid(char *out, size_t outsz)
{
	HANDLE tok = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
		return 0;

	DWORD need = 0;
	GetTokenInformation(tok, TokenUser, NULL, 0, &need);
	if (need == 0) {
		CloseHandle(tok);
		return 0;
	}

	int ok = 0;
	TOKEN_USER *tu = (TOKEN_USER *)LocalAlloc(LPTR, need);
	if (tu && GetTokenInformation(tok, TokenUser, tu, need, &need)) {
		LPSTR s = NULL;
		if (ConvertSidToStringSidA(tu->User.Sid, &s) && s) {
			size_t n = strlen(s);
			if (n < outsz) {
				memcpy(out, s, n + 1);
				ok = 1;
			}
			LocalFree(s);
		}
	}
	if (tu)
		LocalFree(tu);
	CloseHandle(tok);
	return ok;
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

	char sid[256];
	if (current_user_sid(sid, sizeof(sid))) {
		h = fnv1a(h, "|", 1);
		h = fnv1a(h, sid, strlen(sid));
	}

	int n = snprintf(out, outsz, "\\\\.\\pipe\\alpmrpc.%u.%016llx",
			 ARPC_PROTO_VERSION, h);
	return n > 0 && (size_t)n < outsz;
}
