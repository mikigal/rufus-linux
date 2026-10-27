/*
 * Rufus: The Reliable USB Formatting Utility
 * Unit test helpers for the questions that the core asks
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
 * test_ask() records the questions of the core and gives the answers that the
 * test scripted with answer(). Without localization, the titles and messages
 * are "MSG_xxx UNTRANSLATED", so asked_msg() identifies a question by its
 * title's message ID. A question that was not scripted is a failure.
 */

#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rufus.h"
#include "ui_iface.h"
#include "test.h"

#define MAX_QUESTIONS               32

typedef struct {
	enum ui_question_type type;
	char title[128];
	int flags;						// MB_* of a notification
	int nb_choices;					// For a selection
	char choice[16][128];
	int mask;						// Preselected entries of a selection
	int style;
} asked_t;

static asked_t asked[MAX_QUESTIONS];
static int nb_asked = 0;
static int answers[MAX_QUESTIONS];
static int nb_answers = 0, next_answer = 0;

static int test_ask(ui_question_t* q)
{
	asked_t* a = &asked[nb_asked < MAX_QUESTIONS ? nb_asked++ : MAX_QUESTIONS - 1];
	uint32_t i;

	memset(a, 0, sizeof(*a));
	a->type = q->type;
	static_strcpy(a->title, q->title != NULL ? q->title : "");
	if (q->type == UI_QUESTION_NOTIFICATION) {
		a->flags = q->notification.flags;
	} else if (q->type == UI_QUESTION_SELECTION) {
		a->nb_choices = (int)q->selection->choices.Index;
		for (i = 0; i < q->selection->choices.Index && i < ARRAYSIZE(a->choice); i++)
			static_strcpy(a->choice[i], q->selection->choices.String[i]);
		a->mask = q->selection->mask;
		a->style = q->selection->style;
	}
	if (q->type == UI_QUESTION_LIST)
		return 0;
	if (next_answer < nb_answers)
		return answers[next_answer++];
	fprintf(stderr, "Unexpected question: '%s'\n", a->title);
	test_failures++;
	return (q->type == UI_QUESTION_SELECTION) ? -1 : IDCANCEL;
}

// Script the answers to the next questions, and forget the previous ones
static void answer(int n, ...)
{
	va_list ap;
	int i;

	va_start(ap, n);
	for (i = 0; i < n && i < MAX_QUESTIONS; i++)
		answers[i] = va_arg(ap, int);
	va_end(ap);
	nb_answers = n;
	next_answer = 0;
	nb_asked = 0;
}

// Whether question #i has the title of message 'msg' (e.g. MSG_130)
static BOOL asked_msg(int i, int msg)
{
	char id[16];

	if (i >= nb_asked)
		return FALSE;
	snprintf(id, sizeof(id), "MSG_%03d ", msg - MSG_000);
	return (strncmp(asked[i].title, id, strlen(id)) == 0);
}

#define CHECK_ALL_ANSWERED() do { \
	CHECK_EQ_INT(next_answer, nb_answers); \
	CHECK_EQ_INT(nb_asked, nb_answers); \
} while (0)
