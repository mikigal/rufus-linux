/*
 * Rufus: The Reliable USB Formatting Utility
 * A minimal JSON parser
 * Copyright © 2026 Rufus for Linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * A recursive descent parser for RFC 8259 JSON, into a tree of values, for the
 * replies of the Microsoft download site that Fido parses with PowerShell.
 */

#include <stdlib.h>
#include <string.h>

#include "rufus.h"
#include "json.h"

#define JSON_MAX_DEPTH              64

typedef struct {
	const char* p;
	const char* end;
	int depth;
} parser_t;

static json_value_t* ParseValue(parser_t* ps);

static void SkipSpaces(parser_t* ps)
{
	while ((ps->p < ps->end) && ((*ps->p == ' ') || (*ps->p == '\t') || (*ps->p == '\r') || (*ps->p == '\n')))
		ps->p++;
}

static BOOL Literal(parser_t* ps, const char* word)
{
	size_t len = strlen(word);

	if (((size_t)(ps->end - ps->p) < len) || (memcmp(ps->p, word, len) != 0))
		return FALSE;
	ps->p += len;
	return TRUE;
}

static int HexDigit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static BOOL ParseHex4(parser_t* ps, uint32_t* cp)
{
	int i, d;

	if (ps->end - ps->p < 4)
		return FALSE;
	for (*cp = 0, i = 0; i < 4; i++) {
		d = HexDigit(ps->p[i]);
		if (d < 0)
			return FALSE;
		*cp = (*cp << 4) | (uint32_t)d;
	}
	ps->p += 4;
	return TRUE;
}

static size_t PutUtf8(char* out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	} else if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	} else if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

// A string, after its opening quote, as allocated UTF-8
static char* ParseString(parser_t* ps)
{
	const char* start = ps->p;
	char *str, *q;
	uint32_t cp, low;

	// The unescaped string is never longer than the escaped one
	while ((ps->p < ps->end) && (*ps->p != '"')) {
		if (*ps->p == '\\')
			ps->p++;
		ps->p++;
	}
	if (ps->p >= ps->end)
		return NULL;
	str = malloc((size_t)(ps->p - start) + 1);
	if (str == NULL)
		return NULL;
	ps->end = ps->p;	// Temporarily, to unescape [start, closing quote)
	ps->p = start;
	for (q = str; ps->p < ps->end; ) {
		if ((unsigned char)*ps->p < 0x20)
			goto error;
		if (*ps->p != '\\') {
			*q++ = *ps->p++;
			continue;
		}
		ps->p++;
		switch (*ps->p++) {
		case '"': *q++ = '"'; break;
		case '\\': *q++ = '\\'; break;
		case '/': *q++ = '/'; break;
		case 'b': *q++ = '\b'; break;
		case 'f': *q++ = '\f'; break;
		case 'n': *q++ = '\n'; break;
		case 'r': *q++ = '\r'; break;
		case 't': *q++ = '\t'; break;
		case 'u':
			if (!ParseHex4(ps, &cp))
				goto error;
			// A surrogate pair
			if ((cp >= 0xd800) && (cp < 0xdc00)) {
				if ((ps->end - ps->p < 6) || (ps->p[0] != '\\') || (ps->p[1] != 'u'))
					goto error;
				ps->p += 2;
				if (!ParseHex4(ps, &low) || (low < 0xdc00) || (low > 0xdfff))
					goto error;
				cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
			} else if ((cp >= 0xdc00) && (cp <= 0xdfff)) {
				goto error;
			}
			// A NUL would end the string
			if (cp == 0)
				goto error;
			q += PutUtf8(q, cp);
			break;
		default:
			goto error;
		}
	}
	*q = 0;
	return str;

error:
	free(str);
	return NULL;
}

static json_value_t* NewValue(json_type_t type)
{
	json_value_t* v = calloc(1, sizeof(json_value_t));

	if (v != NULL)
		v->type = type;
	return v;
}

static json_value_t* ParseString_(parser_t* ps)
{
	const char* end = ps->end;
	json_value_t* v = NewValue(JSON_STRING);

	if (v == NULL)
		return NULL;
	ps->p++;
	v->string = ParseString(ps);
	ps->end = end;
	if (v->string == NULL) {
		free(v);
		return NULL;
	}
	ps->p++;	// The closing quote
	return v;
}

