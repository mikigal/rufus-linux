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

#pragma once

#include "rufus.h"

/* The replies of the APIs of the Microsoft download site, for Fido (fido.c) */

typedef enum {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT,
} json_type_t;

typedef struct json_value {
	json_type_t type;
	char* key;						// The name of a member of an object
	char* string;					// JSON_STRING, as UTF-8
	double number;					// JSON_NUMBER, and JSON_BOOL (0 or 1)
	struct json_value* child;		// The first element of an array or member of an object
	struct json_value* next;		// The next element or member
} json_value_t;

// Parse a document, or return NULL if it is not valid JSON
extern json_value_t* JsonParse(const char* text, size_t len);
extern void JsonFree(json_value_t* value);
// The member of an object with this name (case sensitive), or NULL
extern json_value_t* JsonGet(const json_value_t* object, const char* key);
// The string member of an object, or NULL if it isn't one
extern const char* JsonGetString(const json_value_t* object, const char* key);
