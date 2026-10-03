/* Each handle's signatures, checked against that handle's keyring.
 *
 * libalpm hands gpgme a gpgdir once per process: init_gpgme() sets it the
 * first time a signature is checked and never again, and every gpgme context
 * libalpm opens after that starts from it. This process serves one client
 * after another, so left alone, the first client to check a signature would
 * choose the keyring for every client after it. One whose gpgdir differs
 * would be checked against keys it never imported -- and could be told yes
 * as easily as no.
 *
 * So before each call the dispatcher points gpgme at the gpgdir of the
 * handle the call is for, whenever that is not where gpgme already points.
 * The first time sets gpgme up, earlier than libalpm would have: one short
 * gpgconf run per server, which libalpm's first signature check would
 * otherwise make.
 */
#include "arpc_server.h"

#include <alpm.h>
#include <gpgme.h>

#include <stdlib.h>
#include <string.h>

/* Each handle's gpgdir, as last read. libalpm is asked only while the
 * handle has no error pending: alpm_option_get_gpgdir() clears it, as every
 * libalpm entry point does, and the call about to run may be alpm_errno()
 * asking what the last one failed with. Otherwise the last value read is
 * still the handle's: the gpgdir changes only through a successful
 * alpm_option_set_gpgdir(), which leaves no error, so the next call reads
 * it again. A released handle's entry stays until its address comes back,
 * and a handle at that address is read before anything else uses it. */
typedef struct seen {
	struct seen *next;
	void *handle;
	char *gpgdir;           /* NULL is gpgme's default */
} seen;
static seen *g_seen;

/* Where gpgme was last pointed from here; nowhere until the first call. */
static int g_pointed;
static char *g_gpgdir;

static int same(const char *a, const char *b)
{
	return a == b || (a && b && !strcmp(a, b));
}

/* *dst = a copy of src, NULL for NULL; 0 if out of memory, *dst untouched. */
static int replace(char **dst, const char *src)
{
	char *copy = src ? strdup(src) : NULL;
	if (src && !copy)
		return 0;
	free(*dst);
	*dst = copy;
	return 1;
}

static seen *gpgdir_of(alpm_handle_t *h)
{
	seen *s = g_seen;
	while (s && s->handle != h)
		s = s->next;
	if (alpm_errno(h) != ALPM_ERR_OK)
		return s;

	const char *now = alpm_option_get_gpgdir(h);
	if (!s) {
		s = calloc(1, sizeof(*s));
		if (!s)
			return NULL;
		s->handle = h;
		s->next = g_seen;
		g_seen = s;
	}
	if (!same(s->gpgdir, now) && !replace(&s->gpgdir, now))
		return NULL;
	return s;
}

void arpc_gpgdir_follow(void *handle)
{
	if (!handle)
		return;
	seen *s = gpgdir_of((alpm_handle_t *)handle);
	if (!s || (g_pointed && same(s->gpgdir, g_gpgdir)))
		return;

	/* gpgme_check_version() is what initialises gpgme, and libalpm may
	 * not have called it yet; calling it again is harmless. */
	gpgme_check_version(NULL);
	if (gpgme_set_engine_info(GPGME_PROTOCOL_OpenPGP, NULL, s->gpgdir))
		return;                 /* unchanged, so the next call tries again */
	if (replace(&g_gpgdir, s->gpgdir))
		g_pointed = 1;
}
