/*
 * venture-ai-harness.c - the assistant's harness, on the web
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ai-glib's harness, wired to a browser and to this install's records.
 * The shape is the one `ai-tui` uses: a resource registry scans the
 * directories agent tools keep commands in, a command set resolves a
 * typed line against them, and a completion context answers "what could
 * this be" for the text at the cursor. What differs is the front end --
 * a composer in a panel rather than a terminal -- and what an `@` names.
 */

#include "venture.h"

#include <string.h>

/* How many candidates a menu is given. Past this it stops being a menu. */
#define VENTURE_HARNESS_MAX_ITEMS (12)

/* How much of a long text field a record description carries. */
#define VENTURE_HARNESS_FIELD_LIMIT (600)

/* And how much of a whole record. */
#define VENTURE_HARNESS_RECORD_LIMIT (3000)

struct _VentureAiHarness
{
	GObject parent_instance;

	VentureContext		*context;

	/*
	 * The library's three pieces, in the order ai-tui builds them: the
	 * registry finds resource files, the command set turns them into
	 * commands and resolves a line, and the completion context answers
	 * for the text at a cursor.
	 */
	AiResourceRegistry	*registry;
	AiCommandSet		*commands;
	AiCompletionContext	*completion;
};

G_DEFINE_FINAL_TYPE(VentureAiHarness, venture_ai_harness, G_TYPE_OBJECT)

void
venture_harness_item_free(VentureHarnessItem *item)
{
	if (NULL == item)
		return;

	g_free(item->insert);
	g_free(item->label);
	g_free(item->name);
	g_free(item->description);
	g_free(item->origin);
	g_free(item);
}

static VentureHarnessItem *
venture_harness_item_new(
	VentureHarnessCompletion	 kind,
	const gchar			*insert,
	const gchar			*label,
	const gchar			*name,
	const gchar			*description,
	const gchar			*origin
){
	VentureHarnessItem *item;

	item = g_new0(VentureHarnessItem, 1);
	item->kind = kind;
	item->insert = g_strdup(insert);
	item->label = g_strdup(label);
	item->name = g_strdup((NULL != name) ? name : "");
	item->description = g_strdup((NULL != description) ? description : "");
	item->origin = g_strdup((NULL != origin) ? origin : "");

	return item;
}

static void
venture_ai_harness_finalize(GObject *object)
{
	VentureAiHarness *self = VENTURE_AI_HARNESS(object);

	g_clear_object(&self->completion);
	g_clear_object(&self->commands);
	g_clear_object(&self->registry);
	g_clear_object(&self->context);

	G_OBJECT_CLASS(venture_ai_harness_parent_class)->finalize(object);
}

static void
venture_ai_harness_class_init(VentureAiHarnessClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_ai_harness_finalize;
}

static void
venture_ai_harness_init(VentureAiHarness *self)
{
	self->registry = ai_resource_registry_new();
	self->commands = ai_command_set_new(self->registry);
	self->completion = ai_completion_context_new(self->commands, NULL);

	/*
	 * A command body's `` !`cmd` `` stays literal.
	 *
	 * The directories these come from are shared with every other agent
	 * tool on the machine, and this process answers a network port.
	 * Opt-in is the library's default and would already require a file
	 * to say `shell: true`, but a server should not be one edited file
	 * away from running commands, so it is turned off outright.
	 */
	ai_command_set_set_shell_policy(self->commands, AI_COMMAND_SHELL_NEVER);

	ai_completion_context_set_max_items(self->completion,
	                                    VENTURE_HARNESS_MAX_ITEMS);
}

VentureAiHarness *
venture_ai_harness_new(VentureContext *context)
{
	VentureAiHarness *self;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	self = g_object_new(VENTURE_TYPE_AI_HARNESS, NULL);
	self->context = g_object_ref(context);

	ai_resource_registry_scan(self->registry);

	return self;
}