static json_value_t* ParseNumber(parser_t* ps)
{
	char buf[64], *endp;
	const char* start = ps->p;
	json_value_t* v;
	size_t len;

	while ((ps->p < ps->end) && (strchr("+-0123456789.eE", *ps->p) != NULL))
		ps->p++;
	len = (size_t)(ps->p - start);
	if ((len == 0) || (len >= sizeof(buf)))
		return NULL;
	memcpy(buf, start, len);
	buf[len] = 0;
	v = NewValue(JSON_NUMBER);
	if (v == NULL)
		return NULL;
	v->number = strtod(buf, &endp);
	if (*endp != 0) {
		free(v);
		return NULL;
	}
	return v;
}

// An array or an object, from its opening bracket
static json_value_t* ParseContainer(parser_t* ps, BOOL object)
{
	json_value_t *v = NewValue(object ? JSON_OBJECT : JSON_ARRAY), *item, **last;
	const char* end;
	char* key = NULL;

	if (v == NULL)
		return NULL;
	if (++ps->depth > JSON_MAX_DEPTH)
		goto error;
	last = &v->child;
	ps->p++;
	SkipSpaces(ps);
	if ((ps->p < ps->end) && (*ps->p == (object ? '}' : ']'))) {
		ps->p++;
		ps->depth--;
		return v;
	}
	for (;;) {
		SkipSpaces(ps);
		if (object) {
			if ((ps->p >= ps->end) || (*ps->p != '"'))
				goto error;
			ps->p++;
			end = ps->end;
			key = ParseString(ps);
			ps->end = end;
			if (key == NULL)
				goto error;
			ps->p++;
			SkipSpaces(ps);
			if ((ps->p >= ps->end) || (*ps->p != ':'))
				goto error;
			ps->p++;
		}
		item = ParseValue(ps);
		if (item == NULL)
			goto error;
		item->key = key;
		key = NULL;
		*last = item;
		last = &item->next;
		SkipSpaces(ps);
		if (ps->p >= ps->end)
			goto error;
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p != (object ? '}' : ']'))
			goto error;
		ps->p++;
		ps->depth--;
		return v;
	}

error:
	free(key);
	JsonFree(v);
	return NULL;
}

static json_value_t* ParseValue(parser_t* ps)
{
	json_value_t* v;

	SkipSpaces(ps);
	if (ps->p >= ps->end)
		return NULL;
	switch (*ps->p) {
	case '{':
		return ParseContainer(ps, TRUE);
	case '[':
		return ParseContainer(ps, FALSE);
	case '"':
		return ParseString_(ps);
	case 't':
	case 'f':
		v = NewValue(JSON_BOOL);
		if ((v != NULL) && (Literal(ps, "true") || Literal(ps, "false"))) {
			v->number = (ps->p[-1] == 'e' && ps->p[-2] == 'u') ? 1 : 0;
			return v;
		}
		free(v);
		return NULL;
	case 'n':
		return Literal(ps, "null") ? NewValue(JSON_NULL) : NULL;
	default:
		return ParseNumber(ps);
	}
}

json_value_t* JsonParse(const char* text, size_t len)
{
	parser_t ps = { text, text + len, 0 };
	json_value_t* v;

	if (text == NULL)
		return NULL;
	// A UTF-8 BOM
	if ((len >= 3) && (memcmp(text, "\xef\xbb\xbf", 3) == 0))
		ps.p += 3;
	v = ParseValue(&ps);
	SkipSpaces(&ps);
	if ((v != NULL) && (ps.p != ps.end)) {
		JsonFree(v);
		return NULL;
	}
	return v;
}

void JsonFree(json_value_t* value)
{
	json_value_t* next;

	while (value != NULL) {
		next = value->next;
		JsonFree(value->child);
		free(value->key);
		free(value->string);
		free(value);
		value = next;
	}
}

json_value_t* JsonGet(const json_value_t* object, const char* key)
{
	json_value_t* v;

	if ((object == NULL) || (object->type != JSON_OBJECT))
		return NULL;
	for (v = object->child; v != NULL; v = v->next) {
		if (strcmp(v->key, key) == 0)
			return v;
	}
	return NULL;
}

const char* JsonGetString(const json_value_t* object, const char* key)
{
	json_value_t* v = JsonGet(object, key);

	return ((v != NULL) && (v->type == JSON_STRING)) ? v->string : NULL;
}
