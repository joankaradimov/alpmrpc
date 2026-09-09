/* What a column costs, and what the reads after it cost.
 *
 * The first read of any field for a package in a materialised list fetches
 * that field for the whole list in one call; the reads that follow are
 * memory. This prints both, per field, so that the README's claim about the
 * 1149 reads that follow the first can be checked against a real pkgcache.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <alpm.h>
#include <alpm_list.h>

#include <stdio.h>

static double freq;

static double now_us(void)
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart / freq;
}

/* The first member's read is the column fetch; the rest are the cache. */
#define COLUMN(label, expr)                                              \
	do {                                                             \
		alpm_list_t *i = cache;                                  \
		double t0 = now_us();                                    \
		(void)(expr);                                            \
		double t1 = now_us();                                    \
		for (i = i->next; i; i = i->next)                        \
			(void)(expr);                                    \
		double t2 = now_us();                                    \
		printf("  %-26s %8.2f ms %8.3f ms\n", label,             \
		       (t1 - t0) / 1000.0, (t2 - t1) / 1000.0);          \
	} while (0)

int main(void)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	freq = (double)f.QuadPart / 1e6;

	alpm_errno_t err = 0;
	alpm_handle_t *h = alpm_initialize("/", "/var/lib/pacman/", &err);
	if (!h) {
		printf("alpm_initialize failed (err=%d)\n", (int)err);
		return 1;
	}
	alpm_list_t *cache = alpm_db_get_pkgcache(alpm_get_localdb(h));
	size_t n = alpm_list_count(cache);
	if (n == 0) {
		printf("no packages in the local db\n");
		alpm_release(h);
		return 1;
	}
	printf("  %zu packages\n\n", n);

	char rest[32];
	snprintf(rest, sizeof(rest), "rest (x%zu)", n - 1);
	printf("  %-26s %10s %10s\n", "field", "first", rest);

	COLUMN("alpm_pkg_get_name",
	       alpm_pkg_get_name((alpm_pkg_t *)i->data));
	COLUMN("alpm_pkg_get_version",
	       alpm_pkg_get_version((alpm_pkg_t *)i->data));
	COLUMN("alpm_pkg_get_desc",
	       alpm_pkg_get_desc((alpm_pkg_t *)i->data));
	COLUMN("alpm_pkg_get_url",
	       alpm_pkg_get_url((alpm_pkg_t *)i->data));
	COLUMN("alpm_pkg_get_isize",
	       alpm_pkg_get_isize((alpm_pkg_t *)i->data));
	COLUMN("alpm_pkg_get_builddate",
	       alpm_pkg_get_builddate((alpm_pkg_t *)i->data));

	alpm_release(h);
	return 0;
}