void
venture_ai_harness_set_working_directory(
	VentureAiHarness	*self,
	const gchar		*path
){
	g_return_if_fail(VENTURE_IS_AI_HARNESS(self));

	ai_resource_registry_set_working_directory(self->registry, path);
	ai_completion_context_set_working_directory(self->completion, path);
	ai_resource_registry_scan(self->registry);
}

void
venture_ai_harness_refresh(VentureAiHarness *self)
{
	g_return_if_fail(VENTURE_IS_AI_HARNESS(self));

	ai_resource_registry_scan(self->registry);
}

/* --- Where the cursor is ---------------------------------------------------- */

/*
 * A token starts a word when it is at the start of the buffer or after a
 * space. It is what stops "C#" and "issue#3" from opening a menu, and it
 * is the same rule the retrieval code applies to a #base.
 */
static gboolean
venture_ai_harness_at_boundary(
	const gchar	*buffer,
	guint		 offset
){
	return (0 == offset) || g_ascii_isspace(buffer[offset - 1]);
}

/*
 * Walks back from the cursor to the sigil that governs it.
 *
 * Returns the kind, and where the token starts. Anything with a space in
 * it has stopped being a token: a person who typed "@ticket 12" has moved
 * on, and a menu that reopened behind them would be in the way.
 */
static VentureHarnessCompletion
venture_ai_harness_locate(
	const gchar	*buffer,
	guint		 cursor,
	guint		*out_start
){
	guint i;

	if (NULL == buffer)
		return VENTURE_HARNESS_COMPLETION_NONE;

	if (cursor > strlen(buffer))
		cursor = (guint)strlen(buffer);

	for (i = cursor; i > 0; i--)
	{
		gchar c;

		c = buffer[i - 1];

		if (g_ascii_isspace(c))
			break;

		if (('/' == c) || ('@' == c) || ('#' == c))
		{
			/*
			 * A slash is a command only at the very start of the
			 * composer. Anywhere else it is a date, a path or a
			 * fraction, and every one of those used to open a menu
			 * over what was being typed.
			 */
			if ('/' == c)
			{
				if (1 != i)
					continue;

				if (NULL != out_start)
					*out_start = i - 1;

				return VENTURE_HARNESS_COMPLETION_COMMAND;
			}

			if (!venture_ai_harness_at_boundary(buffer, i - 1))
				break;

			if (NULL != out_start)
				*out_start = i - 1;

			return ('@' == c)
				? VENTURE_HARNESS_COMPLETION_RECORD
				: VENTURE_HARNESS_COMPLETION_BASE;
		}
	}

	return VENTURE_HARNESS_COMPLETION_NONE;
}

VentureHarnessCompletion
venture_ai_harness_completion_kind(
	const gchar	*buffer,
	guint		 cursor
){
	return venture_ai_harness_locate(buffer, cursor, NULL);
}

/* --- Describing a record ---------------------------------------------------- */

/*
 * One JSON value, the way a person reads it rather than the way it
 * serialises: money as money, a string unquoted, a flag as yes or no.
 */
static gchar *
venture_ai_harness_value(JsonNode *value)
{
	if (JSON_NODE_HOLDS_VALUE(value))
	{
		GType held;

		held = json_node_get_value_type(value);

		if (G_TYPE_STRING == held)
			return g_strdup(json_node_get_string(value));

		if (G_TYPE_BOOLEAN == held)
			return g_strdup(json_node_get_boolean(value)
				? "yes" : "no");
	}

	if (JSON_NODE_HOLDS_OBJECT(value))
	{
		JsonObject *object;

		object = json_node_get_object(value);

		if (json_object_has_member(object, "amount") &&
		    json_object_has_member(object, "currency"))
		{
			g_autoptr(VentureMoney) money = NULL;

			money = venture_money_new(
				venture_json_object_get_int(object, "amount", 0),
				venture_json_object_get_string(object, "currency",
				                               "USD"),
				(guint)venture_json_object_get_int(object,
					"exponent", 2));

			if (NULL != money)
				return venture_money_to_display_string(money, TRUE);
		}
	}

	return venture_json_to_string(value, FALSE);
}

