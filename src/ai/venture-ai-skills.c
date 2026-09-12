/*
 * venture-ai-skills.c - saved ways of asking the assistant
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A skill is a prompt behind a slash. The built-in ones are the questions
 * a business tool gets asked every day -- summarise this, draft a reply,
 * what should I do next -- and they lean on the page context every
 * question already carries, so "/summarise" on a ticket means that ticket.
 * The records let an operator add their own, in their own words, and
 * replace a built-in they would phrase differently.
 */

#include "venture.h"

#include <string.h>

/* The compiled-in skills. Triggers are lowercase; the menu shows them as
 * typed. */
static const struct
{
	const gchar *trigger;
	const gchar *name;
	const gchar *description;
	const gchar *prompt;
} venture_ai_builtin_skills[] = {
	{ "summarise", "Summarise",
	  "What this is, its state, what is open, who owns it, what is next",
	  "Summarise what I am looking at in at most six short bullets: what "
	  "it is, its current state, what is still open, who owns it, and "
	  "what should happen next. Read the record with venture_get if the "
	  "context is not enough. Do not invent anything the record does not "
	  "say." },
	{ "reply", "Draft a reply",
	  "A reply to the requester of this ticket, ready to send",
	  "Draft a reply to the requester of the ticket I am looking at. "
	  "Answer what they asked, say plainly what has been done and what "
	  "happens next, and keep it short. Use the ticket's comments for the "
	  "history. Give me only the reply text, ready to paste." },
	{ "triage", "Triage",
	  "Propose priority, owner, tags and the next step, with reasons",
	  "Triage what I am looking at: propose a priority, an assignee, tags "
	  "and the single next step, and say in one line each why. If it is a "
	  "list, do it for the items that most need it, worst first. Stage "
	  "the changes rather than describing them if I say so." },
	{ "next", "What next",
	  "What I should do next, ordered by impact",
	  "Across everything you can see -- tickets, promises, invoices, "
	  "incidents, runs -- what should I do next? Give me a short ordered "
	  "list, highest impact first, each with the one reason it is there "
	  "and a link to the record." },
	{ "standup", "Stand-up",
	  "Yesterday, today, blocked -- from tickets, sprints and runs",
	  "Give me a stand-up: what moved in the last day, what is planned "
	  "for today, and what is blocked, drawn from tickets, the current "
	  "sprint, incidents and agent runs. Three short sections, bullets, "
	  "with links." },
	{ "weekly", "Weekly review",
	  "Sales, spend, tickets, incidents, runs and cost for the week",
	  "Write a weekly review for the last seven days: sales and revenue, "
	  "expenses, tickets opened and closed, service levels missed, "
	  "incidents, and agent runs with their cost. Use the reports and the "
	  "records; give figures, not adjectives; end with three things to "
	  "watch next week." },
	{ "explain", "Explain",
	  "This page or record in plain language, for somebody new",
	  "Explain what I am looking at in plain language to somebody new to "
	  "this business: what it is for, what the important fields mean, and "
	  "what usually happens to it next." },
	{ "chase", "Chase",
	  "A polite payment reminder for the invoice I am looking at",
	  "Draft a short, polite payment reminder for the invoice I am looking "
	  "at, naming the amount, the due date and how to pay. Firm on the "
	  "second sentence, friendly everywhere else. Give me only the text." }
};

void
venture_ai_skill_info_free(VentureAiSkillInfo *info)
{
	if (NULL == info)
		return;

	g_free(info->trigger);
	g_free(info->name);
	g_free(info->description);
	g_free(info);
}

static VentureAiSkillInfo *
venture_ai_skill_info_new(
	const gchar	*trigger,
	const gchar	*name,
	const gchar	*description,
	gboolean	 builtin
){
	VentureAiSkillInfo *info;

	info = g_new0(VentureAiSkillInfo, 1);
	info->trigger = g_ascii_strdown(trigger, -1);
	info->name = g_strdup((NULL != name) ? name : trigger);
	info->description = g_strdup((NULL != description) ? description : "");
	info->builtin = builtin;

	return info;
}

static gint
venture_ai_skill_info_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureAiSkillInfo *const *left = a;
	const VentureAiSkillInfo *const *right = b;

	return g_strcmp0((*left)->trigger, (*right)->trigger);
}

/*
 * Whether the ai_skill type is registered at all: the chat module can be
 * off, in which case there are no records and the built-ins stand alone.
 */
static gboolean
venture_ai_skills_type_is_on(VentureContext *context)
{
	return G_TYPE_INVALID != venture_entity_registry_lookup(
		venture_context_get_entity_registry(context), "ai_skill");
}

/*
 * The enabled records, or an empty array when the type is off.
 *
 * Returns: (transfer container) (element-type VentureEntity): the records
 */
