/*
 * venture-mcp-catalog.c - The MCP tool surface, generated from the schema
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */


#include "mcp/venture-mcp-catalog.h"

#include "venture-error.h"
#include "util/venture-json-util.h"

#include <string.h>

/*
 * One record type, as the catalog needs it.
 *
 * `canonical` is the singular the REST path is built from; `plural` is the
 * other spelling of the same resource. `description` is the schema entry
 * kept whole, so `venture_schema` can hand back exactly what the server
 * said rather than a lossy re-rendering of it.
 */
typedef struct
{
	gchar		*canonical;
	gchar		*plural;
	JsonNode	*description;
} VentureMcpType;

struct _VentureMcpCatalog
{
	GObject		 parent_instance;

	/* Declaration order, which is the order the server listed them in.
	 * The type enum an agent reads should match the schema's own
	 * ordering rather than a hash table's. */
	GPtrArray	*types;

	/* Both spellings, lowercased, pointing at the owning entry's
	 * canonical string. One lookup answers either. */
	GHashTable	*by_name;
};

G_DEFINE_TYPE(VentureMcpCatalog, venture_mcp_catalog, G_TYPE_OBJECT)

/* --- The tool table ------------------------------------------------------ */

/*
 * Which generated pieces a tool's schema needs. The tool NAMES are fixed --
 * they are the verbs, not the nouns -- while everything that enumerates
 * record types is filled in from the schema.
 *
 * A per-type tool surface was the alternative and is the wrong shape: 31
 * built-in types times five verbs is 155 tools in every context window, and
 * `venturectl` already demonstrates that one generic command per verb is
 * enough. What must not be hand-written is the list of nouns, and it is not.
 */
typedef enum
{
	TOOL_FLAG_NONE		= 0,

	/* Carries a `type` parameter whose enum is the catalog's types. */
	TOOL_FLAG_TYPE		= 1 << 0,

	/* That `type` parameter is required. */
	TOOL_FLAG_TYPE_REQUIRED	= 1 << 1,

	/* Carries a required numeric `id`. */
	TOOL_FLAG_ID		= 1 << 2,

	/* Carries a required free-form `values` object. */
	TOOL_FLAG_VALUES	= 1 << 3
} VentureMcpToolFlags;

typedef struct
{
	const gchar		*name;
	const gchar		*description;
	VentureMcpToolFlags	 flags;
} VentureMcpToolDef;

