/* Round-trip tests for the JSON codec.
 *
 * The writer emits, the reader parses it back, and the result must equal what
 * went in. Escaping is where a fast encoder goes wrong quietly, so most of
 * these are strings chosen to land on a boundary in the run-copying loop.
 */
#include "arpc_json.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void fail(const char *what, const char *got, const char *want)
{
	printf("FAIL  %s\n        got  %s\n        want %s\n", what,
	       got ? got : "(null)", want ? want : "(null)");
	fails++;
}

/* Encode one string as {"v":<s>}, parse it back, compare. */
static void rt_str(const char *label, const char *s)
{
	aj_w w;
	ajw_init(&w);
	ajw_obj_begin(&w);
	ajw_key(&w, "v");
	ajw_str(&w, s);
	ajw_obj_end(&w);

	if (w.err) {
		fail(label, "(writer error)", s);
		ajw_free(&w);
		return;
	}

	aj_doc d;
	if (!aj_parse(&d, w.buf, w.len)) {
		printf("FAIL  %s -- did not re-parse: %s\n", label, w.buf);
		fails++;
		aj_free(&d);
		ajw_free(&w);
		return;
	}
	const char *back = aj_str(&d, aj_member(&d, 0, "v"), NULL);
	if (!s) {
		if (!aj_is_null(&d, aj_member(&d, 0, "v")))
			fail(label, back, "null");
	} else if (!back || strcmp(back, s)) {
		fail(label, back, s);
	}
	aj_free(&d);
	ajw_free(&w);
}

static void rt_i64(const char *label, long long v)
{
	aj_w w;
	ajw_init(&w);
	ajw_obj_begin(&w);
	ajw_key(&w, "v");
	ajw_i64(&w, v);
	ajw_obj_end(&w);

	aj_doc d;
	if (!aj_parse(&d, w.buf, w.len)) {
		printf("FAIL  %s -- did not re-parse: %s\n", label, w.buf);
		fails++;
	} else {
		long long back = aj_i64(&d, aj_member(&d, 0, "v"), 0);
		if (back != v) {
			char g[32], e[32];
			snprintf(g, sizeof(g), "%lld", back);
			snprintf(e, sizeof(e), "%lld", v);
			fail(label, g, e);
		}
	}
	aj_free(&d);
	ajw_free(&w);
}

static void expect_reject(const char *label, const char *text)
{
	aj_doc d;
	if (aj_parse(&d, text, strlen(text))) {
		printf("FAIL  %s -- accepted invalid input: %s\n", label, text);
		fails++;
	}
	aj_free(&d);
}

int main(void)
{
	printf("strings\n");
	rt_str("empty", "");
	rt_str("plain", "mingw-w64-ucrt-x86_64-gcc");
	rt_str("null pointer", NULL);
	rt_str("quote", "say \"hello\"");
	rt_str("backslash", "C:\\dev\\msys64\\usr");
	rt_str("both", "a\"b\\c");
	rt_str("newline/tab", "line1\nline2\tend");
	rt_str("all simple escapes", "\b\f\n\r\t\"\\");
	rt_str("control char", "bell:\x07 nul-free");
	rt_str("escape at start", "\"leading");
	rt_str("escape at end", "trailing\"");
	rt_str("only escapes", "\\\\\\\\");
	/* Hex escapes are greedy, so each is closed off with a string break. */
	rt_str("utf-8 2-byte", "Ce\xc3\xa7" "a d\xc3\xa9" "j\xc3\xa0");
	rt_str("utf-8 3-byte", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
	rt_str("utf-8 4-byte", "\xf0\x9f\x93\xa6" " package");
	rt_str("utf-8 then escape", "\xc3\xa9" "\"" "\xc3\xa9");

	/* Long strings cross the initial reservation and force a regrow
	 * mid-run, which is where the run-copying loop is easiest to break. */
	char big[8192];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	rt_str("long clean", big);
	big[4000] = '"';
	big[4001] = '\\';
	big[6000] = '\n';
	rt_str("long with escapes", big);

	printf("integers\n");
	rt_i64("zero", 0);
	rt_i64("one", 1);
	rt_i64("negative", -1);
	rt_i64("nine digits", 123456789);
	rt_i64("LLONG_MAX", LLONG_MAX);
	rt_i64("LLONG_MIN", LLONG_MIN);
	rt_i64("epoch-ish", 1700000000);

	printf("structure\n");
	{
		aj_w w;
		ajw_init(&w);
		ajw_obj_begin(&w);
		ajw_key(&w, "id");
		ajw_i64(&w, 7);
		ajw_key(&w, "list");
		ajw_arr_begin(&w);
		for (int i = 0; i < 3; i++) {
			ajw_obj_begin(&w);
			ajw_key(&w, "n");
			ajw_i64(&w, i);
			ajw_obj_end(&w);
		}
		ajw_arr_end(&w);
		ajw_obj_end(&w);

		aj_doc d;
		if (!aj_parse(&d, w.buf, w.len)) {
			printf("FAIL  nested -- did not parse: %s\n", w.buf);
			fails++;
		} else {
			int arr = aj_member(&d, 0, "list");
			if (aj_count(&d, arr) != 3)
				fail("nested count", "?", "3");
			for (int i = 0; i < 3; i++) {
				int e = aj_elem(&d, arr, i);
				if (aj_i64(&d, aj_member(&d, e, "n"), -1) != i)
					fail("nested element", "?", "i");
			}
			if (aj_i64(&d, aj_member(&d, 0, "id"), 0) != 7)
				fail("first key at offset 0", "?", "7");
		}
		aj_free(&d);
		ajw_free(&w);
	}

	printf("splice (ajw_raw)\n");
	{
		aj_w inner;
		ajw_init(&inner);
		ajw_obj_begin(&inner);
		ajw_key(&inner, "ret");
		ajw_str(&inner, "local\"db");
		ajw_obj_end(&inner);

		aj_w outer;
		ajw_init(&outer);
		ajw_obj_begin(&outer);
		ajw_key(&outer, "id");
		ajw_i64(&outer, 1);
		ajw_reserve(&outer, inner.len + 2);
		ajw_key(&outer, "result");
		ajw_raw(&outer, inner.buf, inner.len);
		outer.need_comma = 1;
		ajw_obj_end(&outer);

		aj_doc d;
		if (!aj_parse(&d, outer.buf, outer.len)) {
			printf("FAIL  splice -- did not parse: %s\n", outer.buf);
			fails++;
		} else {
			int res = aj_member(&d, 0, "result");
			const char *v = aj_str(&d, aj_member(&d, res, "ret"), NULL);
			if (!v || strcmp(v, "local\"db"))
				fail("spliced value", v, "local\"db");
		}
		aj_free(&d);
		ajw_free(&inner);
		ajw_free(&outer);
	}

	printf("rejects malformed input\n");
	expect_reject("trailing garbage", "{\"a\":1} x");
	expect_reject("unterminated string", "{\"a\":\"oops}");
	expect_reject("unterminated object", "{\"a\":1");
	expect_reject("bare control char", "{\"a\":\"x\ny\"}");
	expect_reject("float", "{\"a\":1.5}");
	expect_reject("missing colon", "{\"a\" 1}");
	expect_reject("trailing comma", "{\"a\":1,}");
	expect_reject("empty input", "");

	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
	       fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