static GPtrArray *
venture_ai_skills_records(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	GPtrArray *found;

	if (!venture_ai_skills_type_is_on(context))
		return g_ptr_array_new_with_free_func(g_object_unref);

	query = venture_query_new(VENTURE_TYPE_AI_SKILL);
	found = venture_database_find(venture_context_get_database(context),
	                              query, error);

	return found;
}

GPtrArray *
venture_ai_skills_list(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(GPtrArray) records = NULL;
	g_autoptr(GHashTable) seen = NULL;
	GPtrArray *skills;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	records = venture_ai_skills_records(context, error);

	if (NULL == records)
		return NULL;

	skills = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_ai_skill_info_free);
	seen = g_hash_table_new(g_str_hash, g_str_equal);

	/* Records first, so a record's trigger shadows a built-in's. */
	for (i = 0; i < records->len; i++)
	{
		VentureEntity *record;
		g_autofree gchar *trigger = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *description = NULL;
		VentureAiSkillInfo *info;
		gboolean enabled;

		record = g_ptr_array_index(records, i);
		g_object_get(record, "trigger", &trigger, "name", &name,
		             "description", &description, "enabled", &enabled, NULL);

		if (!enabled || venture_string_is_empty(trigger))
			continue;

		info = venture_ai_skill_info_new(trigger, name, description, FALSE);

		if (g_hash_table_contains(seen, info->trigger))
		{
			venture_ai_skill_info_free(info);
			continue;
		}

		g_hash_table_add(seen, info->trigger);
		g_ptr_array_add(skills, info);
	}

	for (i = 0; i < G_N_ELEMENTS(venture_ai_builtin_skills); i++)
	{
		if (g_hash_table_contains(seen,
		                          venture_ai_builtin_skills[i].trigger))
			continue;

		g_ptr_array_add(skills, venture_ai_skill_info_new(
			venture_ai_builtin_skills[i].trigger,
			venture_ai_builtin_skills[i].name,
			venture_ai_builtin_skills[i].description, TRUE));
	}

	g_ptr_array_sort(skills, venture_ai_skill_info_compare);

	return skills;
}

/*
 * Fills the prompt in: {input} becomes the text after the trigger, or the
 * text is appended as instructions when there is no placeholder. An
 * empty input removes the placeholder and adds nothing.
 */
static gchar *
venture_ai_skills_fill(
	const gchar	*prompt,
	const gchar	*input
){
	g_autoptr(GString) out = NULL;
	const gchar *placeholder;

	placeholder = strstr(prompt, "{input}");

	if (NULL != placeholder)
	{
		out = g_string_new_len(prompt, placeholder - prompt);
		g_string_append(out, input);
		g_string_append(out, placeholder + strlen("{input}"));

		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	out = g_string_new(prompt);

	if (!venture_string_is_empty(input))
	{
		g_string_append(out, "\n\nInstructions: ");
		g_string_append(out, input);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

gchar *
venture_ai_skills_expand(
	VentureContext	*context,
	const gchar	*message
){
	g_autofree gchar *trigger = NULL;
	g_autoptr(GPtrArray) records = NULL;
	const gchar *rest;
	const gchar *end;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (venture_string_is_empty(message) || ('/' != message[0]))
		return NULL;

	/* The trigger is the first word; a slash alone is not one. */
	end = message + 1;

	while (('\0' != *end) && !g_ascii_isspace(*end))
		end++;

	if (end == message + 1)
		return NULL;

	trigger = g_ascii_strdown(message + 1, end - (message + 1));
	rest = end;

	while (g_ascii_isspace(*rest))
		rest++;

	/* A record with this trigger wins over a built-in. */
	records = venture_ai_skills_records(context, NULL);

	for (i = 0; (NULL != records) && (i < records->len); i++)
	{
		VentureEntity *record;
		g_autofree gchar *candidate = NULL;
		g_autofree gchar *prompt = NULL;
		gboolean enabled;

		record = g_ptr_array_index(records, i);
		g_object_get(record, "trigger", &candidate, "prompt", &prompt,
		             "enabled", &enabled, NULL);

		if (!enabled || venture_string_is_empty(candidate) ||
		    venture_string_is_empty(prompt) ||
		    (0 != g_ascii_strcasecmp(candidate, trigger)))
			continue;

		return venture_ai_skills_fill(prompt, rest);
	}

	for (i = 0; i < G_N_ELEMENTS(venture_ai_builtin_skills); i++)
	{
		if (0 != g_strcmp0(venture_ai_builtin_skills[i].trigger, trigger))
			continue;

		return venture_ai_skills_fill(venture_ai_builtin_skills[i].prompt,
		                              rest);
	}

	return NULL;
}
