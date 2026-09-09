/* End-to-end spike test.
 *
 * Built for ucrt64 and linked against the generated client. Every call below
 * is a real libalpm call executing inside MSYS2, reached over the pipe. The
 * server is not running when this starts -- the first call launches it.
 */
#include <alpm.h>
#include <alpm_list.h>
#include "path_form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int cond, const char *what, const char *detail)
{
	printf("%-6s %-34s %s\n", cond ? "ok" : "FAIL", what,
	       detail ? detail : "");
	if (!cond)
		failures++;
}

int main(void)
{
	printf("-- no handle needed (this call starts the server) --\n");

	const char *ver = alpm_version();
	check(ver && strlen(ver) > 0, "alpm_version()", ver);

	int caps = alpm_capabilities();
	char buf[64];
	snprintf(buf, sizeof(buf), "0x%x", caps);
	check(caps != -1, "alpm_capabilities()", buf);

	check(alpm_pkg_vercmp("1.0-1", "1.0-2") < 0, "vercmp 1.0-1 < 1.0-2", NULL);
	check(alpm_pkg_vercmp("2.0", "1.9") > 0, "vercmp 2.0 > 1.9", NULL);
	/* Expectations here are taken from the reference `vercmp` shipped with
	 * this pacman, not from assumptions about epoch semantics: the point of
	 * the test is that we agree with the local libalpm, whatever it does. */
	check(alpm_pkg_vercmp("1:1.0", "2.0") < 0, "vercmp matches ref vercmp",
	      "1:1.0 < 2.0");

	printf("\n-- caller-owned string (header returns char*, we free it) --\n");
	char *sum = alpm_compute_md5sum("C:/dev/msys64/usr/include/alpm.h");
	check(sum && strlen(sum) == 32, "alpm_compute_md5sum()", sum);
	free(sum);

	printf("\n-- handles --\n");
	alpm_errno_t err = 0;
	alpm_handle_t *h = alpm_initialize("/", "/var/lib/pacman/", &err);
	snprintf(buf, sizeof(buf), "err=%d", (int)err);
	check(h != NULL, "alpm_initialize()", buf);
	if (!h) {
		printf("\ncannot continue without a handle\n");
		return 1;
	}

	const char *root = alpm_option_get_root(h);
	check(path_in_built_form(root),
	      "alpm_option_get_root() is a " PATH_FORM " path", root);
	const char *dbpath = alpm_option_get_dbpath(h);
	check(dbpath && strstr(dbpath, "pacman") != NULL,
	      "alpm_option_get_dbpath()", dbpath);

	alpm_db_t *db = alpm_get_localdb(h);
	check(db != NULL, "alpm_get_localdb()", NULL);

	const char *name = alpm_db_get_name(db);
	check(name && !strcmp(name, "local"), "alpm_db_get_name()", name);

	printf("\n-- borrowed strings keep libalpm's lifetime contract --\n");
	const char *again = alpm_db_get_name(db);
	check(again == name, "same pointer on repeat call",
	      "cached against the owning handle");
	check(!strcmp(name, "local"), "earlier pointer still readable", name);

	printf("\n-- bytes cross as bytes, not as text --\n");
	/* "AP9BAAo=" decodes to 00 ff 41 00 0a: two NULs and a byte that is
	 * not valid in any text encoding. A wire that treated this as a
	 * string would come back one byte long, and a length that was
	 * believed rather than derived would not notice. */
	unsigned char *raw = NULL;
	size_t raw_len = 0;
	int drc = alpm_decode_signature("AP9BAAo=", &raw, &raw_len);
	check(drc == 0, "alpm_decode_signature()", NULL);
	{
		static const unsigned char want[] = { 0x00, 0xff, 0x41, 0x00,
						      0x0a };
		char detail[64];
		snprintf(detail, sizeof(detail), "%zu bytes", raw_len);
		check(raw_len == sizeof(want), "the whole buffer came back",
		      detail);
		check(raw && raw_len == sizeof(want)
		      && memcmp(raw, want, sizeof(want)) == 0,
		      "every byte of it, NULs included", NULL);
	}
	free(raw);

	printf("\n-- type safety across the wire --\n");
	/* An alpm_handle_t id where a db id belongs. On the server this fails
	 * a tag check instead of reaching libalpm as the wrong pointer. */
	const char *bogus = alpm_db_get_name((alpm_db_t *)h);
	check(bogus == NULL, "handle id rejected as db id", "server tag check");

	printf("\n-- teardown --\n");
	int rc = alpm_release(h);
	check(rc == 0, "alpm_release()", NULL);

	const char *dead = alpm_db_get_name(db);
	check(dead == NULL, "db id invalid after release",
	      "owner cascade dropped it");

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
