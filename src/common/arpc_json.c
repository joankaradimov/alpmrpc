#include "arpc_json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- reader */

typedef struct {
	const char *p, *end;
	aj_doc *d;
} aj_p;

static int node_new(aj_doc *d, aj_type t)
{
	if (d->count == d->cap) {
		int cap = d->cap ? d->cap * 2 : 32;
		aj_node *n = (aj_node *)realloc(d->nodes, (size_t)cap * sizeof(*n));
		if (!n)
			return -1;
		d->nodes = n;
		d->cap = cap;
	}
	aj_node *n = &d->nodes[d->count];
	memset(n, 0, sizeof(*n));
	n->type = t;
	n->first_child = n->next_sibling = -1;
	return d->count++;
}

/* Strings are decoded into one growing buffer. We hand out offsets as
 * pointers only after parsing finishes, since the buffer may move. */
static long sbuf_put(aj_doc *d, const char *s, size_t n)
{
	if (d->slen + n + 1 > d->scap) {
		size_t cap = d->scap ? d->scap * 2 : 256;
		while (cap < d->slen + n + 1)
			cap *= 2;
		char *b = (char *)realloc(d->sbuf, cap);
		if (!b)
			return -1;
		d->sbuf = b;
		d->scap = cap;
	}
	long off = (long)d->slen;
	memcpy(d->sbuf + d->slen, s, n);
	d->slen += n;
	d->sbuf[d->slen++] = '\0';
	return off;
}

static void skip_ws(aj_p *p)
{
	while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' ||
				 *p->p == '\n' || *p->p == '\r'))
		p->p++;
}

static int hex4(const char *s)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		int c = (unsigned char)s[i], d;
		if (c >= '0' && c <= '9')      d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return -1;
		v = v * 16 + d;
	}
	return v;
}

static void utf8_put(char *out, size_t *n, unsigned cp)
{
	if (cp < 0x80) {
		out[(*n)++] = (char)cp;
	} else if (cp < 0x800) {
		out[(*n)++] = (char)(0xC0 | (cp >> 6));
		out[(*n)++] = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out[(*n)++] = (char)(0xE0 | (cp >> 12));
		out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[(*n)++] = (char)(0x80 | (cp & 0x3F));
	} else {
		out[(*n)++] = (char)(0xF0 | (cp >> 18));
		out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[(*n)++] = (char)(0x80 | (cp & 0x3F));
	}
}

/* Decodes a JSON string starting at the opening quote. Returns an offset
 * into d->sbuf, or -1. */
static long parse_string_raw(aj_p *p)
{
	if (p->p >= p->end || *p->p != '"')
		return -1;
	p->p++;

	char stackbuf[256];
	char *tmp = stackbuf;
	size_t cap = sizeof(stackbuf), n = 0;
	long result = -1;

	while (p->p < p->end && *p->p != '"') {
		/* worst case one escape expands to 4 bytes */
		if (n + 5 > cap) {
			size_t ncap = cap * 2;
			char *nb = (char *)malloc(ncap);
			if (!nb)
				goto done;
			memcpy(nb, tmp, n);
			if (tmp != stackbuf)
				free(tmp);
			tmp = nb;
			cap = ncap;
		}
		unsigned char c = (unsigned char)*p->p;
		if (c != '\\') {
			if (c < 0x20)
				goto done;      /* control char must be escaped */
			tmp[n++] = (char)c;
			p->p++;
			continue;
		}
		p->p++;
		if (p->p >= p->end)
			goto done;
		switch (*p->p++) {
		case '"':  tmp[n++] = '"';  break;
		case '\\': tmp[n++] = '\\'; break;
		case '/':  tmp[n++] = '/';  break;
		case 'b':  tmp[n++] = '\b'; break;
		case 'f':  tmp[n++] = '\f'; break;
		case 'n':  tmp[n++] = '\n'; break;
		case 'r':  tmp[n++] = '\r'; break;
		case 't':  tmp[n++] = '\t'; break;
		case 'u': {
			if (p->end - p->p < 4)
				goto done;
			int hi = hex4(p->p);
			if (hi < 0)
				goto done;
			p->p += 4;
			unsigned cp = (unsigned)hi;
			if (cp >= 0xD800 && cp <= 0xDBFF &&
			    p->end - p->p >= 6 && p->p[0] == '\\' && p->p[1] == 'u') {
				int lo = hex4(p->p + 2);
				if (lo >= 0xDC00 && lo <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) +
					     ((unsigned)lo - 0xDC00);
					p->p += 6;
				}
			}
			utf8_put(tmp, &n, cp);
			break;
		}
		default:
			goto done;
		}
	}
	if (p->p >= p->end || *p->p != '"')
		goto done;
	p->p++;
	result = sbuf_put(p->d, tmp, n);

