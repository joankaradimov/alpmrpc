/* The form paths come back in is decided when the DLL is built --
 * ALPMRPC_WIN32_PATHS, which the tests are compiled with too -- so a test
 * that looks at one checks for the form that was built. What the tests pass
 * in need not change with it: the server's converter leaves an already-POSIX
 * path alone, so "/" names the server's root under either build.
 */
#ifndef ALPMRPC_TESTS_PATH_FORM_H
#define ALPMRPC_TESTS_PATH_FORM_H

#ifdef ALPMRPC_WIN32_PATHS
#define PATH_FORM "Win32"
/* A drive letter, a colon and a separator: what no POSIX path starts with. */
static int path_in_built_form(const char *p)
{
	return p && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))
	       && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}
#else
#define PATH_FORM "POSIX"
static int path_in_built_form(const char *p)
{
	return p && p[0] == '/';
}
#endif

#endif
