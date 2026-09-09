/* Paths in the caller's form, or the server's.
 *
 * libalpm's paths are the server's: it is an MSYS2 process, and "C:\msys64"
 * is "/" to it. A native caller has no cygwin_conv_path to make that
 * translation with -- it lives in msys-2.0.dll, which the client must never
 * load -- so the server makes it, on request. A connection that asks, with
 * arpc.set_path_style, sends Win32 paths and gets Win32 paths back: every
 * argument, return and field the overlay names as a path passes through here
 * in the direction the wire is carrying it, and nothing else does. Which
 * strings are paths is the overlay's to say -- a package's filename is not
 * one, and neither is a pattern relative to the root -- and the generator
 * checks each name it gives against the header.
 */
#include "arpc_server.h"

#include <alpm_list.h>
#include <sys/cygwin.h>

#include <stdlib.h>
#include <string.h>

/* Per connection: set by arpc.set_path_style, POSIX again when the
 * connection ends. */
static int g_win32;

void arpc_paths_set(int win32)
{
	g_win32 = win32;
}

/* A copy of `s` in the other form, or as it came if it does not convert;
 * NULL for NULL. Relative paths stay relative -- resolving one against this
 * process's directory would be quietly wrong -- so a caller passes absolute
 * ones, as it would natively. */
static char *convert(const char *s, cygwin_conv_path_t what)
{
	if (!s)
		return NULL;
	if (!g_win32 || !*s)
		return strdup(s);
	what |= CCP_RELATIVE;
	ssize_t n = cygwin_conv_path(what, s, NULL, 0);
	char *out = n > 0 ? (char *)malloc((size_t)n) : NULL;
	if (out && cygwin_conv_path(what, s, out, (size_t)n) == 0)
		return out;
	free(out);
	return strdup(s);
}

char *arpc_path_in(const char *s)
{
	return convert(s, CCP_WIN_A_TO_POSIX);
}

char *arpc_path_out(const char *s)
{
	return convert(s, CCP_POSIX_TO_WIN_A);
}

void arpc_ajw_path(aj_w *w, const char *posix)
{
	char *t = arpc_path_out(posix);
	ajw_str(w, t ? t : posix);
	free(t);
}

/* The same shape as the generated list writers: null for no list. */
void arpc_put_path_list(aj_w *w, const alpm_list_t *l)
{
	if (!l) {
		ajw_null(w);
		return;
	}
	ajw_arr_begin(w);
	for (; l; l = l->next)
		arpc_ajw_path(w, (const char *)l->data);
	ajw_arr_end(w);
}

void arpc_paths_in_list(alpm_list_t *l)
{
	for (; l; l = l->next) {
		char *c = arpc_path_in((const char *)l->data);
		if (c) {
			free(l->data);
			l->data = c;
		}
	}
}