done:
	if (tmp != stackbuf)
		free(tmp);
	return result;
}

static int parse_value(aj_p *p);

static int parse_container(aj_p *p, int is_obj)
{
	int self = node_new(p->d, is_obj ? AJ_OBJ : AJ_ARR);
	if (self < 0)
		return -1;
	p->p++;                                 /* consume { or [ */
	skip_ws(p);

	char close = is_obj ? '}' : ']';
	if (p->p < p->end && *p->p == close) {
		p->p++;
		return self;
	}

	int prev = -1;
	for (;;) {
		long koff = -1;
		if (is_obj) {
			skip_ws(p);
			koff = parse_string_raw(p);
			if (koff < 0)
				return -1;
			skip_ws(p);
			if (p->p >= p->end || *p->p != ':')
				return -1;
			p->p++;
		}
		int child = parse_value(p);
		if (child < 0)
			return -1;
		if (is_obj)     /* biased by 1: offset 0 is a real key, NULL is not */
			p->d->nodes[child].key = (const char *)(intptr_t)(koff + 1);

		if (prev < 0)
			p->d->nodes[self].first_child = child;
		else
			p->d->nodes[prev].next_sibling = child;
		prev = child;

		skip_ws(p);
		if (p->p < p->end && *p->p == ',') {
			p->p++;
			continue;
		}
		if (p->p < p->end && *p->p == close) {
			p->p++;
			return self;
		}
		return -1;
	}
}

static int parse_value(aj_p *p)
{
	skip_ws(p);
	if (p->p >= p->end)
		return -1;

	char c = *p->p;
	if (c == '{')
		return parse_container(p, 1);
	if (c == '[')
		return parse_container(p, 0);

	if (c == '"') {
		long off = parse_string_raw(p);
		if (off < 0)
			return -1;
		int n = node_new(p->d, AJ_STR);
		if (n < 0)
			return -1;
		p->d->nodes[n].str = (const char *)(intptr_t)off;
		return n;
	}
	if ((size_t)(p->end - p->p) >= 4 && !memcmp(p->p, "true", 4)) {
		p->p += 4;
		int n = node_new(p->d, AJ_BOOL);
		if (n >= 0) p->d->nodes[n].num = 1;
		return n;
	}
	if ((size_t)(p->end - p->p) >= 5 && !memcmp(p->p, "false", 5)) {
		p->p += 5;
		int n = node_new(p->d, AJ_BOOL);
		if (n >= 0) p->d->nodes[n].num = 0;
		return n;
	}
	if ((size_t)(p->end - p->p) >= 4 && !memcmp(p->p, "null", 4)) {
		p->p += 4;
		return node_new(p->d, AJ_NULL);
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		int neg = 0;
		if (c == '-') { neg = 1; p->p++; }
		if (p->p >= p->end || *p->p < '0' || *p->p > '9')
			return -1;
		long long v = 0;
		while (p->p < p->end && *p->p >= '0' && *p->p <= '9') {
			v = v * 10 + (*p->p - '0');
			p->p++;
		}
		/* The protocol carries no reals; reject rather than truncate. */
		if (p->p < p->end && (*p->p == '.' || *p->p == 'e' || *p->p == 'E'))
			return -1;
		int n = node_new(p->d, AJ_NUM);
		if (n >= 0) p->d->nodes[n].num = neg ? -v : v;
		return n;
	}
	return -1;
}

int aj_parse(aj_doc *d, const char *text, size_t len)
{
	memset(d, 0, sizeof(*d));
	aj_p p = { text, text + len, d };

	int root = parse_value(&p);
	if (root != 0)
		return 0;
	skip_ws(&p);
	if (p.p != p.end)
		return 0;

	/* String offsets become pointers only now that sbuf has stopped moving. */
	for (int i = 0; i < d->count; i++) {
		aj_node *n = &d->nodes[i];
		if (n->type == AJ_STR)
			n->str = d->sbuf + (intptr_t)n->str;
		if (n->key)
			n->key = d->sbuf + ((intptr_t)n->key - 1);
	}
	d->ok = 1;
	return 1;
}

void aj_free(aj_doc *d)
{
	free(d->nodes);
	free(d->sbuf);
	memset(d, 0, sizeof(*d));
}

static const aj_node *at(const aj_doc *d, int i)
{
	if (i < 0 || i >= d->count)
		return NULL;
	return &d->nodes[i];
}