/* The spine and the timestamps are machinery. Shown to a model they bury
 * the two fields that matter under eight that never differ. */
static gboolean
venture_ai_harness_is_machinery(const gchar *name)
{
	static const gchar *const machinery[] = {
		"type", "id", "uuid", "organization_id", "created_at",
		"updated_at", "deleted_at", "version", "display_name",
		"attributes", NULL
	};

	return g_strv_contains(machinery, name);
}

gchar *
venture_ai_harness_describe_record(
	VentureAiHarness	*self,
	const gchar		*type_name,
	gint64			 id
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) out = NULL;
	g_autofree gchar *name = NULL;
	JsonObject *object;
	GList *members;
	GList *m;
	GType type;
	guint shown;

	g_return_val_if_fail(VENTURE_IS_AI_HARNESS(self), NULL);

	if (venture_string_is_empty(type_name) || (id <= 0))
		return NULL;

	type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(self->context), type_name);

	if (G_TYPE_INVALID == type)
		return NULL;

	record = venture_database_get(venture_context_get_database(self->context),
	                              type, id, NULL);

	if (NULL == record)
		return NULL;

	name = venture_entity_get_display_name(record);
	out = g_string_new(NULL);
	g_string_append_printf(out, "%s #%" G_GINT64_FORMAT " (\"%s\"):\n",
	                       type_name, id, (NULL != name) ? name : "");

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE);
	object = json_node_get_object(node);
	members = json_object_get_members(object);
	shown = 0;

	for (m = members; NULL != m; m = m->next)
	{
		g_autofree gchar *rendered = NULL;
		JsonNode *value;

		if (venture_ai_harness_is_machinery(m->data))
			continue;

		value = json_object_get_member(object, m->data);

		if ((NULL == value) || JSON_NODE_HOLDS_NULL(value))
			continue;

		rendered = venture_ai_harness_value(value);

		if (venture_string_is_empty(rendered))
			continue;

		/* A long text field is cut. The model has venture_get for the
		 * rest, and the point of this is orientation. */
		if (strlen(rendered) > VENTURE_HARNESS_FIELD_LIMIT)
		{
			g_autofree gchar *cut = NULL;

			cut = venture_truncate(rendered, VENTURE_HARNESS_FIELD_LIMIT);
			g_string_append_printf(out, "- %s: %s\n",
			                       (const gchar *)m->data, cut);
		}
		else
		{
			g_string_append_printf(out, "- %s: %s\n",
			                       (const gchar *)m->data, rendered);
		}

		shown++;

		if (out->len > VENTURE_HARNESS_RECORD_LIMIT)
		{
			g_string_append(out, "- (more fields not shown)\n");
			break;
		}
	}

	g_list_free(members);

	if (0 == shown)
		g_string_append(out, "- (no fields set)\n");

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- @record ---------------------------------------------------------------- */

/*
 * Splits `ticket/12` into its halves. The id half may be absent or
 * partial, which is the ordinary case while somebody is still typing.
 */
static void
venture_ai_harness_split_mention(
	const gchar	 *fragment,
	gchar		**out_type,
	gchar		**out_rest
){
	const gchar *slash;

	slash = strchr(fragment, '/');

	if (NULL == slash)
	{
		*out_type = g_strdup(fragment);
		*out_rest = NULL;
		return;
	}

	*out_type = g_strndup(fragment, slash - fragment);
	*out_rest = g_strdup(slash + 1);
}

/*
 * The record types an @ may name, filtered by what has been typed.
 *
 * Every registered type is offered rather than a hand-picked list: the
 * registry is what the modules decide, a plugin adds to it, and a
 * curated list here would be missing whatever was added last.
 */