static const VentureMcpToolDef tool_defs[] = {
	{
		"venture_schema",
		"List every record type this VENTURE server holds, or describe "
		"one of them: its fields in the spelling you have to type, what "
		"each reference points at, and what each enumeration accepts. "
		"Read this before writing a record -- most mistakes are a field "
		"name guessed rather than looked up.",
		TOOL_FLAG_TYPE
	},
	{
		"venture_list",
		"List records of one type, filtered. `filters` takes "
		"field=value for equality or field__operator=value for anything "
		"else; the operators are eq, ne, lt, lte, gt, gte, like, ilike, "
		"in, not_in, is_null, not_null and between. `period` accepts "
		"today, this_month, last_month, this_quarter, ytd, fy_2026, "
		"2026-Q2, 2026-01-01..2026-03-31 and all.",
		TOOL_FLAG_TYPE | TOOL_FLAG_TYPE_REQUIRED
	},
	{
		"venture_get",
		"Fetch one record by its numeric id.",
		TOOL_FLAG_TYPE | TOOL_FLAG_TYPE_REQUIRED | TOOL_FLAG_ID
	},
	{
		"venture_create",
		"Create a record. `values` names fields in their wire spelling "
		"-- underscores, never hyphens -- which venture_schema prints. "
		"Money accepts \"12.34\" or \"12.34 EUR\", dates accept "
		"YYYY-MM-DD, and an enumeration accepts its nick in any case.",
		TOOL_FLAG_TYPE | TOOL_FLAG_TYPE_REQUIRED | TOOL_FLAG_VALUES
	},
	{
		"venture_update",
		"Change named fields of one record. Only the fields present in "
		"`values` are touched; everything else keeps the value it has.",
		TOOL_FLAG_TYPE | TOOL_FLAG_TYPE_REQUIRED | TOOL_FLAG_ID |
			TOOL_FLAG_VALUES
	},
	{
		"venture_delete",
		"Delete a record. The delete is soft: the row stays and can be "
		"restored, so this is recoverable rather than destructive.",
		TOOL_FLAG_TYPE | TOOL_FLAG_TYPE_REQUIRED | TOOL_FLAG_ID
	},
	{
		"venture_reports",
		"List the reports this server can run, with what each one "
		"measures.",
		TOOL_FLAG_NONE
	},
	{
		"venture_report",
		"Run one report over a period. The result says which content "
		"type came back, because `format: csv` returns CSV rather than "
		"JSON and reading one as the other silently produces nonsense.",
		TOOL_FLAG_NONE
	},
	{
		"venture_confirmations",
		"List every change waiting for a person to approve it -- staged by "
		"this session's writes, by another agent, or by VENTURE's own "
		"assistant -- or approve or reject one of them by id.",
		TOOL_FLAG_NONE
	},
	{
		"venture_modules",
		"Which modules this server runs -- CRM, invoicing, the software "
		"factory, dashboards and the rest -- with what each owns and why "
		"any is off. A record type whose module is off is not in the "
		"schema and its routes answer 404.",
		TOOL_FLAG_NONE
	},
	{
		"venture_links",
		"Every link touching one record, read from it: what it blocks, "
		"depends on, produced, or is otherwise joined to, across every "
		"record type. Follow a thread from a release to the tickets it "
		"shipped, or from an incident to the deployment that caused it.",
		TOOL_FLAG_TYPE | TOOL_FLAG_TYPE_REQUIRED | TOOL_FLAG_ID
	},
	{
		"venture_link",
		"Link two records of any types with a kind that says what the "
		"link means read from the source: related, blocks, blocked_by, "
		"depends_on, required_by, parent_of, child_of, duplicates, "
		"causes, caused_by, produces, produced_by, references, "
		"referenced_by, supersedes, superseded_by. A write, so it is "
		"staged like venture_create unless the server was started with "
		"--apply-writes.",
		TOOL_FLAG_NONE
	},
	{
		"venture_dashboards",
		"The dashboards on this server (`what`: list), the catalogue of "
		"widget kinds a dashboard can hold (`what`: kinds -- each with the "
		"settings it reads), or the shipped templates with their full "
		"definitions (`what`: templates). Read kinds before building a "
		"dashboard.",
		TOOL_FLAG_NONE
	},
	{
		"venture_dashboard",
		"One dashboard by slug. `mode` data (the default) evaluates every "
		"widget and returns what each shows right now -- counts, lists, "
		"report figures, notes; settings lists the widgets without "
		"evaluating them; definition returns the portable JSON that "
		"venture_dashboard_build accepts, for copying or editing.",
		TOOL_FLAG_NONE
	},
	{
		"venture_dashboard_build",
		"Create a dashboard: from a shipped template by name, or from a "
		"definition -- {name, description, purpose, layout, widgets: "
		"[{kind, title, entity_type, filter, report_name, ...}]} as "
		"venture_dashboards (what: templates) shows. Every widget is "
		"checked before anything is written. A dashboard is layout, not "
		"business data, but it is still a write: it needs the server "
		"started with --apply-writes; otherwise use venture_create on "
		"dashboard and then dashboard_widget, which stage like any record.",
		TOOL_FLAG_NONE
	},
	{
		"venture_inbox",
		"The caller's inbox: mentions, tickets handed over, changes to "
		"watched records, service levels about to be missed, budgets "
		"crossed, runs finishing. `action` list (the default) reads it, "
		"unread only unless `unread` is false; read marks one notification "
		"read by `id`, or every one with id 0. Reading the inbox is not a "
		"write and needs no --apply-writes.",
		TOOL_FLAG_NONE
	},
	{
		"venture_runs",
		"Mission control for the coding runs. `what` runs (the default) "
		"lists them across every repository with state, runner, model, "
		"tokens, cost and duration plus totals -- live, succeeded, failed, "
		"pull requests, cost, cost per success -- narrowed by `state`; "
		"budgets lists the agent budgets with spend against limit for the "
		"current window.",
		TOOL_FLAG_NONE
	},
	{
		"venture_desk",
		"The workdesk. Reads: `action` sla (a ticket's service-level "
		"clocks, by id), activity (a record's timeline by type and id: "
		"every change with who and what moved, plus a ticket's comments "
		"and worklogs), sprints (each with its burn), sprint (one, with "
		"its tickets), triage (a proposed priority, issue type and tags "
		"for a ticket with a summary and the requester's sentiment -- it "
		"changes nothing), summarise (what a thread amounts to) and draft "
		"(the next reply, for a person to edit; never posted). Writes: worklog logs hours against a ticket and "
		"stages like any record; macro applies a macro by name to a "
		"ticket, fix_ticket opens the bug for an incident, bulk changes "
		"many records of a type at once (ids and changes, or delete: "
		"true) and sweep marks service levels that have fallen due -- "
		"those four cannot be staged and need --apply-writes.",
		TOOL_FLAG_NONE
	},
	{
		"venture_factory",
		"The software factory. `action` status (the default) is the loop "
		"at a glance: open milestones with progress, the newest releases, "
		"the latest CI builds, each environment with the release it runs, "
		"and open incidents. changelog drafts a release's changelog from "
		"the tickets marked as fixed in it (replace: true overwrites one "
		"somebody wrote); publish cuts the release on the git forge, "
		"creating the tag, which cannot be undone from here. The two "
		"writes need --apply-writes.",
		TOOL_FLAG_NONE
	}
};

