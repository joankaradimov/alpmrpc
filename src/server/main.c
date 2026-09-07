/* alpmrpcd -- the MSYS2-side worker.
 *
 * A plain MSYS2 (Cygwin) process. It starts no threads, which is what keeps
 * libalpm's fork() for scriptlets and hooks on the path Cygwin actually
 * supports.
 *
 * Lifetime: started on demand by the client, exits once no client has been
 * connected for --idle seconds. Nothing is registered anywhere; the endpoint
 * name is derived from the MSYS2 root, so client and server find each other
 * without configuration.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>

#include "arpc_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_verbose;

static void logf_(const char *fmt, ...)
{
	if (!g_verbose)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "alpmrpcd: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* <root>/usr/bin/alpmrpcd.exe -> <root> */
static int derive_root(char *out, size_t outsz)
{
	char exe[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
	if (n == 0 || n >= sizeof(exe))
		return 0;
	for (int up = 0; up < 3; up++) {
		char *slash = strrchr(exe, '\\');
		char *fwd = strrchr(exe, '/');
		if (fwd > slash)
			slash = fwd;
		if (!slash)
			return 0;
		*slash = '\0';
	}
	size_t len = strlen(exe);
	if (len + 1 > outsz)
		return 0;
	memcpy(out, exe, len + 1);
	return 1;
}

/* Owner-only DACL. The derived pipe name already includes the user SID, but
 * a name is not a permission -- this is what actually keeps another user on
 * the machine from driving package installs through this pipe. */
static int owner_only_sa(SECURITY_ATTRIBUTES *sa, PSECURITY_DESCRIPTOR *sd_out)
{
	HANDLE tok = NULL;
	char sddl[512];
	LPSTR sidstr = NULL;
	int ok = 0;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
		return 0;

	DWORD need = 0;
	GetTokenInformation(tok, TokenUser, NULL, 0, &need);
	TOKEN_USER *tu = need ? (TOKEN_USER *)LocalAlloc(LPTR, need) : NULL;
	if (tu && GetTokenInformation(tok, TokenUser, tu, need, &need) &&
	    ConvertSidToStringSidA(tu->User.Sid, &sidstr) && sidstr) {
		snprintf(sddl, sizeof(sddl),
			 "D:(A;;GA;;;%s)(A;;GA;;;SY)", sidstr);
		PSECURITY_DESCRIPTOR sd = NULL;
		if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
			    sddl, SDDL_REVISION_1, &sd, NULL)) {
			sa->nLength = sizeof(*sa);
			sa->lpSecurityDescriptor = sd;
			sa->bInheritHandle = FALSE;
			*sd_out = sd;
			ok = 1;
		}
	}
	if (sidstr)
		LocalFree(sidstr);
	if (tu)
		LocalFree(tu);
	CloseHandle(tok);
	return ok;
}

/* ---- framed I/O ---- */

static int read_exact(HANDLE h, void *buf, DWORD n)
{
	char *p = (char *)buf;
	while (n) {
		DWORD got = 0;
		if (!ReadFile(h, p, n, &got, NULL) || got == 0)
			return 0;
		p += got;
		n -= got;
	}
	return 1;
}

static int write_exact(HANDLE h, const void *buf, DWORD n)
{
	const char *p = (const char *)buf;
	while (n) {
		DWORD put = 0;
		if (!WriteFile(h, p, n, &put, NULL) || put == 0)
			return 0;
		p += put;
		n -= put;
	}
	return 1;
}

/* Ask a running server on this endpoint to exit, and wait for it to let go.
 *
 * A rebuild has to be able to replace alpmrpcd.exe, and Windows will not let
 * it while the old one is running. Killing it would be blunt and would drop
 * whatever a client was doing; asking is enough, because the server finishes
 * its current connection first. Returns 0 only if a server was there and
 * would not leave. */
