#include "arpc_b64.h"

#include <stdlib.h>
#include <string.h>

static const char B64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *arpc_b64_encode(const unsigned char *src, size_t n)
{
	size_t out_len = ((n + 2) / 3) * 4;
	char *out = (char *)malloc(out_len + 1);
	if (!out)
		return NULL;

	size_t i = 0, o = 0;
	for (; i + 3 <= n; i += 3) {
		unsigned v = ((unsigned)src[i] << 16)
			   | ((unsigned)src[i + 1] << 8)
			   | (unsigned)src[i + 2];
		out[o++] = B64[(v >> 18) & 63];
		out[o++] = B64[(v >> 12) & 63];
		out[o++] = B64[(v >> 6) & 63];
		out[o++] = B64[v & 63];
	}
	if (i < n) {
		size_t rem = n - i;
		unsigned v = (unsigned)src[i] << 16;
		if (rem == 2)
			v |= (unsigned)src[i + 1] << 8;
		out[o++] = B64[(v >> 18) & 63];
		out[o++] = B64[(v >> 12) & 63];
		out[o++] = rem == 2 ? B64[(v >> 6) & 63] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
	return out;
}

/* -1 for anything that is not a base64 digit, `=` included: padding is
 * handled by position, not by decoding to a value. */
static int b64val(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

unsigned char *arpc_b64_decode(const char *src, size_t *n)
{
	*n = 0;
	if (!src)
		return NULL;

	size_t len = strlen(src);
	if (len % 4)
		return NULL;

	/* Padding runs to the very end or it is not padding. Counting `=`
	 * without checking that would let "AB=C" through as two bytes. */
	size_t pad = 0;
	if (len) {
		if (src[len - 1] == '=')
			pad = src[len - 2] == '=' ? 2 : 1;
		else if (src[len - 2] == '=')
			return NULL;
	}
	size_t out_len = len / 4 * 3 - pad;

	/* malloc(0) may return NULL, which would read as failure. */
	unsigned char *out = (unsigned char *)malloc(out_len ? out_len : 1);
	if (!out)
		return NULL;

	size_t o = 0;
	for (size_t i = 0; i < len; i += 4) {
		int last = (i + 4 == len);
		int v[4];
		for (int k = 0; k < 4; k++) {
			/* `=` is only ever the third or fourth character of
			 * the final group. Anywhere else it is corruption,
			 * and b64val refuses it. */
			if (last && k >= 2 && src[i + k] == '=') {
				v[k] = 0;
				continue;
			}
			v[k] = b64val((unsigned char)src[i + k]);
			if (v[k] < 0) {
				free(out);
				return NULL;
			}
		}
		unsigned w = ((unsigned)v[0] << 18) | ((unsigned)v[1] << 12)
			   | ((unsigned)v[2] << 6) | (unsigned)v[3];
		if (o < out_len)
			out[o++] = (unsigned char)((w >> 16) & 0xff);
		if (o < out_len)
			out[o++] = (unsigned char)((w >> 8) & 0xff);
		if (o < out_len)
			out[o++] = (unsigned char)(w & 0xff);
	}

	*n = out_len;
	return out;
}