/* --- VentureMcpType ------------------------------------------------------ */

static void
venture_mcp_type_free(gpointer data)
{
	VentureMcpType *type;

	type = data;

	if (NULL == type)
		return;

	g_free(type->canonical);
	g_free(type->plural);

	if (NULL != type->description)
		json_node_unref(type->description);

	g_free(type);
}

/* --- GObject ------------------------------------------------------------- */

static void
venture_mcp_catalog_finalize(GObject *object)
{
	VentureMcpCatalog *self;

	self = VENTURE_MCP_CATALOG(object);

	g_clear_pointer(&self->types, g_ptr_array_unref);
	g_clear_pointer(&self->by_name, g_hash_table_unref);

	G_OBJECT_CLASS(venture_mcp_catalog_parent_class)->finalize(object);
}

static void
venture_mcp_catalog_class_init(VentureMcpCatalogClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = venture_mcp_catalog_finalize;
}

static void
venture_mcp_catalog_init(VentureMcpCatalog *self)
{
	self->types = g_ptr_array_new_with_free_func(venture_mcp_type_free);
	self->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      NULL);
}

/* --- Construction -------------------------------------------------------- */

VentureMcpCatalog *
venture_mcp_catalog_new_from_schema(
	JsonNode	 *schema,
	GError		**error
){
	g_autoptr(VentureMcpCatalog) self = NULL;
	JsonArray *entries;
	guint i;
	guint length;

	g_return_val_if_fail(NULL != schema, NULL);

	/*
	 * A JsonNode can hold the array TYPE and no array, so the type check
	 * is not a pointer check. Every failure below names what arrived,
	 * because "the schema is malformed" sends the reader nowhere.
	 */
	if (!JSON_NODE_HOLDS_ARRAY(schema) ||
	    (NULL == json_node_get_array(schema)))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_SERIALIZATION,
		                    "GET /api/v1/schema did not return an array of "
		                    "record types. Check that VENTURE_URL names a "
		                    "VENTURE server and not something else "
		                    "answering on that port.");
		return NULL;
	}

	entries = json_node_get_array(schema);
	length = json_array_get_length(entries);

	if (0 == length)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_SERIALIZATION,
		                    "GET /api/v1/schema described no record types "
		                    "at all. A VENTURE server registers its "
		                    "built-in types at startup, so an empty schema "
		                    "means the response did not come from one.");
		return NULL;
	}

	self = g_object_new(VENTURE_TYPE_MCP_CATALOG, NULL);

	for (i = 0; i < length; i++)
	{
		VentureMcpType *type;
		JsonObject *entry;
		JsonNode *node;
		const gchar *name;
		const gchar *plural;

		node = json_array_get_element(entries, i);

		if (!JSON_NODE_HOLDS_OBJECT(node) ||
		    (NULL == json_node_get_object(node)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
			            "Entry %u of GET /api/v1/schema is not a record "
			            "type description.", i);
			return NULL;
		}

		entry = json_node_get_object(node);
		name = venture_json_object_get_string(entry, "name", NULL);

		/*
		 * Refused rather than skipped. A skipped entry produces a
		 * catalog that is quietly missing a record type, and an agent
		 * told "there is no such record type" goes and creates a
		 * duplicate somewhere else instead of reporting a broken
		 * server.
		 */
		if ((NULL == name) || ('\0' == name[0]))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
			            "Entry %u of GET /api/v1/schema has no \"name\", so "
			            "the record type it describes cannot be addressed.",
			            i);
			return NULL;
		}

		if (NULL != g_hash_table_lookup(self->by_name, name))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
			            "GET /api/v1/schema named the record type \"%s\" "
			            "twice.", name);
			return NULL;
		}

		type = g_new0(VentureMcpType, 1);
		type->canonical = g_strdup(name);
		type->description = json_node_ref(node);

		/*
		 * The plural comes from the schema rather than from
		 * venture_pluralise(): a second copy of the rule is a second
		 * answer, and this one has to agree with the routing that
		 * accepts both spellings.
		 */
		plural = venture_json_object_get_string(entry, "plural", NULL);

		if ((NULL != plural) && ('\0' != plural[0]) &&
		    (0 != g_strcmp0(plural, name)))
			type->plural = g_strdup(plural);

		g_ptr_array_add(self->types, type);

		g_hash_table_insert(self->by_name, g_ascii_strdown(name, -1),
		                    type->canonical);

		/* A plural colliding with another type's singular is left
		 * alone: the singular is the addressable spelling and must
		 * win. */
		if ((NULL != type->plural) &&
		    (NULL == g_hash_table_lookup(self->by_name, type->plural)))
		{
			g_hash_table_insert(self->by_name,
			                    g_ascii_strdown(type->plural, -1),
			                    type->canonical);
		}
	}

	return g_steal_pointer(&self);
}