static void
venture_ai_harness_complete_types(
	VentureAiHarness		*self,
	const gchar			*fragment,
	VentureHarnessAllowFunc		 allow,
	gpointer			 allow_data,
	GPtrArray			*items
){
	g_auto(GStrv) names = NULL;
	gsize i;

	names = venture_entity_registry_list_names(
		venture_context_get_entity_registry(self->context));

	for (i = 0; (NULL != names) && (NULL != names[i]); i++)
	{
		g_autofree gchar *insert = NULL;
		g_autofree gchar *label = NULL;
		GType type;

		if (!venture_string_is_empty(fragment) &&
		    (NULL == strstr(names[i], fragment)))
			continue;

		type = venture_entity_registry_lookup(
			venture_context_get_entity_registry(self->context),
			names[i]);

		if (G_TYPE_INVALID == type)
			continue;

		if ((NULL != allow) && !allow(type, allow_data))
			continue;

		/* No trailing space: the next thing to type is the slash and
		 * then which one, and the menu follows straight on. */
		insert = g_strdup_printf("@%s/", names[i]);
		label = g_strdup_printf("@%s", names[i]);

		g_ptr_array_add(items, venture_harness_item_new(
			VENTURE_HARNESS_COMPLETION_RECORD, insert, label,
			names[i], "Name one of these", "record type"));

		if (items->len >= VENTURE_HARNESS_MAX_ITEMS)
			return;
	}
}

/*
 * The records of one type, searched by what has been typed after the
 * slash. A bare `@ticket/` lists the most recent, which is what somebody
 * who has just opened the menu wants.
 */
static void
venture_ai_harness_complete_records(
	VentureAiHarness		*self,
	const gchar			*type_name,
	const gchar			*fragment,
	VentureHarnessAllowFunc		 allow,
	gpointer			 allow_data,
	GPtrArray			*items
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	GType type;
	guint i;

	type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(self->context), type_name);

	if (G_TYPE_INVALID == type)
		return;

	if ((NULL != allow) && !allow(type, allow_data))
		return;

	query = venture_query_new(type);

	/*
	 * All digits is an id, not a word: "@ticket/12" means ticket 12
	 * rather than every ticket with a 12 somewhere in its title.
	 */
	if (!venture_string_is_empty(fragment))
	{
		gboolean numeric;
		gsize c;

		numeric = TRUE;

		for (c = 0; '\0' != fragment[c]; c++)
		{
			if (!g_ascii_isdigit(fragment[c]))
			{
				numeric = FALSE;
				break;
			}
		}

		if (numeric)
			venture_query_add_filter_int(query, "id",
				VENTURE_FILTER_OP_EQ,
				g_ascii_strtoll(fragment, NULL, 10), NULL);
		else
			venture_query_set_search(query, fragment);
	}

	venture_query_set_limit(query, VENTURE_HARNESS_MAX_ITEMS);
	found = venture_database_find(venture_context_get_database(self->context),
	                              query, NULL);

	for (i = 0; (NULL != found) && (i < found->len); i++)
	{
		g_autofree gchar *insert = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *name = NULL;
		VentureEntity *record;
		gint64 id;

		record = g_ptr_array_index(found, i);
		id = venture_entity_get_id(record);
		name = venture_entity_get_display_name(record);

		insert = g_strdup_printf("@%s/%" G_GINT64_FORMAT " ", type_name, id);
		label = g_strdup_printf("@%s/%" G_GINT64_FORMAT, type_name, id);

		g_ptr_array_add(items, venture_harness_item_new(
			VENTURE_HARNESS_COMPLETION_RECORD, insert, label,
			(NULL != name) ? name : "", "", type_name));
	}
}

