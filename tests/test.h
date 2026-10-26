/*
 * Rufus: The Reliable USB Formatting Utility
 * Minimal unit test helpers
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

#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		test_failures++; \
	} \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
	long long _a = (long long)(a), _b = (long long)(b); \
	if (_a != _b) { \
		fprintf(stderr, "%s:%d: CHECK_EQ_INT failed: %s == %lld, %s == %lld\n", \
			__FILE__, __LINE__, #a, _a, #b, _b); \
		test_failures++; \
	} \
} while (0)

#define CHECK_EQ_STR(a, b) do { \
	const char *_a = (a), *_b = (b); \
	if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
		fprintf(stderr, "%s:%d: CHECK_EQ_STR failed: \"%s\" != \"%s\"\n", \
			__FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); \
		test_failures++; \
	} \
} while (0)

#define CHECK_EQ_MEM(a, b, len) do { \
	if (memcmp((a), (b), (len)) != 0) { \
		fprintf(stderr, "%s:%d: CHECK_EQ_MEM failed: %s != %s\n", __FILE__, __LINE__, #a, #b); \
		test_failures++; \
	} \
} while (0)

#define TEST_RESULT() (test_failures == 0 ? 0 : 1)
