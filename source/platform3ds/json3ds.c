#include "json3ds.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Finds `"key"` followed by a colon, at any depth. Every object parsed here is
 * flat, so depth tracking would be unused weight -- but a key that could also
 * appear nested would make this ambiguous, which is why the header says so. */
static const char *find_value(const char *json, const char *key)
{
	size_t klen = strlen(key);
	for (const char *p = json; (p = strchr(p, '"')); p++) {
		if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"')
			continue;
		const char *q = p + 2 + klen;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
			q++;
		if (*q != ':')
			continue;
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
			q++;
		return q;
	}
	return NULL;
}

/* Four hex digits after a "\u", as a code unit; -1 when they are not. */
static long hex4(const char *p, const char *end)
{
	if (end - p < 4)
		return -1;
	long v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		int d = (c >= '0' && c <= '9') ? c - '0'
		      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
		      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if (d < 0)
			return -1;
		v = v << 4 | d;
	}
	return v;
}

/* `cp` as UTF-8; how many bytes. */
static size_t utf8_put(long cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | cp >> 12);
		out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | cp >> 18);
	out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
	out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* Decodes the string whose opening quote is at `v`, reading no further than
 * `end`. A "\u" escape becomes UTF-8 -- a surrogate pair one 4-byte
 * character, a lone surrogate '?' -- since the services send text as UTF-8
 * and escape only what JSON makes them (emoji, as pairs). */
static bool decode_string(const char *v, const char *end, char *out,
                          size_t outlen)
{
	if (v >= end || *v != '"')
		return false;
	v++;

	size_t n = 0;
	for (; v < end && *v && *v != '"'; v++) {
		char enc[4];
		size_t k = 1;
		if (*v != '\\') {
			enc[0] = *v;
		} else {
			if (++v >= end)
				return false;
			switch (*v) {
			case 'n': enc[0] = '\n'; break;
			case 'r': enc[0] = '\r'; break;
			case 't': enc[0] = '\t'; break;
			case 'b': enc[0] = '\b'; break;
			case 'f': enc[0] = '\f'; break;
			case '"': case '\\': case '/': enc[0] = *v; break;
			case 'u': {
				long cp = hex4(v + 1, end);
				if (cp < 0)
					return false;
				v += 4;
				if (cp >= 0xD800 && cp <= 0xDBFF && end - v >= 7 &&
				    v[1] == '\\' && v[2] == 'u') {
					long lo = hex4(v + 3, end);
					if (lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						v += 6;
					}
				}
				if (cp >= 0xD800 && cp <= 0xDFFF)
					cp = '?';          /* a surrogate left alone */
				if (cp == 0)
					cp = '?';          /* would end the C string */
				k = utf8_put(cp, enc);
				break;
			}
			case '\0': return false;
			default:   enc[0] = *v; break;
			}
		}
		if (n + k >= outlen)
			return false;              /* truncation is a failure, not a value */
		memcpy(out + n, enc, k);
		n += k;
	}
	if (v >= end || *v != '"')
		return false;                  /* unterminated string */
	out[n] = '\0';
	return true;
}