static int stop_running_server(const char *name, int timeout_ms)
{
	HANDLE p = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			       OPEN_EXISTING, 0, NULL);
	if (p == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();
		if (e == ERROR_FILE_NOT_FOUND) {
			logf_("no server listening on %s", name);
			return 1;       /* genuinely nothing to stop */
		}
		/* The server accepts one connection at a time, so a busy pipe
		 * means a server is up and serving somebody. That is the case
		 * the caller most needs told apart from "not running": its
		 * binary is locked and cannot be replaced. */
		if (e == ERROR_PIPE_BUSY) {
			/* Give the current client a moment to finish. */
			if (WaitNamedPipeA(name, (DWORD)timeout_ms)) {
				p = CreateFileA(name,
						GENERIC_READ | GENERIC_WRITE, 0,
						NULL, OPEN_EXISTING, 0, NULL);
			}
			if (p == INVALID_HANDLE_VALUE) {
				logf_("server is busy with another client");
				return 0;
			}
		} else {
			logf_("cannot reach the endpoint (%u)", (unsigned)e);
			return 0;
		}
	}

	static const char req[] =
		"{\"id\":1,\"method\":\"arpc.shutdown\",\"params\":[]}";
	unsigned len = (unsigned)(sizeof(req) - 1);
	unsigned char hdr[4] = {
		(unsigned char)(len & 0xFF), (unsigned char)((len >> 8) & 0xFF),
		(unsigned char)((len >> 16) & 0xFF),
		(unsigned char)((len >> 24) & 0xFF)
	};
	int sent = write_exact(p, hdr, 4) && write_exact(p, (void *)req, len);
	if (sent && read_exact(p, hdr, 4)) {
		unsigned rlen = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
				((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
		char *drop = (rlen && rlen < ARPC_MAX_FRAME)
				     ? (char *)malloc(rlen) : NULL;
		if (drop) {
			read_exact(p, drop, rlen);
			free(drop);
		}
	}
	CloseHandle(p);                 /* our disconnect is what lets it go */
	if (!sent)
		return 0;

	for (int waited = 0; waited < timeout_ms; waited += 25) {
		Sleep(25);
		HANDLE probe = CreateFileA(name, GENERIC_READ, 0, NULL,
					   OPEN_EXISTING, 0, NULL);
		if (probe == INVALID_HANDLE_VALUE) {
			logf_("server stopped");
			return 1;
		}
		CloseHandle(probe);
	}
	logf_("server did not stop within %dms", timeout_ms);
	return 0;
}

/* The live connection, as the callback layer sees it. */
struct arpc_conn {
	HANDLE pipe;
};

static int send_framed(HANDLE pipe, const char *buf, size_t len)
{
	unsigned char hdr[4];
	hdr[0] = (unsigned char)(len & 0xFF);
	hdr[1] = (unsigned char)((len >> 8) & 0xFF);
	hdr[2] = (unsigned char)((len >> 16) & 0xFF);
	hdr[3] = (unsigned char)((len >> 24) & 0xFF);
	return write_exact(pipe, hdr, 4) &&
	       write_exact(pipe, (void *)buf, (DWORD)len);
}

static char *recv_framed(HANDLE pipe)
{
	unsigned char hdr[4];
	if (!read_exact(pipe, hdr, 4))
		return NULL;
	unsigned len = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
		       ((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
	if (len == 0 || len > ARPC_MAX_FRAME)
		return NULL;
	char *buf = (char *)malloc(len + 1);
	if (!buf)
		return NULL;
	if (!read_exact(pipe, buf, len)) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

/* Called from inside a libalpm callback, part-way through serving a request.
 * The client is in its own frame loop and answers this before its original
 * call can return, so a plain send-then-read is the correct shape. */
int arpc_conn_exchange(arpc_conn *c, const char *frame, size_t len,
		       char **reply_out)
{
	*reply_out = NULL;
	if (!c || c->pipe == INVALID_HANDLE_VALUE)
		return 0;
	logf_("cb-> %s", frame);
	if (!send_framed(c->pipe, frame, len))
		return 0;
	char *reply = recv_framed(c->pipe);
	if (!reply)
		return 0;
	logf_("cb<- %s", reply);
	*reply_out = reply;
	return 1;
}

static void serve_connection(HANDLE pipe)
{
	arpc_conn conn = { pipe };
	arpc_cb_set_conn(&conn);
	for (;;) {
		unsigned char lenbuf[4];
		if (!read_exact(pipe, lenbuf, 4))
			break;
		unsigned len = (unsigned)lenbuf[0] | ((unsigned)lenbuf[1] << 8) |
			       ((unsigned)lenbuf[2] << 16) | ((unsigned)lenbuf[3] << 24);
		if (len == 0 || len > ARPC_MAX_FRAME) {
			logf_("refusing frame of %u bytes", len);
			break;
		}

		char *req = (char *)malloc(len + 1);
		if (!req)
			break;
		if (!read_exact(pipe, req, len)) {
			free(req);
			break;
		}
		req[len] = '\0';
		logf_("--> %s", req);

		char *rsp = arpc_handle_frame(req, len);
		free(req);
		if (!rsp)
			break;
		logf_("<-- %s", rsp);

		size_t rlen = strlen(rsp);
		unsigned char out[4];
		out[0] = (unsigned char)(rlen & 0xFF);
		out[1] = (unsigned char)((rlen >> 8) & 0xFF);
		out[2] = (unsigned char)((rlen >> 16) & 0xFF);
		out[3] = (unsigned char)((rlen >> 24) & 0xFF);
		int ok = write_exact(pipe, out, 4) &&
			 write_exact(pipe, rsp, (DWORD)rlen);
		free(rsp);
		if (!ok)
			break;
	}
	arpc_cb_set_conn(NULL);
}

/* Reads framed requests from stdin and writes framed responses to stdout,
 * with no pipe and no client. This is how the dispatch layer gets tested
 * without any IPC in the way -- and how you drive the server by hand. */
static int selftest_stdio(void)
{
	char line[65536];
	while (fgets(line, sizeof(line), stdin)) {
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		if (!n)
			continue;
		char *rsp = arpc_handle_frame(line, n);
		printf("%s\n", rsp ? rsp : "{\"error\":\"internal\"}");
		fflush(stdout);
		free(rsp);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *root_override = NULL;
	int idle_ms = 30000;
	int stdio_mode = 0;
	int stop_mode = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--stdio"))
			stdio_mode = 1;
		else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			g_verbose = 1;
		else if (!strcmp(argv[i], "--root") && i + 1 < argc)
			root_override = argv[++i];
		else if (!strcmp(argv[i], "--idle") && i + 1 < argc)
			idle_ms = atoi(argv[++i]) * 1000;
		else if (!strcmp(argv[i], "--stop"))
			stop_mode = 1;
		else if (!strcmp(argv[i], "--print-endpoint")) {
			char root[MAX_PATH], name[256];
			if (!derive_root(root, sizeof(root)))
				return 1;
			if (!arpc_pipe_name(root, name, sizeof(name)))
				return 1;
			printf("%s\n", name);
			return 0;
		} else {
			fprintf(stderr,
				"usage: alpmrpcd [--stdio] [-v] [--root DIR] "
				"[--idle SECONDS] [--print-endpoint]\n");
			return 2;
		}
	}

	if (stdio_mode)
		return selftest_stdio();

	char root[MAX_PATH];
	if (root_override) {
		snprintf(root, sizeof(root), "%s", root_override);
	} else if (!derive_root(root, sizeof(root))) {
		fprintf(stderr, "alpmrpcd: cannot determine MSYS2 root\n");
		return 1;
	}

	char name[256];
	if (!arpc_pipe_name(root, name, sizeof(name))) {
		fprintf(stderr, "alpmrpcd: cannot derive endpoint name\n");
		return 1;
	}
	logf_("root=%s endpoint=%s idle=%dms", root, name, idle_ms);

	if (stop_mode)
		return stop_running_server(name, 5000) ? 0 : 1;

	SECURITY_ATTRIBUTES sa;
	PSECURITY_DESCRIPTOR sd = NULL;
	SECURITY_ATTRIBUTES *psa = owner_only_sa(&sa, &sd) ? &sa : NULL;
	if (!psa)
		logf_("WARNING: falling back to the default pipe DACL");

	HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!ev)
		return 1;

	int served = 0;
	for (;;) {
		HANDLE pipe = CreateNamedPipeA(
			name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, psa);
		if (pipe == INVALID_HANDLE_VALUE) {
			fprintf(stderr, "alpmrpcd: CreateNamedPipe failed (%u)\n",
				(unsigned)GetLastError());
			return 1;
		}

		OVERLAPPED ov;
		memset(&ov, 0, sizeof(ov));
		ResetEvent(ev);
		ov.hEvent = ev;

		BOOL connected = ConnectNamedPipe(pipe, &ov);
		DWORD err = GetLastError();
		if (!connected && err == ERROR_IO_PENDING) {
			DWORD w = WaitForSingleObject(ev, (DWORD)idle_ms);
			if (w == WAIT_TIMEOUT) {
				CancelIo(pipe);
				CloseHandle(pipe);
				logf_("idle for %dms, exiting (served %d)",
				      idle_ms, served);
				break;
			}
			DWORD got = 0;
			if (!GetOverlappedResult(pipe, &ov, &got, FALSE)) {
				CloseHandle(pipe);
				continue;
			}
		} else if (!connected && err != ERROR_PIPE_CONNECTED) {
			CloseHandle(pipe);
			continue;
		}

		served++;
		logf_("client connected (#%d)", served);
		serve_connection(pipe);
		FlushFileBuffers(pipe);
		DisconnectNamedPipe(pipe);
		CloseHandle(pipe);
		logf_("client gone; %u handles live", arpc_handle_live());
		if (arpc_shutdown_requested()) {
			logf_("shutdown requested; exiting (served %d)", served);
			break;
		}
	}

	CloseHandle(ev);
	if (sd)
		LocalFree(sd);
	arpc_handle_reset();
	return 0;
}
