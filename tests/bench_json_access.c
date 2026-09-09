/* aj_elem(d, arr, i) walks the sibling chain from the start every time, so a
 * loop over it is O(n^2). Does that matter at the sizes actually sent?
 *
 * The answer decided the aj_first/aj_next pair: at 1214 elements, the size
 * of a real pkgcache, indexing measured 918us against 1.6us for a walk. The
 * generated code walks; this is here so that the reason stays checkable.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "arpc_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double freq;

static double now_us(void)
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart / freq;
}

/* An array of n package names: the shape a column arrives in. */
static char *build_array(int n, size_t *len)
{
	aj_w w;
	ajw_init(&w);
	ajw_arr_begin(&w);
	for (int i = 0; i < n; i++) {
		char name[64];
		snprintf(name, sizeof(name), "mingw-w64-ucrt-x86_64-package-%d",
			 i);
		ajw_str(&w, name);
	}
	ajw_arr_end(&w);
	*len = w.len;
	return w.buf;
}

int main(void)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	freq = (double)f.QuadPart / 1e6;

	static const int sizes[] = { 100, 500, 1214, 5000 };
	const int reps = 200;

	printf("  %-10s %-9s %10s %10s %10s\n", "elements", "payload", "parse",
	       "aj_elem", "walk");
	for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); s++) {
		int n = sizes[s];
		size_t len;
		char *json = build_array(n, &len);

		double t0 = now_us();
		for (int r = 0; r < reps; r++) {
			aj_doc d;
			aj_parse(&d, json, len);
			aj_free(&d);
		}
		double parse = (now_us() - t0) / reps;

		aj_doc d;
		aj_parse(&d, json, len);

		t0 = now_us();
		for (int r = 0; r < reps; r++) {
			volatile size_t acc = 0;
			for (int i = 0; i < n; i++)
				acc += (size_t)aj_str(&d, aj_elem(&d, 0, i), NULL);
		}
		double by_index = (now_us() - t0) / reps;

		t0 = now_us();
		for (int r = 0; r < reps; r++) {
			volatile size_t acc = 0;
			for (int e = aj_first(&d, 0); e >= 0; e = aj_next(&d, e))
				acc += (size_t)aj_str(&d, e, NULL);
		}
		double by_walk = (now_us() - t0) / reps;

		aj_free(&d);
		printf("  %-10d %7.1f KB %8.1f us %8.1f us %8.1f us\n", n,
		       len / 1024.0, parse, by_index, by_walk);
		free(json);
	}
	return 0;
}