/* --- Types --------------------------------------------------------------- */

guint
venture_mcp_catalog_get_n_types(VentureMcpCatalog *self)
{
	g_return_val_if_fail(VENTURE_IS_MCP_CATALOG(self), 0);

	return self->types->len;
}

gchar **
venture_mcp_catalog_list_types(VentureMcpCatalog *self)
{
	gchar **names;
	guint i;

	g_return_val_if_fail(VENTURE_IS_MCP_CATALOG(self), NULL);

	names = g_new0(gchar *, self->types->len + 1);

	for (i = 0; i < self->types->len; i++)
	{
		VentureMcpType *type;

		type = g_ptr_array_index(self->types, i);
		names[i] = g_strdup(type->canonical);
	}

	return names;
}

const gchar *
venture_mcp_catalog_resolve_type(
	VentureMcpCatalog	*self,
	const gchar		*name
){
	g_autofree gchar *folded = NULL;

	g_return_val_if_fail(VENTURE_IS_MCP_CATALOG(self), NULL);

	if ((NULL == name) || ('\0' == name[0]))
		return NULL;

	folded = g_ascii_strdown(name, -1);

	return g_hash_table_lookup(self->by_name, folded);
}

/*
 * Finds an entry by its canonical name. The caller has already resolved the
 * spelling, so this is a straight scan over a list that is a few dozen long.
 */
static VentureMcpType *
venture_mcp_catalog_find(
	VentureMcpCatalog	*self,
	const gchar		*canonical
){
	guint i;

	for (i = 0; i < self->types->len; i++)
	{
		VentureMcpType *type;

		type = g_ptr_array_index(self->types, i);

		if (0 == g_strcmp0(type->canonical, canonical))
			return type;
	}

	return NULL;
}