gchar *
venture_ai_harness_expand_mentions(
	VentureAiHarness		*self,
	const gchar			*text,
	VentureHarnessAllowFunc		 allow,
	gpointer			 allow_data
){
	g_autoptr(GString) out = NULL;
	g_autoptr(GHashTable) seen = NULL;
	GList *mentions;
	GList *m;

	g_return_val_if_fail(VENTURE_IS_AI_HARNESS(self), NULL);

	if (venture_string_is_empty(text) || (NULL == strchr(text, '@')))
		return NULL;

	/* The library's scanner, so an @ here means what an @ means in
	 * ai-tui: at a word boundary, up to whitespace, with the full stop
	 * that ended the sentence left out of it. */
	mentions = ai_mention_scan(text);
	out = g_string_new(NULL);
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (m = mentions; NULL != m; m = m->next)
	{
		AiMention *mention = m->data;
		g_autofree gchar *type_name = NULL;
		g_autofree gchar *rest = NULL;
		g_autofree gchar *described = NULL;
		GType type;
		gint64 id;

		if (NULL == mention->path)
			continue;

		venture_ai_harness_split_mention(mention->path, &type_name, &rest);

		if (venture_string_is_empty(rest))
			continue;

		id = g_ascii_strtoll(rest, NULL, 10);

		if (id <= 0)
			continue;

		type = venture_entity_registry_lookup(
			venture_context_get_entity_registry(self->context),
			type_name);

		if (G_TYPE_INVALID == type)
			continue;

		if ((NULL != allow) && !allow(type, allow_data))
			continue;

		/* Named twice in one question is one record. */
		if (g_hash_table_contains(seen, mention->path))
			continue;

		described = venture_ai_harness_describe_record(self, type_name, id);

		if (NULL == described)
			continue;

		g_hash_table_add(seen, g_strdup(mention->path));

		if (0 != out->len)
			g_string_append_c(out, '\n');

		g_string_append_printf(out, "@%s is the ", mention->path);
		g_string_append(out, described);
	}

	g_list_free_full(mentions, (GDestroyNotify)ai_mention_free);

	if (0 == out->len)
		return NULL;

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- /command --------------------------------------------------------------- */

/*
 * The commands, from both sides: this install's skills, then whatever the
 * resource directories hold. A skill shadows a file of the same name,
 * which is the same precedence expanding one uses.
 */
static void
venture_ai_harness_complete_commands(
	VentureAiHarness	*self,
	const gchar		*fragment,
	GPtrArray		*items
){
	g_autoptr(GPtrArray) skills = NULL;
	g_autoptr(GHashTable) seen = NULL;
	GList *commands;
	GList *c;
	guint i;

	seen = g_hash_table_new(g_str_hash, g_str_equal);
	skills = venture_ai_skills_list(self->context, NULL);

	for (i = 0; (NULL != skills) && (i < skills->len); i++)
	{
		VentureAiSkillInfo *skill;
		g_autofree gchar *insert = NULL;
		g_autofree gchar *label = NULL;

		skill = g_ptr_array_index(skills, i);
		g_hash_table_add(seen, skill->trigger);

		if (!venture_string_is_empty(fragment) &&
		    !g_str_has_prefix(skill->trigger, fragment) &&
		    (NULL == strstr(skill->name, fragment)))
			continue;

		insert = g_strdup_printf("/%s ", skill->trigger);
		label = g_strdup_printf("/%s", skill->trigger);

		g_ptr_array_add(items, venture_harness_item_new(
			VENTURE_HARNESS_COMPLETION_COMMAND, insert, label,
			skill->name, skill->description,
			skill->builtin ? "venture" : "yours"));
	}

	commands = ai_command_set_list(self->commands);

	for (c = commands; NULL != c; c = c->next)
	{
		AiCommand *command = c->data;
		const gchar *name;
		const gchar *description;
		const gchar *hint;
		g_autofree gchar *insert = NULL;
		g_autofree gchar *label = NULL;

		name = ai_command_get_name(command);

		if (venture_string_is_empty(name) ||
		    g_hash_table_contains(seen, name))
			continue;

		description = ai_command_get_description(command);
		hint = ai_command_get_argument_hint(command);

		if (!venture_string_is_empty(fragment) &&
		    !g_str_has_prefix(name, fragment))
			continue;

		insert = g_strdup_printf("/%s ", name);
		label = venture_string_is_empty(hint)
			? g_strdup_printf("/%s", name)
			: g_strdup_printf("/%s %s", name, hint);

		g_ptr_array_add(items, venture_harness_item_new(
			VENTURE_HARNESS_COMPLETION_COMMAND, insert, label, name,
			(NULL != description) ? description : "",
			ai_command_get_origin(command)));
	}

	g_list_free(commands);
}

gchar *
venture_ai_harness_expand(
	VentureAiHarness	*self,
	const gchar		*line
){
	g_autoptr(AiCommandResult) result = NULL;
	g_autofree gchar *native = NULL;

	g_return_val_if_fail(VENTURE_IS_AI_HARNESS(self), NULL);

	if (venture_string_is_empty(line) || ('/' != line[0]))
		return NULL;

	/* This install's own skills first: a record an operator wrote here
	 * is the most specific thing there is. */
	native = venture_ai_skills_expand(self->context, line);

	if (NULL != native)
		return g_steal_pointer(&native);

	if (!ai_command_set_is_command_line(line))
		return NULL;

	result = ai_command_set_resolve(self->commands, line, NULL, NULL, NULL);

	if (NULL == result)
		return NULL;

	/*
	 * A builtin is the frontend's to act on and an agent needs a
	 * subagent to run it; neither is a prompt, and the web composer has
	 * nowhere to put either yet. Left unexpanded, so the line is sent as
	 * the question it looks like rather than silently doing nothing.
	 */
	if (AI_COMMAND_OUTCOME_PROMPT != ai_command_result_get_outcome(result))
		return NULL;

	{
		const gchar *prompt;
		const gchar *arguments;
		AiCommand *command;
		AiResource *resource;

		prompt = ai_command_result_get_prompt(result);
		arguments = ai_command_result_get_arguments(result);

		if (venture_string_is_empty(prompt))
			return NULL;

		/*
		 * A command written without `$ARGUMENTS` still takes them.
		 * Substitution dropped anything typed after the name, which
		 * from the composer reads as the assistant ignoring half the
		 * sentence.
		 */
		command = ai_command_result_get_command(result);
		resource = (NULL != command) ? ai_command_get_resource(command) : NULL;

		if (!venture_string_is_empty(arguments) && (NULL != resource) &&
		    (NULL == strstr(ai_resource_get_body(resource), "$ARGUMENTS")))
			return g_strdup_printf("%s\n\nInstructions: %s", prompt,
			                       arguments);

		return g_strdup(prompt);
	}
}

/* --- #base ------------------------------------------------------------------ */

static void
venture_ai_harness_complete_bases(
	VentureAiHarness		*self,
	const gchar			*fragment,
	VentureHarnessAllowFunc		 allow,
	gpointer			 allow_data,
	GPtrArray			*items
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) bases = NULL;
	GType type;
	guint i;

	type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(self->context),
		"knowledge_base");

	if (G_TYPE_INVALID == type)
		return;

	if ((NULL != allow) && !allow(type, allow_data))
		return;

	query = venture_query_new(type);
	venture_query_set_limit(query, VENTURE_HARNESS_MAX_ITEMS);
	bases = venture_database_find(venture_context_get_database(self->context),
	                              query, NULL);

	for (i = 0; (NULL != bases) && (i < bases->len); i++)
	{
		g_autofree gchar *slug = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *description = NULL;
		g_autofree gchar *insert = NULL;
		g_autofree gchar *label = NULL;

		g_object_get(g_ptr_array_index(bases, i), "slug", &slug,
		             "name", &name, "description", &description, NULL);

		if (venture_string_is_empty(slug))
			continue;

		if (!venture_string_is_empty(fragment) &&
		    !g_str_has_prefix(slug, fragment))
			continue;

		insert = g_strdup_printf("#%s ", slug);
		label = g_strdup_printf("#%s", slug);

		g_ptr_array_add(items, venture_harness_item_new(
			VENTURE_HARNESS_COMPLETION_BASE, insert, label,
			(NULL != name) ? name : slug,
			(NULL != description) ? description : "",
			"knowledge base"));
	}
}

