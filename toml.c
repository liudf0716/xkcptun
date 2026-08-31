/*
  MIT License

  Copyright (c) 2017 CK Tan
  https://github.com/cktan/tomlc99

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>

#include "toml.h"

typedef struct toml_keyval_t toml_keyval_t;
struct toml_keyval_t {
	const char *key;
	const char *val;
};

struct toml_array_t {
	const char *key;
	int kind; /* 'v'alue, 'a'rray, 't'able */
	int type; /* 'd'ouble, 'i'nt, 'b'ool, 's'tring, 't'imestamp, 't'able, 'a'rray */
	int nelem;
	union {
		char **val;
		toml_array_t **arr;
		toml_table_t **tab;
	} u;
};

struct toml_table_t {
	const char *key;
	bool implicit;
	int nkval;
	toml_keyval_t **kval;
	int narr;
	toml_array_t **arr;
	int ntab;
	toml_table_t **tab;
};

static void xfree(const void *x) { if (x) free((void *)x); }
static void *xrealloc(void *ptr, size_t sz) {
	void *p = realloc(ptr, sz);
	if (!p && sz) {
		free(ptr);
		return NULL;
	}
	return p;
}

static void xfree_array(toml_array_t *p);
static void xfree_table(toml_table_t *p);

static void xfree_array(toml_array_t *p) {
	if (!p) return;
	xfree(p->key);
	if (p->kind == 'v') {
		for (int i = 0; i < p->nelem; i++) xfree(p->u.val[i]);
		xfree(p->u.val);
	} else if (p->kind == 'a') {
		for (int i = 0; i < p->nelem; i++) xfree_array(p->u.arr[i]);
		xfree(p->u.arr);
	} else if (p->kind == 't') {
		for (int i = 0; i < p->nelem; i++) xfree_table(p->u.tab[i]);
		xfree(p->u.tab);
	}
	xfree(p);
}

static void xfree_table(toml_table_t *p) {
	if (!p) return;
	xfree(p->key);
	for (int i = 0; i < p->nkval; i++) {
		xfree(p->kval[i]->key);
		xfree(p->kval[i]->val);
		xfree(p->kval[i]);
	}
	xfree(p->kval);
	for (int i = 0; i < p->narr; i++) xfree_array(p->arr[i]);
	xfree(p->arr);
	for (int i = 0; i < p->ntab; i++) xfree_table(p->tab[i]);
	xfree(p->tab);
	xfree(p);
}

void toml_free(toml_table_t *tab) {
	xfree_table(tab);
}

/* Helper token scanner */
typedef struct {
	char *buf;
	int top;
	int cap;
} strbuf_t;

static void sb_init(strbuf_t *sb) { sb->buf = NULL; sb->top = sb->cap = 0; }
static void sb_free(strbuf_t *sb) { xfree(sb->buf); }
static int sb_push(strbuf_t *sb, char c) {
	if (sb->top + 1 >= sb->cap) {
		int ncap = sb->cap ? sb->cap * 2 : 64;
		char *nbuf = xrealloc(sb->buf, ncap);
		if (!nbuf) return -1;
		sb->buf = nbuf;
		sb->cap = ncap;
	}
	sb->buf[sb->top++] = c;
	sb->buf[sb->top] = 0;
	return 0;
}

static void skip_ws(const char **sp) {
	while (**sp && (**sp == ' ' || **sp == '\t' || **sp == '\r')) (*sp)++;
}

static void skip_comment_and_ws(const char **sp) {
	for (;;) {
		skip_ws(sp);
		if (**sp == '#') {
			while (**sp && **sp != '\n') (*sp)++;
		} else if (**sp == '\n') {
			(*sp)++;
		} else {
			break;
		}
	}
}

static char *scan_key(const char **sp, char *errbuf, int errbufsz) {
	skip_ws(sp);
	strbuf_t sb;
	sb_init(&sb);
	if (**sp == '"' || **sp == '\'') {
		char q = *(*sp)++;
		while (**sp && **sp != q && **sp != '\n') {
			if (q == '"' && **sp == '\\') {
				(*sp)++;
				if (!**sp) break;
			}
			sb_push(&sb, *(*sp)++);
		}
		if (**sp != q) {
			snprintf(errbuf, errbufsz, "unterminated quoted key");
			sb_free(&sb);
			return NULL;
		}
		(*sp)++;
	} else {
		while (**sp && (isalnum((unsigned char)**sp) || **sp == '_' || **sp == '-')) {
			sb_push(&sb, *(*sp)++);
		}
	}
	if (sb.top == 0) {
		snprintf(errbuf, errbufsz, "empty key");
		sb_free(&sb);
		return NULL;
	}
	return sb.buf;
}