JsonNode *
venture_mcp_catalog_describe_type(
	VentureMcpCatalog	*self,
	const gchar		*name
){
	g_autoptr(JsonBuilder) builder = NULL;
	VentureMcpType *type;
	JsonObject *entry;
	JsonArray *fields;
	const gchar *canonical;
	guint i;

	g_return_val_if_fail(VENTURE_IS_MCP_CATALOG(self), NULL);

	canonical = venture_mcp_catalog_resolve_type(self, name);

	if (NULL == canonical)
		return NULL;

	type = venture_mcp_catalog_find(self, canonical);

	if (NULL == type)
		return NULL;

	entry = json_node_get_object(type->description);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, type->canonical);

	/* Said out loud so a caller that saw "sales" somewhere does not
	 * conclude there are two resources. */
	if (NULL != type->plural)
	{
		json_builder_set_member_name(builder, "also_accepted");
		json_builder_add_string_value(builder, type->plural);
	}

	if (json_object_has_member(entry, "table"))
	{
		json_builder_set_member_name(builder, "table");
		json_builder_add_string_value(builder,
			venture_json_object_get_string(entry, "table", ""));
	}

	json_builder_set_member_name(builder, "fields");
	json_builder_begin_array(builder);

	fields = json_object_has_member(entry, "fields")
		? json_object_get_array_member(entry, "fields") : NULL;

	for (i = 0; (NULL != fields) && (i < json_array_get_length(fields)); i++)
	{
		g_autofree gchar *wire = NULL;
		JsonObject *field;
		GList *members;
		GList *iter;
		const gchar *field_name;

		field = json_array_get_object_element(fields, i);

		if (NULL == field)
			continue;

		field_name = venture_json_object_get_string(field, "name", NULL);

		if (NULL == field_name)
			continue;

		json_builder_begin_object(builder);

		/*
		 * The wire spelling, not the C one.
		 *
		 * A POST body with dashed keys is ignored member by member:
		 * the record saves and the values are simply not there. So a
		 * field name handed to a caller in the spelling that does not
		 * work is a silent data-loss bug rather than an error, and
		 * this is the one place anybody looks a field name up.
		 */
		wire = g_strdelimit(g_strdup(field_name), "-", '_');

		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, wire);

		members = json_object_get_members(field);

		for (iter = members; NULL != iter; iter = iter->next)
		{
			if (0 == g_strcmp0(iter->data, "name"))
				continue;

			json_builder_set_member_name(builder, iter->data);
			json_builder_add_value(builder,
				json_node_ref(json_object_get_member(field, iter->data)));
		}

		g_list_free(members);

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* --- Tools --------------------------------------------------------------- */

/*
 * Emits the `type` property: a string whose enum is every record type the
 * schema described. This is the whole of what "generated from the schema"
 * means at the tool-list level, so it exists once and every tool that names
 * a type calls it.
 */
static void
venture_mcp_catalog_add_type_property(
	VentureMcpCatalog	*self,
	JsonBuilder		*builder
){
	guint i;

	json_builder_set_member_name(builder, "type");
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "string");

	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder,
		"The record type. Both the singular and the plural name the same "
		"resource; the singular is listed here.");

	json_builder_set_member_name(builder, "enum");
	json_builder_begin_array(builder);

	for (i = 0; i < self->types->len; i++)
	{
		VentureMcpType *type;

		type = g_ptr_array_index(self->types, i);
		json_builder_add_string_value(builder, type->canonical);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
}

static void
venture_mcp_catalog_add_string_property(
	JsonBuilder	*builder,
	const gchar	*name,
	const gchar	*description
){
	json_builder_set_member_name(builder, name);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "string");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, description);
	json_builder_end_object(builder);
}

static void
venture_mcp_catalog_add_integer_property(
	JsonBuilder	*builder,
	const gchar	*name,
	const gchar	*description
){
	json_builder_set_member_name(builder, name);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "integer");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, description);
	json_builder_end_object(builder);
}

static void
venture_mcp_catalog_add_object_property(
	JsonBuilder	*builder,
	const gchar	*name,
	const gchar	*description
){
	json_builder_set_member_name(builder, name);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "object");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, description);
	json_builder_end_object(builder);
}