bool json_str(const char *json, const char *key, char *out, size_t outlen)
{
	if (!json || !out || outlen == 0)
		return false;
	out[0] = '\0';

	const char *v = find_value(json, key);
	if (!v || !decode_string(v, v + strlen(v), out, outlen)) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool json_slice_str(const char *v, size_t len, char *out, size_t outlen)
{
	if (!v || !out || outlen == 0)
		return false;
	out[0] = '\0';
	if (!decode_string(v, v + len, out, outlen)) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool json_int(const char *json, const char *key, long *out)
{
	const char *v = find_value(json, key);
	if (!v || (!isdigit((unsigned char)*v) && *v != '-'))
		return false;
	char *end = NULL;
	long n = strtol(v, &end, 10);
	if (end == v)
		return false;
	if (out)
		*out = n;
	return true;
}

/* Steps over a JSON string starting at the opening quote; returns the index
 * just past the closing quote, honouring backslash escapes. */
static size_t skip_string(const char *s, size_t at)
{
	at++;                                   /* the opening quote */
	while (s[at]) {
		if (s[at] == '\\' && s[at + 1])
			at += 2;
		else if (s[at] == '"')
			return at + 1;
		else
			at++;
	}
	return at;
}

bool json_array_next(const char *json, const char *key, size_t *cursor,
                     const char **obj, size_t *objlen)
{
	if (!json || !cursor)
		return false;

	size_t at = *cursor;
	if (at == 0) {
		/* First call: find the key, then its opening bracket. */
		char needle[64];
		int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
		if (n < 0 || (size_t)n >= sizeof(needle))
			return false;
		const char *k = strstr(json, needle);
		const char *open = k ? strchr(k, '[') : NULL;
		if (!open)
			return false;
		at = (size_t)(open - json) + 1;
	}

	/* Find the next object at depth 1, skipping whitespace and commas. */
	while (json[at] == ' ' || json[at] == '\n' || json[at] == '\r' ||
	       json[at] == '\t' || json[at] == ',')
		at++;
	if (json[at] != '{') {
		*cursor = at;
		return false;                       /* ']' or anything else ends it */
	}

	size_t start = at;
	int depth = 0;
	while (json[at]) {
		if (json[at] == '"') {
			at = skip_string(json, at);
			continue;
		}
		if (json[at] == '{')
			depth++;
		else if (json[at] == '}') {
			depth--;
			if (depth == 0) {
				at++;
				*obj = json + start;
				*objlen = at - start;
				*cursor = at;
				return true;
			}
		}
		at++;
	}
	return false;                           /* unterminated */
}

/* ------------------------------------------------- bounded slice walking */

static const char *skip_ws(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

/* Just past the value starting at `p`, or NULL when it runs past `e`. */
static const char *skip_value(const char *p, const char *e)
{
	if (p >= e)
		return NULL;
	if (*p == '"') {
		for (p++; p < e; p++) {
			if (*p == '\\')
				p++;
			else if (*p == '"')
				return p + 1;
		}
		return NULL;
	}
	if (*p == '{' || *p == '[') {
		int depth = 0;
		while (p < e) {
			if (*p == '"') {
				p = skip_value(p, e);
				if (!p)
					return NULL;
				continue;
			}
			if (*p == '{' || *p == '[')
				depth++;
			else if (*p == '}' || *p == ']') {
				if (--depth == 0)
					return p + 1;
			}
			p++;
		}
		return NULL;
	}
	/* A number, true, false or null: up to the next delimiter. */
	const char *s = p;
	while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
	       *p != '\n' && *p != '\r' && *p != '\t')
		p++;
	return p > s ? p : NULL;
}

bool json_member(const char *json, size_t len, const char *key,
                 const char **out, size_t *outlen)
{
	if (!json || !key)
		return false;
	const char *e = json + len;
	const char *p = skip_ws(json, e);
	if (p >= e || *p != '{')
		return false;
	size_t klen = strlen(key);
	p++;
	for (;;) {
		p = skip_ws(p, e);
		if (p >= e || *p == '}')
			return false;
		if (*p != '"')
			return false;
		const char *k = p + 1;
		const char *ke = skip_value(p, e);
		if (!ke)
			return false;
		bool match = (size_t)(ke - 1 - k) == klen && memcmp(k, key, klen) == 0;
		p = skip_ws(ke, e);
		if (p >= e || *p != ':')
			return false;
		p = skip_ws(p + 1, e);
		const char *ve = skip_value(p, e);
		if (!ve)
			return false;
		if (match) {
			if (out)
				*out = p;
			if (outlen)
				*outlen = (size_t)(ve - p);
			return true;
		}
		p = skip_ws(ve, e);
		if (p < e && *p == ',')
			p++;
	}
}

bool json_path(const char *json, size_t len, const char *path,
               const char **out, size_t *outlen)
{
	if (!json || !path)
		return false;
	const char *v = json;
	size_t vl = len;
	while (*path) {
		const char *dot = strchr(path, '.');
		size_t n = dot ? (size_t)(dot - path) : strlen(path);
		char key[48];
		if (n == 0 || n >= sizeof(key))
			return false;
		memcpy(key, path, n);
		key[n] = '\0';
		if (!json_member(v, vl, key, &v, &vl))
			return false;
		path += n + (dot ? 1 : 0);
	}
	if (out)
		*out = v;
	if (outlen)
		*outlen = vl;
	return true;
}

bool json_item_next(const char *arr, size_t len, size_t *cursor,
                    const char **out, size_t *outlen)
{
	if (!arr || !cursor)
		return false;
	const char *e = arr + len;
	const char *p;
	if (*cursor == 0) {
		p = skip_ws(arr, e);
		if (p >= e || *p != '[')
			return false;
		p++;
	} else {
		p = arr + *cursor;
	}
	p = skip_ws(p, e);
	if (p < e && *p == ',')
		p = skip_ws(p + 1, e);
	if (p >= e || *p == ']')
		return false;
	const char *ve = skip_value(p, e);
	if (!ve)
		return false;
	if (out)
		*out = p;
	if (outlen)
		*outlen = (size_t)(ve - p);
	*cursor = (size_t)(ve - arr);
	return true;
}

bool json_member_str(const char *json, size_t len, const char *path,
                     char *out, size_t outlen)
{
	const char *v;
	size_t vl;
	if (out && outlen)
		out[0] = '\0';
	return json_path(json, len, path, &v, &vl) &&
	       json_slice_str(v, vl, out, outlen);
}

bool json_member_int(const char *json, size_t len, const char *path,
                     long *out)
{
	const char *v;
	size_t vl;
	if (!json_path(json, len, path, &v, &vl) || vl == 0 || vl >= 24)
		return false;
	if (!isdigit((unsigned char)*v) && *v != '-')
		return false;
	char num[24];
	memcpy(num, v, vl);
	num[vl] = '\0';
	char *end = NULL;
	long n = strtol(num, &end, 10);
	if (end == num)
		return false;
	if (out)
		*out = n;
	return true;
}
