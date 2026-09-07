/* Base64, for the parts of libalpm that deal in bytes rather than text.
 *
 * A signature and a changelog chunk are arbitrary bytes: they contain NULs
 * and byte sequences that are not text in any encoding. A JSON string cannot
 * carry those, so they travel encoded. Nothing else on this wire needs it --
 * package names and paths are text and go as text.
 */
#ifndef ARPC_B64_H
#define ARPC_B64_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `n` bytes to a malloc'd NUL-terminated string; NULL on allocation failure.
 * n == 0 gives "", which decodes back to an empty buffer. */
char *arpc_b64_encode(const unsigned char *src, size_t n);

/* Back to a malloc'd buffer, with its length in *n. Returns NULL and sets
 * *n to 0 if `src` is not well-formed base64 -- padded, no stray characters,
 * no `=` anywhere but the end. Rejecting is the point: a decoder that
 * shrugged at bad input would hand libalpm a buffer of the wrong length. */
unsigned char *arpc_b64_decode(const char *src, size_t *n);

#ifdef __cplusplus
}
#endif

#endif