static char *scan_val(const char **sp, char *errbuf, int errbufsz);

static char *scan_val(const char **sp, char *errbuf, int errbufsz) {
	skip_ws(sp);
	if (!**sp) {
		snprintf(errbuf, errbufsz, "unexpected EOF in value");
		return NULL;
	}

	strbuf_t sb;
	sb_init(&sb);

	if (**sp == '"' || **sp == '\'') {
		char q = *(*sp)++;
		sb_push(&sb, q);
		while (**sp && **sp != q && **sp != '\n') {
			if (q == '"' && **sp == '\\') {
				sb_push(&sb, *(*sp)++);
				if (!**sp) break;
			}
			sb_push(&sb, *(*sp)++);
		}
		if (**sp != q) {
			snprintf(errbuf, errbufsz, "unterminated quoted string");
			sb_free(&sb);
			return NULL;
		}
		sb_push(&sb, *(*sp)++);
		return sb.buf;
	}

	if (**sp == '[') {
		/* Inline Array */
		sb_push(&sb, *(*sp)++);
		int depth = 1;
		while (**sp && depth > 0) {
			skip_comment_and_ws(sp);
			if (**sp == '[') depth++;
			else if (**sp == ']') depth--;
			if (**sp == '"' || **sp == '\'') {
				char *s = scan_val(sp, errbuf, errbufsz);
				if (!s) { sb_free(&sb); return NULL; }
				for (char *p = s; *p; p++) sb_push(&sb, *p);
				free(s);
				continue;
			}
			if (**sp) sb_push(&sb, *(*sp)++);
		}
		return sb.buf;
	}

	if (**sp == '{') {
		/* Inline Table */
		sb_push(&sb, *(*sp)++);
		int depth = 1;
		while (**sp && depth > 0) {
			if (**sp == '{') depth++;
			else if (**sp == '}') depth--;
			if (**sp == '"' || **sp == '\'') {
				char *s = scan_val(sp, errbuf, errbufsz);
				if (!s) { sb_free(&sb); return NULL; }
				for (char *p = s; *p; p++) sb_push(&sb, *p);
				free(s);
				continue;
			}
			if (**sp) sb_push(&sb, *(*sp)++);
		}
		return sb.buf;
	}

	/* Simple literal: integer, bool, float, timestamp */
	while (**sp && **sp != '\n' && **sp != '\r' && **sp != '#' && **sp != ',' && **sp != ']' && **sp != '}') {
		if (**sp == ' ' || **sp == '\t') {
			/* Check if trailing comment or end of token */
			const char *peek = *sp;
			skip_ws(&peek);
			if (*peek == '\n' || *peek == '\r' || *peek == '#' || *peek == ',' || *peek == ']' || *peek == '}' || !*peek)
				break;
		}
		sb_push(&sb, *(*sp)++);
	}

	if (sb.top == 0) {
		snprintf(errbuf, errbufsz, "empty value");
		sb_free(&sb);
		return NULL;
	}
	return sb.buf;
}

static toml_table_t *create_table(const char *key, bool implicit) {
	toml_table_t *t = calloc(1, sizeof(*t));
	if (t && key) t->key = strdup(key);
	if (t) t->implicit = implicit;
	return t;
}

static toml_array_t *create_array(const char *key, int kind) {
	toml_array_t *a = calloc(1, sizeof(*a));
	if (a && key) a->key = strdup(key);
	if (a) a->kind = kind;
	return a;
}

static int add_keyval(toml_table_t *t, const char *k, const char *v) {
	toml_keyval_t *kv = malloc(sizeof(*kv));
	if (!kv) return -1;
	kv->key = strdup(k);
	kv->val = strdup(v);
	toml_keyval_t **n = xrealloc(t->kval, sizeof(*n) * (t->nkval + 1));
	if (!n) { free((char*)kv->key); free((char*)kv->val); free(kv); return -1; }
	t->kval = n;
	t->kval[t->nkval++] = kv;
	return 0;
}