/* --- The one entry point ---------------------------------------------------- */

GPtrArray *
venture_ai_harness_complete(
	VentureAiHarness		 *self,
	const gchar			 *buffer,
	guint				  cursor,
	VentureHarnessAllowFunc		  allow,
	gpointer			  allow_data,
	guint				 *out_start,
	guint				 *out_end
){
	g_autofree gchar *fragment = NULL;
	GPtrArray *items;
	VentureHarnessCompletion kind;
	guint start;

	g_return_val_if_fail(VENTURE_IS_AI_HARNESS(self), NULL);

	start = 0;
	kind = venture_ai_harness_locate(buffer, cursor, &start);

	if (VENTURE_HARNESS_COMPLETION_NONE == kind)
		return NULL;

	if (cursor > strlen(buffer))
		cursor = (guint)strlen(buffer);

	/* What has been typed after the sigil. */
	fragment = g_ascii_strdown(buffer + start + 1, cursor - start - 1);

	if (NULL != out_start)
		*out_start = start;

	if (NULL != out_end)
		*out_end = cursor;

	items = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_harness_item_free);

	switch (kind)
	{
	case VENTURE_HARNESS_COMPLETION_COMMAND:
		venture_ai_harness_complete_commands(self, fragment, items);
		break;

	case VENTURE_HARNESS_COMPLETION_RECORD:
	{
		g_autofree gchar *type_name = NULL;
		g_autofree gchar *rest = NULL;

		venture_ai_harness_split_mention(fragment, &type_name, &rest);

		if (NULL == rest)
			venture_ai_harness_complete_types(self, type_name, allow,
			                                  allow_data, items);
		else
			venture_ai_harness_complete_records(self, type_name, rest,
			                                    allow, allow_data,
			                                    items);

		break;
	}

	case VENTURE_HARNESS_COMPLETION_BASE:
		venture_ai_harness_complete_bases(self, fragment, allow,
		                                  allow_data, items);
		break;

	default:
		break;
	}

	return items;
}

