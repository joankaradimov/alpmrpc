/* The bridge's own API, beyond libalpm's: what a caller can ask of this DLL
 * that the real library has no notion of. A program that never includes
 * this header is none the worse; it gets libalpm's behaviour exactly.
 */
#ifndef ALPMRPC_H
#define ALPMRPC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Paths in Win32 form rather than the server's: "C:\msys64\var\lib\pacman"
 * rather than "/var/lib/pacman".
 *
 * libalpm's paths are the server's, because the server is an MSYS2 process,
 * and the translation between the two forms lives in msys-2.0.dll, which
 * this DLL must never load into its caller. So the server translates, on
 * request. With this on, every path the caller passes is taken as Win32 and
 * every path it gets back -- the root, the cachedirs, a file conflict's
 * file, a fetch callback's destination -- comes back as Win32. Which strings
 * are paths is stated per function in the generator's overlay; a package's
 * filename and a pattern relative to the root are not among them. Paths
 * should be absolute, as they should be natively.
 *
 * Best called before the first libalpm call. Called later it takes effect
 * at once, and whatever was cached in the old form is fetched again in the
 * new. ALPMRPC_PATHS=win32 in the environment does the same for a program
 * that cannot be changed. Returns 0, or -1 if the server could not be told.
 */
int alpmrpc_win32_paths(int enable);

#ifdef __cplusplus
}
#endif

#endif