static toml_table_t *find_or_create_subtable(toml_table_t *root, const char *key, bool implicit) {
	for (int i = 0; i < root->ntab; i++) {
		if (strcmp(root->tab[i]->key, key) == 0) return root->tab[i];
	}
	toml_table_t *sub = create_table(key, implicit);
	if (!sub) return NULL;
	toml_table_t **n = xrealloc(root->tab, sizeof(*n) * (root->ntab + 1));
	if (!n) { xfree_table(sub); return NULL; }
	root->tab = n;
	root->tab[root->ntab++] = sub;
	return sub;
}

static toml_array_t *find_or_create_array_of_tables(toml_table_t *root, const char *key) {
	for (int i = 0; i < root->narr; i++) {
		if (root->arr[i]->kind == 't' && strcmp(root->arr[i]->key, key) == 0)
			return root->arr[i];
	}
	toml_array_t *arr = create_array(key, 't');
	if (!arr) return NULL;
	toml_array_t **n = xrealloc(root->arr, sizeof(*n) * (root->narr + 1));
	if (!n) { xfree_array(arr); return NULL; }
	root->arr = n;
	root->arr[root->narr++] = arr;
	return arr;
}

static toml_table_t *append_table_to_array(toml_array_t *arr, const char *key) {
	toml_table_t *t = create_table(key, false);
	if (!t) return NULL;
	toml_table_t **n = xrealloc(arr->u.tab, sizeof(*n) * (arr->nelem + 1));
	if (!n) { xfree_table(t); return NULL; }
	arr->u.tab = n;
	arr->u.tab[arr->nelem++] = t;
	return t;
}

toml_table_t *toml_parse(char *conf, char *errbuf, int errbufsz) {
	if (!conf) return NULL;
	if (!errbuf || errbufsz <= 0) {
		static char dummy[256];
		errbuf = dummy;
		errbufsz = sizeof(dummy);
	}
	errbuf[0] = '\0';

	toml_table_t *root = create_table(NULL, false);
	toml_table_t *cur_tab = root;
	const char *sp = conf;

	while (*sp) {
		skip_ws(&sp);
		if (!*sp) break;

		if (*sp == '#') {
			while (*sp && *sp != '\n') sp++;
			continue;
		}
		if (*sp == '\n' || *sp == '\r') {
			sp++;
			continue;
		}

		/* Array of Tables [[section]] */
		if (sp[0] == '[' && sp[1] == '[') {
			sp += 2;
			char *key = scan_key(&sp, errbuf, errbufsz);
			if (!key) { toml_free(root); return NULL; }
			skip_ws(&sp);
			if (sp[0] != ']' || sp[1] != ']') {
				snprintf(errbuf, errbufsz, "expected ']]' at end of table array name");
				free(key);
				toml_free(root);
				return NULL;
			}
			sp += 2;
			toml_array_t *arr = find_or_create_array_of_tables(root, key);
			if (!arr) { free(key); toml_free(root); return NULL; }
			cur_tab = append_table_to_array(arr, key);
			free(key);
			continue;
		}

		/* Standard Table [section] */
		if (*sp == '[') {
			sp++;
			char *key = scan_key(&sp, errbuf, errbufsz);
			if (!key) { toml_free(root); return NULL; }
			skip_ws(&sp);
			if (*sp != ']') {
				snprintf(errbuf, errbufsz, "expected ']' at end of table name");
				free(key);
				toml_free(root);
				return NULL;
			}
			sp++;
			cur_tab = find_or_create_subtable(root, key, false);
			free(key);
			continue;
		}

		/* Key = Value */
		char *k = scan_key(&sp, errbuf, errbufsz);
		if (!k) { toml_free(root); return NULL; }
		skip_ws(&sp);
		if (*sp != '=') {
			snprintf(errbuf, errbufsz, "expected '=' after key [%s]", k);
			free(k);
			toml_free(root);
			return NULL;
		}
		sp++;
		char *v = scan_val(&sp, errbuf, errbufsz);
		if (!v) { free(k); toml_free(root); return NULL; }
		add_keyval(cur_tab, k, v);
		free(k);
		free(v);
	}

	return root;
}