int aj_member(const aj_doc *d, int obj, const char *key)
{
	const aj_node *o = at(d, obj);
	if (!o || o->type != AJ_OBJ)
		return -1;
	for (int i = o->first_child; i >= 0; i = d->nodes[i].next_sibling)
		if (d->nodes[i].key && !strcmp(d->nodes[i].key, key))
			return i;
	return -1;
}

int aj_elem(const aj_doc *d, int arr, int index)
{
	const aj_node *a = at(d, arr);
	if (!a || (a->type != AJ_ARR && a->type != AJ_OBJ) || index < 0)
		return -1;
	int i = a->first_child;
	while (i >= 0 && index--)
		i = d->nodes[i].next_sibling;
	return i;
}

int aj_count(const aj_doc *d, int container)
{
	const aj_node *a = at(d, container);
	if (!a || (a->type != AJ_ARR && a->type != AJ_OBJ))
		return 0;
	int n = 0;
	for (int i = a->first_child; i >= 0; i = d->nodes[i].next_sibling)
		n++;
	return n;
}

long long aj_i64(const aj_doc *d, int node, long long dflt)
{
	const aj_node *n = at(d, node);
	return (n && (n->type == AJ_NUM || n->type == AJ_BOOL)) ? n->num : dflt;
}

const char *aj_str(const aj_doc *d, int node, const char *dflt)
{
	const aj_node *n = at(d, node);
	return (n && n->type == AJ_STR) ? n->str : dflt;
}

int aj_is_null(const aj_doc *d, int node)
{
	const aj_node *n = at(d, node);
	return !n || n->type == AJ_NULL;
}

/* ---------------------------------------------------------------- writer */

static void wput(aj_w *w, const char *s, size_t n)
{
	if (w->err)
		return;
	if (w->len + n + 1 > w->cap) {
		size_t cap = w->cap ? w->cap : 256;
		while (cap < w->len + n + 1)
			cap *= 2;
		char *b = (char *)realloc(w->buf, cap);
		if (!b) {
			w->err = 1;
			return;
		}
		w->buf = b;
		w->cap = cap;
	}
	memcpy(w->buf + w->len, s, n);
	w->len += n;
	w->buf[w->len] = '\0';
}

static void wsep(aj_w *w)
{
	if (w->need_comma)
		wput(w, ",", 1);
	w->need_comma = 1;
}

void ajw_init(aj_w *w) { memset(w, 0, sizeof(*w)); }
void ajw_free(aj_w *w) { free(w->buf); memset(w, 0, sizeof(*w)); }

void ajw_obj_begin(aj_w *w) { wsep(w); wput(w, "{", 1); w->need_comma = 0; }
void ajw_obj_end(aj_w *w)   { wput(w, "}", 1); w->need_comma = 1; }
void ajw_arr_begin(aj_w *w) { wsep(w); wput(w, "[", 1); w->need_comma = 0; }
void ajw_arr_end(aj_w *w)   { wput(w, "]", 1); w->need_comma = 1; }

static void wstr_escaped(aj_w *w, const char *s)
{
	wput(w, "\"", 1);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':  wput(w, "\\\"", 2); break;
		case '\\': wput(w, "\\\\", 2); break;
		case '\b': wput(w, "\\b", 2);  break;
		case '\f': wput(w, "\\f", 2);  break;
		case '\n': wput(w, "\\n", 2);  break;
		case '\r': wput(w, "\\r", 2);  break;
		case '\t': wput(w, "\\t", 2);  break;
		default:
			if (*p < 0x20) {
				char buf[7];
				static const char hex[] = "0123456789abcdef";
				buf[0] = '\\'; buf[1] = 'u'; buf[2] = '0'; buf[3] = '0';
				buf[4] = hex[*p >> 4]; buf[5] = hex[*p & 0xF]; buf[6] = 0;
				wput(w, buf, 6);
			} else {
				wput(w, (const char *)p, 1);
			}
		}
	}
	wput(w, "\"", 1);
}

void ajw_key(aj_w *w, const char *key)
{
	wsep(w);
	wstr_escaped(w, key);
	wput(w, ":", 1);
	w->need_comma = 0;
}

void ajw_str(aj_w *w, const char *s)
{
	if (!s) {
		ajw_null(w);
		return;
	}
	wsep(w);
	wstr_escaped(w, s);
}

void ajw_i64(aj_w *w, long long v)
{
	char buf[24];
	int n = 0, neg = v < 0;
	unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1
				   : (unsigned long long)v;
	char tmp[24];
	do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u);
	int k = 0;
	if (neg)
		buf[k++] = '-';
	while (n)
		buf[k++] = tmp[--n];
	wsep(w);
	wput(w, buf, (size_t)k);
}

void ajw_null(aj_w *w)
{
	wsep(w);
	wput(w, "null", 4);
}
