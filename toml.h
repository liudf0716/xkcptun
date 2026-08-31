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
#ifndef TOML_H
#define TOML_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct toml_table_t toml_table_t;
typedef struct toml_array_t toml_array_t;

/* Parse a content. Return 0 on success, or errbuf will contain reason. */
toml_table_t *toml_parse(char *conf, char *errbuf, int errbufsz);

/* Parse a file. Return 0 on success, or errbuf will contain reason. */
toml_table_t *toml_parse_file(FILE *fp, char *errbuf, int errbufsz);

/* Free the table returned by toml_parse() or toml_parse_file(). */
void toml_free(toml_table_t *tab);

/* Value types returned by toml_*_in() or toml_*_at() */
typedef struct {
	bool ok;
	union {
		char *s;      /* string, must be freed by caller */
		int64_t i;    /* integer */
		double d;     /* double */
		bool b;       /* boolean */
		char *ts;     /* timestamp, must be freed by caller */
	} u;
} toml_datum_t;

/* Key traversal in a table. keyidx starts from 0 until returns NULL. */
const char *toml_key_in(const toml_table_t *tab, int keyidx);

/* Table access by key */
toml_datum_t toml_string_in(const toml_table_t *tab, const char *key);
toml_datum_t toml_bool_in(const toml_table_t *tab, const char *key);
toml_datum_t toml_int_in(const toml_table_t *tab, const char *key);
toml_datum_t toml_double_in(const toml_table_t *tab, const char *key);
toml_datum_t toml_timestamp_in(const toml_table_t *tab, const char *key);
toml_table_t *toml_table_in(const toml_table_t *tab, const char *key);
toml_array_t *toml_array_in(const toml_table_t *tab, const char *key);

/* Array access by index. idx starts from 0 to toml_array_nelem() - 1 */
int toml_array_nelem(const toml_array_t *arr);
toml_datum_t toml_string_at(const toml_array_t *arr, int idx);
toml_datum_t toml_bool_at(const toml_array_t *arr, int idx);
toml_datum_t toml_int_at(const toml_array_t *arr, int idx);
toml_datum_t toml_double_at(const toml_array_t *arr, int idx);
toml_datum_t toml_timestamp_at(const toml_array_t *arr, int idx);
toml_table_t *toml_table_at(const toml_array_t *arr, int idx);
toml_array_t *toml_array_at(const toml_array_t *arr, int idx);

#ifdef __cplusplus
}
#endif

#endif /* TOML_H */