toml_table_t *toml_parse_file(FILE *fp, char *errbuf, int errbufsz) {
	if (!fp) {
		if (errbuf && errbufsz > 0) snprintf(errbuf, errbufsz, "NULL file pointer");
		return NULL;
	}
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 0) {
		if (errbuf && errbufsz > 0) snprintf(errbuf, errbufsz, "ftell error");
		return NULL;
	}

	char *buf = malloc(sz + 1);
	if (!buf) {
		if (errbuf && errbufsz > 0) snprintf(errbuf, errbufsz, "out of memory");
		return NULL;
	}
	size_t nr = fread(buf, 1, sz, fp);
	buf[nr] = '\0';
	toml_table_t *tab = toml_parse(buf, errbuf, errbufsz);
	free(buf);
	return tab;
}

const char *toml_key_in(const toml_table_t *tab, int keyidx) {
	if (!tab || keyidx < 0) return NULL;
	if (keyidx < tab->nkval) return tab->kval[keyidx]->key;
	keyidx -= tab->nkval;
	if (keyidx < tab->narr) return tab->arr[keyidx]->key;
	keyidx -= tab->narr;
	if (keyidx < tab->ntab) return tab->tab[keyidx]->key;
	return NULL;
}

toml_datum_t toml_string_in(const toml_table_t *tab, const char *key) {
	toml_datum_t d = {0};
	if (!tab || !key) return d;
	for (int i = 0; i < tab->nkval; i++) {
		if (strcmp(tab->kval[i]->key, key) == 0) {
			const char *raw = tab->kval[i]->val;
			if (*raw == '"' || *raw == '\'') {
				char q = *raw++;
				size_t len = strlen(raw);
				if (len > 0 && raw[len - 1] == q) len--;
				char *s = malloc(len + 1);
				if (s) {
					size_t out = 0;
					for (size_t k = 0; k < len; k++) {
						if (q == '"' && raw[k] == '\\' && k + 1 < len) {
							k++;
							if (raw[k] == 'n') s[out++] = '\n';
							else if (raw[k] == 't') s[out++] = '\t';
							else if (raw[k] == 'r') s[out++] = '\r';
							else s[out++] = raw[k];
						} else {
							s[out++] = raw[k];
						}
					}
					s[out] = '\0';
					d.ok = true;
					d.u.s = s;
				}
			} else {
				d.ok = true;
				d.u.s = strdup(raw);
			}
			return d;
		}
	}
	return d;
}

toml_datum_t toml_bool_in(const toml_table_t *tab, const char *key) {
	toml_datum_t d = {0};
	if (!tab || !key) return d;
	for (int i = 0; i < tab->nkval; i++) {
		if (strcmp(tab->kval[i]->key, key) == 0) {
			const char *v = tab->kval[i]->val;
			if (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0) {
				d.ok = true;
				d.u.b = true;
			} else if (strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0) {
				d.ok = true;
				d.u.b = false;
			}
			return d;
		}
	}
	return d;
}

toml_datum_t toml_int_in(const toml_table_t *tab, const char *key) {
	toml_datum_t d = {0};
	if (!tab || !key) return d;
	for (int i = 0; i < tab->nkval; i++) {
		if (strcmp(tab->kval[i]->key, key) == 0) {
			char *end = NULL;
			long long val = strtoll(tab->kval[i]->val, &end, 0);
			if (end != tab->kval[i]->val) {
				d.ok = true;
				d.u.i = (int64_t)val;
			}
			return d;
		}
	}
	return d;
}

toml_table_t *toml_table_in(const toml_table_t *tab, const char *key) {
	if (!tab || !key) return NULL;
	for (int i = 0; i < tab->ntab; i++) {
		if (strcmp(tab->tab[i]->key, key) == 0) return tab->tab[i];
	}
	return NULL;
}

toml_array_t *toml_array_in(const toml_table_t *tab, const char *key) {
	if (!tab || !key) return NULL;
	for (int i = 0; i < tab->narr; i++) {
		if (strcmp(tab->arr[i]->key, key) == 0) return tab->arr[i];
	}
	return NULL;
}

int toml_array_nelem(const toml_array_t *arr) {
	return arr ? arr->nelem : 0;
}

toml_table_t *toml_table_at(const toml_array_t *arr, int idx) {
	if (!arr || idx < 0 || idx >= arr->nelem || arr->kind != 't') return NULL;
	return arr->u.tab[idx];
}