static void
venture_mcp_catalog_add_boolean_property(
	JsonBuilder	*builder,
	const gchar	*name,
	const gchar	*description
){
	json_builder_set_member_name(builder, name);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "boolean");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, description);
	json_builder_end_object(builder);
}

/*
 * A string property constrained to a few values.
 */
static void
venture_mcp_catalog_add_enum_property(
	JsonBuilder		*builder,
	const gchar		*name,
	const gchar		*description,
	const gchar *const	*values
){
	gsize i;

	json_builder_set_member_name(builder, name);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "string");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, description);
	json_builder_set_member_name(builder, "enum");
	json_builder_begin_array(builder);

	for (i = 0; NULL != values[i]; i++)
		json_builder_add_string_value(builder, values[i]);

	json_builder_end_array(builder);
	json_builder_end_object(builder);
}

/*
 * The parts of an input schema that are particular to one tool. Everything
 * driven by a flag is emitted by the caller, so a tool that grows a record
 * type parameter does not also grow a copy of how to spell one.
 */
static void
venture_mcp_catalog_add_tool_extras(
	JsonBuilder	*builder,
	const gchar	*tool_name
){
	if (0 == g_strcmp0(tool_name, "venture_list"))
	{
		venture_mcp_catalog_add_object_property(builder, "filters",
			"Field filters. \"status__eq\": \"active\" or "
			"\"occurred_at__gte\": \"2026-01-01\". A name that is not a "
			"field of this record type is refused rather than ignored.");
		venture_mcp_catalog_add_string_property(builder, "search",
			"Free-text search across this type's searchable fields.");
		venture_mcp_catalog_add_string_property(builder, "period",
			"A named period, such as this_month, last_quarter, ytd, "
			"fy_2026, 2026-Q2 or 2026-01-01..2026-03-31.");
		venture_mcp_catalog_add_string_property(builder, "period_field",
			"Which date field the period applies to. Defaults to the "
			"record type's own primary date.");
		venture_mcp_catalog_add_string_property(builder, "order",
			"A field name to sort by; prefix with - to reverse it.");
		venture_mcp_catalog_add_integer_property(builder, "limit",
			"How many records to return.");
		venture_mcp_catalog_add_integer_property(builder, "offset",
			"How many records to skip.");
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_report"))
	{
		venture_mcp_catalog_add_string_property(builder, "name",
			"The report to run. venture_reports lists them.");
		venture_mcp_catalog_add_string_property(builder, "period",
			"The period to run it over.");
		venture_mcp_catalog_add_integer_property(builder, "customer_id", "Customer for a statement.");
		venture_mcp_catalog_add_integer_property(builder, "organization_id", "Legal entity to report.");
		venture_mcp_catalog_add_string_property(builder, "currency", "Book currency to report.");
		venture_mcp_catalog_add_string_property(builder, "as_of", "Inclusive historical cutoff.");

		json_builder_set_member_name(builder, "format");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, "string");
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder,
			"json for the structured result, csv for the spreadsheet "
			"export. The result states which one actually came back.");
		json_builder_set_member_name(builder, "enum");
		json_builder_begin_array(builder);
		json_builder_add_string_value(builder, "json");
		json_builder_add_string_value(builder, "csv");
		json_builder_end_array(builder);
		json_builder_end_object(builder);
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_confirmations"))
	{
		json_builder_set_member_name(builder, "action");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, "string");
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder,
			"list to see what is waiting, approve to apply one, reject "
			"to discard it. Defaults to list.");
		json_builder_set_member_name(builder, "enum");
		json_builder_begin_array(builder);
		json_builder_add_string_value(builder, "list");
		json_builder_add_string_value(builder, "approve");
		json_builder_add_string_value(builder, "reject");
		json_builder_end_array(builder);
		json_builder_end_object(builder);

		venture_mcp_catalog_add_string_property(builder, "id",
			"Which staged change to approve or reject.");
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_link"))
	{
		venture_mcp_catalog_add_string_property(builder, "source_type",
			"The record type at this end.");
		venture_mcp_catalog_add_integer_property(builder, "source_id",
			"Its numeric id.");
		venture_mcp_catalog_add_string_property(builder, "kind",
			"What the link means, read from the source. Defaults to "
			"related.");
		venture_mcp_catalog_add_string_property(builder, "target_type",
			"The record type at the other end.");
		venture_mcp_catalog_add_integer_property(builder, "target_id",
			"Its numeric id.");
		venture_mcp_catalog_add_string_property(builder, "note",
			"Why, in a few words.");
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_dashboards"))
	{
		static const gchar *const whats[] = {
			"list", "kinds", "templates", NULL
		};

		venture_mcp_catalog_add_enum_property(builder, "what",
			"list for the dashboards, kinds for the widget catalogue, "
			"templates for the shipped definitions. Defaults to list.",
			whats);
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_dashboard"))
	{
		static const gchar *const modes[] = {
			"data", "settings", "definition", NULL
		};

		venture_mcp_catalog_add_string_property(builder, "slug",
			"The dashboard's slug, as in /dashboards/<slug>. "
			"venture_dashboards lists them.");
		venture_mcp_catalog_add_enum_property(builder, "mode",
			"data evaluates every widget; settings lists them; definition "
			"is the portable JSON. Defaults to data.", modes);
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_dashboard_build"))
	{
		venture_mcp_catalog_add_string_property(builder, "template",
			"A shipped template: factory, reporting, work or overview. "
			"venture_dashboards (what: templates) shows each one's "
			"definition.");
		venture_mcp_catalog_add_object_property(builder, "definition",
			"A dashboard definition, as venture_dashboard (mode: "
			"definition) returns one. Ignored when a template is named.");
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_inbox"))
	{
		static const gchar *const actions[] = { "list", "read", NULL };

		venture_mcp_catalog_add_enum_property(builder, "action",
			"list reads the inbox; read marks a notification read. "
			"Defaults to list.", actions);
		venture_mcp_catalog_add_integer_property(builder, "id",
			"read: the notification's id, or 0 for every unread one.");
		venture_mcp_catalog_add_boolean_property(builder, "unread",
			"list: only what is unread. Defaults to true.");
		venture_mcp_catalog_add_integer_property(builder, "limit",
			"list: at most this many, newest first. Defaults to 50.");
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_runs"))
	{
		static const gchar *const whats[] = { "runs", "budgets", NULL };
		static const gchar *const states[] = {
			"queued", "running", "succeeded", "failed", "cancelled",
			"refused", "interrupted", NULL
		};

		venture_mcp_catalog_add_enum_property(builder, "what",
			"runs for the runs and their totals; budgets for the agent "
			"budgets. Defaults to runs.", whats);
		venture_mcp_catalog_add_enum_property(builder, "state",
			"runs: only runs in this state.", states);
		venture_mcp_catalog_add_integer_property(builder, "limit",
			"runs: at most this many, newest first. Defaults to 50.");
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_desk"))
	{
		static const gchar *const actions[] = {
			"sla", "activity", "sprints", "sprint", "triage", "summarise",
			"draft", "worklog", "macro", "fix_ticket", "bulk", "sweep", NULL
		};

		venture_mcp_catalog_add_enum_property(builder, "action",
			"What to do; see the tool's description.", actions);
		venture_mcp_catalog_add_integer_property(builder, "id",
			"The ticket (sla, worklog, macro), sprint, incident "
			"(fix_ticket) or record (activity).");
		venture_mcp_catalog_add_string_property(builder, "type",
			"activity and bulk: the record type.");
		venture_mcp_catalog_add_string_property(builder, "hours",
			"worklog: how long, as a number of hours, e.g. 1.5.");
		venture_mcp_catalog_add_string_property(builder, "note",
			"worklog: what the time went on.");
		venture_mcp_catalog_add_string_property(builder, "macro",
			"macro: the macro's name or id.");
		venture_mcp_catalog_add_object_property(builder, "changes",
			"bulk: field to value, in the wire spelling.");
		venture_mcp_catalog_add_string_property(builder, "ids",
			"bulk: the record ids, comma separated.");
		venture_mcp_catalog_add_boolean_property(builder, "delete",
			"bulk: remove the records instead of changing them.");
		venture_mcp_catalog_add_integer_property(builder, "limit",
			"activity: at most this many entries.");
		return;
	}

	if (0 == g_strcmp0(tool_name, "venture_factory"))
	{
		static const gchar *const actions[] = {
			"status", "changelog", "publish", NULL
		};

		venture_mcp_catalog_add_enum_property(builder, "action",
			"status for the loop at a glance; changelog to draft a "
			"release's changelog from its tickets; publish to cut a "
			"release on the forge. Defaults to status.", actions);
		venture_mcp_catalog_add_integer_property(builder, "id",
			"The release's numeric id, for changelog and publish.");
		venture_mcp_catalog_add_boolean_property(builder, "replace",
			"changelog only: overwrite a changelog somebody already "
			"wrote. Defaults to false.");
		venture_mcp_catalog_add_boolean_property(builder, "prerelease",
			"publish only: mark the release a pre-release on the forge.");
		return;
	}
}

/*
 * Names the required properties of one tool. Kept beside the flags so a
 * required parameter cannot be declared in the schema and forgotten here.
 */
static void
venture_mcp_catalog_add_required(
	JsonBuilder			*builder,
	const VentureMcpToolDef		*def
){
	json_builder_set_member_name(builder, "required");
	json_builder_begin_array(builder);

	if (0 != (def->flags & TOOL_FLAG_TYPE_REQUIRED))
		json_builder_add_string_value(builder, "type");

	if (0 != (def->flags & TOOL_FLAG_ID))
		json_builder_add_string_value(builder, "id");

	if (0 != (def->flags & TOOL_FLAG_VALUES))
		json_builder_add_string_value(builder, "values");

	if (0 == g_strcmp0(def->name, "venture_report"))
		json_builder_add_string_value(builder, "name");

	if (0 == g_strcmp0(def->name, "venture_dashboard"))
		json_builder_add_string_value(builder, "slug");

	if (0 == g_strcmp0(def->name, "venture_desk"))
		json_builder_add_string_value(builder, "action");

	if (0 == g_strcmp0(def->name, "venture_link"))
	{
		json_builder_add_string_value(builder, "source_type");
		json_builder_add_string_value(builder, "source_id");
		json_builder_add_string_value(builder, "target_type");
		json_builder_add_string_value(builder, "target_id");
	}

	json_builder_end_array(builder);
}

JsonNode *
venture_mcp_catalog_get_tools(VentureMcpCatalog *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_MCP_CATALOG(self), NULL);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < G_N_ELEMENTS(tool_defs); i++)
	{
		const VentureMcpToolDef *def;

		def = &tool_defs[i];

		json_builder_begin_object(builder);

		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, def->name);

		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder, def->description);

		json_builder_set_member_name(builder, "inputSchema");
		json_builder_begin_object(builder);

		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, "object");

		json_builder_set_member_name(builder, "properties");
		json_builder_begin_object(builder);

		if (0 != (def->flags & TOOL_FLAG_TYPE))
			venture_mcp_catalog_add_type_property(self, builder);

		if (0 != (def->flags & TOOL_FLAG_ID))
		{
			venture_mcp_catalog_add_integer_property(builder, "id",
				"The record's numeric id.");
		}

		if (0 != (def->flags & TOOL_FLAG_VALUES))
		{
			venture_mcp_catalog_add_object_property(builder, "values",
				"The fields to write, in their wire spelling. "
				"venture_schema prints them.");
		}

		venture_mcp_catalog_add_tool_extras(builder, def->name);

		json_builder_end_object(builder);

		venture_mcp_catalog_add_required(builder, def);

		json_builder_end_object(builder);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}

gboolean
venture_mcp_catalog_has_tool(
	VentureMcpCatalog	*self,
	const gchar		*tool_name
){
	gsize i;

	g_return_val_if_fail(VENTURE_IS_MCP_CATALOG(self), FALSE);

	if (NULL == tool_name)
		return FALSE;

	for (i = 0; i < G_N_ELEMENTS(tool_defs); i++)
	{
		if (0 == g_strcmp0(tool_defs[i].name, tool_name))
			return TRUE;
	}

	return FALSE;
}
