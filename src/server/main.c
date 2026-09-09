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
	if (n == 0 || n >= sizeof(exe) || !arpc_strip_dirs(exe, 3))
		return 0;
	size_t len = strlen(exe);
	if (len + 1 > outsz)
		return 0;
	memcpy(out, exe, len + 1);
	return 1;
}

/* Owner-only DACL, and a mandatory label at this process's own integrity.
 * The derived pipe name already includes both, but a name is not a
 * permission: the DACL is what keeps another user on the machine from
 * driving package installs through this pipe, and the label is what keeps
 * an unelevated process of this same user from driving an elevated one.
 * Nobody else is on the list -- not even SYSTEM, which needs nothing from
 * here. */
static int owner_only_sa(const arpc_identity *me, SECURITY_ATTRIBUTES *sa,
			 PSECURITY_DESCRIPTOR *sd_out)
{
	char sddl[512];
	snprintf(sddl, sizeof(sddl), "D:(A;;GA;;;%s)S:(ML;;NW;;;S-1-16-%u)",
		 me->sid, (unsigned)me->integrity);

	PSECURITY_DESCRIPTOR sd = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
		    sddl, SDDL_REVISION_1, &sd, NULL))
		return 0;
	sa->nLength = sizeof(*sa);
	sa->lpSecurityDescriptor = sd;
	sa->bInheritHandle = FALSE;
	*sd_out = sd;
	return 1;
}

/* ---- framed I/O ----
 *
 * The framing is shared with the client (arpc_wire.c). What is this side's
 * own is the event: the pipe is opened overlapped so that waiting for a
 * client can time out, and a handle opened that way has to be read and
 * written that way too -- ReadFile with no OVERLAPPED on one is documented
 * as able to report a read complete that is not. The server is
 * single-threaded and the pipe carries one exchange at a time, so one event
 * serves every transfer. */

static HANDLE g_io_ev;

static int send_framed(HANDLE pipe, const char *buf, size_t len)
{
	return arpc_send_frame(pipe, g_io_ev, buf, len);
}

static char *recv_framed(HANDLE pipe)
{
	size_t len = 0;
	char *f = arpc_recv_frame(pipe, g_io_ev, &len);
	if (!f && len)
		logf_("refusing frame of %zu bytes", len);
	return f;
}

/* Ask every server on this endpoint to exit, and wait for them to let go.
 *
 * A rebuild has to be able to replace alpmrpcd.exe, and Windows will not let
 * it while one is running. Killing it would be blunt and would drop whatever
 * a client was doing; asking is enough, because a server finishes its
 * current connection first. Two clients at once mean two servers, each with
 * its own instance of the pipe, and stopping one leaves the other holding
 * the binary -- so this keeps asking until nobody answers. Returns 0 only if
 * a server was there and would not leave. */
static int stop_running_servers(const char *name, int timeout_ms,
				const arpc_identity *me)
{
	int stopped = 0;
	for (int waited = 0; waited < timeout_ms; waited += 25) {
		/* A client here, so the client's rules: nothing but an
		 * anonymous view of this process for the other end, and the
		 * other end has to be this user's. See the client's try_open. */
		HANDLE p = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0,
				       NULL, OPEN_EXISTING,
				       FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT
				       | SECURITY_ANONYMOUS, NULL);
		if (p != INVALID_HANDLE_VALUE && !arpc_peer_is(p, 0, me)) {
			CloseHandle(p);
			fprintf(stderr, "alpmrpcd: %s is held by a process that "
				"is not this user's at this elevation\n", name);
			return 0;
		}
		if (p == INVALID_HANDLE_VALUE) {
			DWORD e = GetLastError();
			if (e == ERROR_FILE_NOT_FOUND) {
				if (stopped)
					logf_("stopped %d server%s", stopped,
					      stopped == 1 ? "" : "s");
				else
					logf_("no server listening on %s",
					      name);
				return 1;
			}
			/* A server accepts one connection at a time, so a
			 * busy pipe means one is up and serving somebody.
			 * That is the case the caller most needs told apart
			 * from "not running": its binary is locked. Give
			 * that client a moment to finish. */
			if (e != ERROR_PIPE_BUSY) {
				logf_("cannot reach the endpoint (%u)",
				      (unsigned)e);
				return 0;
			}
			Sleep(25);
			continue;
		}

		/* The process at the other end, so that its exit can be
		 * waited for: the binary is free once the process is gone,
		 * not once the pipe is, and a Cygwin process takes its time
		 * between the two. */
		DWORD pid = 0;
		HANDLE proc = NULL;
		if (GetNamedPipeServerProcessId(p, &pid))
			proc = OpenProcess(SYNCHRONIZE, FALSE, pid);

		static const char req[] =
			"{\"id\":1,\"method\":\"arpc.shutdown\",\"params\":[]}";
		int sent = send_framed(p, req, sizeof(req) - 1);
		if (sent)
			free(recv_framed(p));
		CloseHandle(p);         /* our disconnect is what lets it go */
		if (!sent) {
			if (proc)
				CloseHandle(proc);
			return 0;
		}
		stopped++;
		if (proc) {
			DWORD w = WaitForSingleObject(
				proc, (DWORD)(timeout_ms - waited));
			CloseHandle(proc);
			if (w != WAIT_OBJECT_0) {
				logf_("server %u did not exit within %dms",
				      (unsigned)pid, timeout_ms);
				return 0;
			}
		} else {
			Sleep(100);
		}
	}
	logf_("a server is still busy with a client after %dms", timeout_ms);
	return 0;
}

