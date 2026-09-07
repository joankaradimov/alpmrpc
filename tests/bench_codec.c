/* Where does the time actually go?
 *
 * Two questions decide whether a codec swap is worth anything:
 *   1. what does one round trip cost, and
 *   2. what share of that is encode+decode rather than the kernel?
 *
 * Part 1 measures real calls through the pipe. Part 2 measures the JSON
 * codec alone, on the same frames, with no IPC in the way. The gap between
 * them is the ceiling on what any faster codec could ever buy.
 *
 * Part 3 measures a bulk payload, because that is the shape the pending
 * alpm_list_t work will actually put on the wire.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <alpm.h>

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

static void hdr(const char *s)
{
	printf("\n%s\n", s);
	for (const char *p = s; *p; p++)
		putchar('-');
	putchar('\n');
}

/* A response frame the size the current protocol actually produces. */
static const char *SMALL_RSP = "{\"id\":1,\"result\":{\"ret\":7}}";
static const char *SMALL_REQ =
	"{\"id\":1,\"method\":\"alpm_capabilities\",\"params\":[]}";

/* Build a listing of `n` packages with the fields a package browser needs.
 * This is what one coarse-grained db.list_packages call would return. */
static char *build_listing(int n, size_t *out_len, size_t reserve)
{
	aj_w w;
	ajw_init(&w);
	if (reserve)
		ajw_reserve(&w, reserve);
	ajw_obj_begin(&w);
	ajw_key(&w, "id");
	ajw_i64(&w, 1);
	ajw_key(&w, "result");
	ajw_obj_begin(&w);
	ajw_key(&w, "ret");
	ajw_arr_begin(&w);
	for (int i = 0; i < n; i++) {
		char buf[64];
		ajw_obj_begin(&w);
		snprintf(buf, sizeof(buf), "mingw-w64-ucrt-x86_64-package-%d", i);
		ajw_key(&w, "name");    ajw_str(&w, buf);
		snprintf(buf, sizeof(buf), "%d.%d.%d-%d", i % 20, i % 7, i % 13, i % 3);
		ajw_key(&w, "version"); ajw_str(&w, buf);
		ajw_key(&w, "desc");
		ajw_str(&w, "A reasonably typical one line package description");
		ajw_key(&w, "url");     ajw_str(&w, "https://example.invalid/project");
		ajw_key(&w, "packager");ajw_str(&w, "CI <ci@example.invalid>");
		ajw_key(&w, "arch");    ajw_str(&w, "x86_64");
		ajw_key(&w, "isize");   ajw_i64(&w, 100000 + i * 37);
		ajw_key(&w, "builddate"); ajw_i64(&w, 1700000000 + i);
		ajw_obj_end(&w);
	}
	ajw_arr_end(&w);
	ajw_obj_end(&w);
	ajw_obj_end(&w);
	*out_len = w.len;
	return w.buf;
}

int main(void)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	freq = (double)f.QuadPart / 1e6;   /* ticks per microsecond */

	hdr("1. round trip through the pipe");

	/* warm up: pays the lazy server launch outside the measurement */
	if (alpm_capabilities() == -1) {
		printf("cannot reach the server\n");
		return 1;
	}

	const int N = 20000;
	double t0 = now_us();
	for (int i = 0; i < N; i++)
		alpm_capabilities();
	double per_trivial = (now_us() - t0) / N;
	printf("  alpm_capabilities()   %8.2f us/call   (smallest possible frame)\n",
	       per_trivial);

	alpm_errno_t err = 0;
	alpm_handle_t *h = alpm_initialize("/", "/var/lib/pacman/", &err);
	if (!h) {
		printf("  alpm_initialize failed (err=%d)\n", (int)err);
		return 1;
	}
	alpm_db_t *db = alpm_get_localdb(h);

	t0 = now_us();
	for (int i = 0; i < N; i++)
		alpm_db_get_name(db);
	double per_string = (now_us() - t0) / N;
	printf("  alpm_db_get_name()    %8.2f us/call   (string return + intern)\n",
	       per_string);

	hdr("2. the JSON codec alone, same frames, no IPC");

	const int M = 200000;
	size_t req_len = strlen(SMALL_REQ), rsp_len = strlen(SMALL_RSP);

	t0 = now_us();
	for (int i = 0; i < M; i++) {
		aj_doc d;
		aj_parse(&d, SMALL_RSP, rsp_len);
		aj_free(&d);
	}
	double dec = (now_us() - t0) / M;

	t0 = now_us();
	for (int i = 0; i < M; i++) {
		aj_w w;
		ajw_init(&w);
		ajw_obj_begin(&w);
		ajw_key(&w, "id");     ajw_i64(&w, i);
		ajw_key(&w, "method"); ajw_str(&w, "alpm_capabilities");
		ajw_key(&w, "params"); ajw_arr_begin(&w); ajw_arr_end(&w);
		ajw_obj_end(&w);
		ajw_free(&w);
	}
	double enc = (now_us() - t0) / M;

	printf("  encode request        %8.3f us   (%zu bytes)\n", enc, req_len);
	printf("  decode response       %8.3f us   (%zu bytes)\n", dec, rsp_len);
	printf("  codec total           %8.3f us\n", enc + dec);
	printf("  share of round trip   %8.1f %%\n",
	       100.0 * (enc + dec) / per_trivial);

	hdr("3. bulk payload -- one coarse call returning 2000 packages");

	size_t big_len = 0;
	char *big = build_listing(2000, &big_len, 0);
	printf("  payload               %8.1f KB\n", big_len / 1024.0);

	const int K = 200;
	t0 = now_us();
	for (int i = 0; i < K; i++) {
		aj_doc d;
		if (!aj_parse(&d, big, big_len)) {
			printf("  parse FAILED\n");
			return 1;
		}
		aj_free(&d);
	}
	double big_dec = (now_us() - t0) / K;

	t0 = now_us();
	for (int i = 0; i < K; i++) {
		size_t n;
		free(build_listing(2000, &n, 0));
	}
	double big_enc = (now_us() - t0) / K;

	printf("  encode                %8.0f us   (%.0f MB/s)\n",
	       big_enc, big_len / big_enc);
	printf("  decode                %8.0f us   (%.0f MB/s)\n",
	       big_dec, big_len / big_dec);

	/* Same encode, but sized up front rather than walking up through every
	 * power of two. The server knows the list length before it starts
	 * emitting, so this is a change it can actually make. */
	t0 = now_us();
	for (int i = 0; i < K; i++) {
		size_t n;
		free(build_listing(2000, &n, big_len + 1024));
	}
	double big_enc_pre = (now_us() - t0) / K;
	printf("  encode, pre-sized     %8.0f us   (%.0f MB/s)   %.2fx\n",
	       big_enc_pre, big_len / big_enc_pre, big_enc / big_enc_pre);

	hdr("4. the comparison that matters");
	printf("  2000 pkgs x 8 fields, one field per call:\n");
	printf("    %d round trips        %8.0f ms\n",
	       2000 * 8 + 3, (2000 * 8 + 3) * per_trivial / 1000.0);
	printf("  same data, one coarse call:\n");
	printf("    1 round trip + codec  %8.1f ms\n",
	       (per_trivial + big_enc + big_dec) / 1000.0);
	printf("    speedup               %8.0fx\n",
	       ((2000 * 8 + 3) * per_trivial) / (per_trivial + big_enc + big_dec));

	free(big);
	alpm_release(h);
	return 0;
}