/* --- What is available ------------------------------------------------------ */

GPtrArray *
venture_ai_harness_list(
	VentureAiHarness	*self,
	AiResourceKind		 kind
){
	GPtrArray *items;
	GList *resources;
	GList *r;

	g_return_val_if_fail(VENTURE_IS_AI_HARNESS(self), NULL);

	items = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_harness_item_free);
	resources = ai_resource_registry_list(self->registry, kind);

	for (r = resources; NULL != r; r = r->next)
	{
		AiResource *resource = r->data;
		const gchar *name;
		g_autofree gchar *label = NULL;

		name = ai_resource_get_name(resource);

		if (venture_string_is_empty(name))
			continue;

		label = (AI_RESOURCE_COMMAND == kind)
			? g_strdup_printf("/%s", name)
			: g_strdup(name);

		g_ptr_array_add(items, venture_harness_item_new(
			VENTURE_HARNESS_COMPLETION_COMMAND, name, label, name,
			ai_resource_get_description(resource),
			ai_resource_get_origin(resource)));
	}

	g_list_free(resources);

	return items;
}

gchar **
venture_ai_harness_search_paths(
	VentureAiHarness	*self,
	AiResourceKind		 kind
){
	g_return_val_if_fail(VENTURE_IS_AI_HARNESS(self), NULL);

	return ai_resource_registry_get_search_paths(self->registry, kind);
}