/* The live connection, as the callback layer sees it. */
struct arpc_conn {
	HANDLE pipe;
};

/* Called from inside a libalpm callback, part-way through serving a request.
 *
 * What comes back is not always the answer. A callback is entitled to ask
 * libalpm something before it can decide -- pacman's own conflict prompt
 * calls alpm_pkg_get_name() on both packages to say which they are -- and on
 * this side that arrives as an ordinary request while the callback is still
 * outstanding. So this serves whatever requests turn up and keeps waiting,
 * which is the mirror of the frame loop the client runs for a callback
 * arriving while a call is outstanding. Reading one frame and calling it the
 * answer looked right until a callback did any work, and then it desynced
 * the pipe: the request was consumed as the answer, and every frame after it
 * was one out of step.
 *
 * The nesting is strict, which is what makes "the next frame that is not a
 * request" the right rule. Each exchange returns only once its own answer
 * arrives, so answers unwind innermost first. */
int arpc_conn_exchange(arpc_conn *c, const char *frame, size_t len,
		       char **reply_out)
{
	*reply_out = NULL;
	if (!c || c->pipe == INVALID_HANDLE_VALUE)
		return 0;
	logf_("cb-> %s", frame);
	if (!send_framed(c->pipe, frame, len))
		return 0;

	for (;;) {
		char *f = recv_framed(c->pipe);
		if (!f)
			return 0;

		/* A request carries a method; an answer does not. Parsed
		 * once, here, and handed on parsed. */
		aj_doc d;
		if (!aj_parse(&d, f, strlen(f)) ||
		    aj_member(&d, 0, "method") < 0) {
			aj_free(&d);
			logf_("cb<- %s", f);
			*reply_out = f;
			return 1;
		}

		logf_("--> nested %s", f);
		free(f);                /* the document copied what it kept */
		char *rsp = arpc_handle_parsed(&d);
		if (!rsp)
			return 0;
		logf_("<-- nested %s", rsp);
		int sent = send_framed(c->pipe, rsp, strlen(rsp));
		free(rsp);
		if (!sent)
			return 0;
	}
}

static void serve_connection(HANDLE pipe)
{
	arpc_conn conn = { pipe };
	arpc_cb_set_conn(&conn);
	arpc_paths_set(0);              /* until this connection says otherwise */
	for (;;) {
		char *req = recv_framed(pipe);
		if (!req)
			break;
		logf_("--> %s", req);

		char *rsp = arpc_handle_frame(req, strlen(req));
		free(req);
		if (!rsp)
			break;
		logf_("<-- %s", rsp);

		int ok = send_framed(pipe, rsp, strlen(rsp));
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
				"[--idle SECONDS] [--print-endpoint] [--stop]\n");
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

	arpc_identity me;
	if (!arpc_process_identity(NULL, &me)) {
		fprintf(stderr, "alpmrpcd: cannot tell which user this is\n");
		return 1;
	}

	char name[256];
	if (!arpc_pipe_name(root, name, sizeof(name))) {
		fprintf(stderr, "alpmrpcd: cannot derive endpoint name\n");
		return 1;
	}
	logf_("root=%s endpoint=%s idle=%dms user=%s integrity=0x%x", root,
	      name, idle_ms, me.sid, (unsigned)me.integrity);

	g_io_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!g_io_ev)
		return 1;

	if (stop_mode)
		return stop_running_servers(name, 5000, &me) ? 0 : 1;

	SECURITY_ATTRIBUTES sa;
	PSECURITY_DESCRIPTOR sd = NULL;
	if (!owner_only_sa(&me, &sa, &sd)) {
		/* The name has the user's SID in it, but a name is not a
		 * permission. Without the DACL the pipe would take anyone's
		 * package installs, so it is not opened at all. */
		fprintf(stderr, "alpmrpcd: cannot build an owner-only DACL for "
			"the pipe (%u); not serving\n",
			(unsigned)GetLastError());
		return 1;
	}

	HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!ev)
		return 1;

	int served = 0;
	for (;;) {
		HANDLE pipe = CreateNamedPipeA(
			name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
		if (pipe == INVALID_HANDLE_VALUE) {
			DWORD e = GetLastError();
			/* Another instance of this name exists and its owner
			 * did not grant this process the right to add one:
			 * somebody else's pipe, under our name. */
			fprintf(stderr, e == ERROR_ACCESS_DENIED
				? "alpmrpcd: %s is held by another user\n"
				: "alpmrpcd: CreateNamedPipe %s failed (%u)\n",
				name, (unsigned)e);
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

		/* The DACL should already have kept anyone else out. Checked
		 * again here, against the process at the other end, because
		 * this is the boundary that matters and a DACL is one line of
		 * SDDL away from being wrong. */
		if (!arpc_peer_is(pipe, 1, &me)) {
			logf_("refusing a connection from a process that is "
			      "not this user's at this elevation");
			DisconnectNamedPipe(pipe);
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
	CloseHandle(g_io_ev);
	LocalFree(sd);
	arpc_handle_reset();
	return 0;
}
