/*
 * venture-entity.c - The abstract base class every persisted record derives from
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The interesting part of this file is that almost nothing in it is specific
 * to any record type. Serialisation, validation, diffing, display names and
 * field descriptions are all derived by walking GObject properties. That is
 * what makes a new entity type cost a handful of g_object_class_install_property
 * calls instead of a schema, a serialiser, a form and a set of routes.
 *
 * Per-property persistence metadata is stored as qdata on the GType rather
 * than in the class struct, for two reasons: the class struct has a fixed
 * layout that plugins compile against, and qdata lookups walk the type
 * ancestry naturally, so a subclass sees the base class's declarations
 * without repeating them.
 */

#include "venture.h"

#include <string.h>

/*
 * Property names on the identity spine. Collected here because several
 * functions need to treat them as a set -- duplication skips them, diffing
 * ignores them, and the schema builder positions them first.
 */
/*
 * The spine every record carries: assigned by the system, never typed, and
 * excluded from diffs because it changes on every save.
 *
 * Deliberately NOT including organization-id. Which entity a record belongs
 * to is real data -- moving a sale from one business to another is exactly
 * the kind of change the audit log exists to record, and a save that cannot
 * see the change concludes nothing happened and writes nothing at all.
 */
static const gchar *const venture_entity_identity_properties[] = {
	"id",
	"uuid",
	"created-at",
	"updated-at",
	"version",
	NULL
};

/*
 * Additionally left out of generated forms, lists and schemas.
 *
 * These are real, persisted, audited data -- they simply have their own
 * controls. The entity a record belongs to is chosen with the picker and
 * rendered as its own select; a soft-deleted record is filtered out before a
 * list is drawn; the attribute bag is edited through the venture type that
 * declares it.
 */
static const gchar *const venture_entity_generated_ui_exclusions[] = {
	"organization-id",
	"deleted-at",
	"attributes",
	"sessions-invalidated-at",
	NULL
};

typedef struct
{
	gint64		 id;
	gchar		*uuid;
	gint64		 organization_id;
	GDateTime	*created_at;
	GDateTime	*updated_at;
	GDateTime	*deleted_at;
	gint64		 version;

	/* Free-form attributes, kept as a string table and materialised to
	 * JSON only when the record is serialised or saved. */
	GHashTable	*attributes;

	/* Storage for declaratively-installed fields, keyed by property name.
	 * A subclass built from a #VentureFieldDecl table has no instance
	 * struct of its own; its values live here. */
	GHashTable	*fields;
} VentureEntityPrivate;

enum
{
	PROP_0,
	PROP_ID,
	PROP_UUID,
	PROP_ORGANIZATION_ID,
	PROP_CREATED_AT,
	PROP_UPDATED_AT,
	PROP_DELETED_AT,
	PROP_VERSION,
	PROP_ATTRIBUTES,
	N_PROPERTIES
};

/*
 * Declaratively-installed properties are numbered from here upward. Starting
 * well clear of the base class's own identifiers means a subclass never has
 * to know how many properties its parent used, so adding one to the base
 * class later cannot silently collide with a plugin's.
 */
#define VENTURE_ENTITY_FIELD_PROP_BASE (1000)

static GParamSpec *venture_entity_properties[N_PROPERTIES] = { NULL };

enum
{
	SIGNAL_CHANGED,
	SIGNAL_VALIDATED,
	N_SIGNALS
};

static guint venture_entity_signals[N_SIGNALS] = { 0 };

static void venture_entity_serializable_init(VentureSerializableInterface *iface);

G_DEFINE_ABSTRACT_TYPE_WITH_CODE(VentureEntity, venture_entity, G_TYPE_OBJECT,
	G_ADD_PRIVATE(VentureEntity)
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_SERIALIZABLE,
	                      venture_entity_serializable_init))

/* --- Per-class metadata -------------------------------------------------- */

/*
 * Column flags and foreign-key declarations are attached to the GType as
 * qdata. Looking a property up walks from the concrete type toward the base,
 * so a subclass inherits the base class's declarations for the identity
 * spine without restating them.
 */
static GQuark
venture_entity_column_flags_quark(void)
{
	static GQuark quark = 0;

	if (0 == quark)
		quark = g_quark_from_static_string("venture-entity-column-flags");

	return quark;
}

static GQuark
venture_entity_reference_quark(void)
{
	static GQuark quark = 0;

	if (0 == quark)
		quark = g_quark_from_static_string("venture-entity-references");

	return quark;
}

static GHashTable *
venture_entity_get_metadata_table(
	GType		type,
	GQuark		quark,
	gboolean	create
){
	GHashTable *table;

	table = g_type_get_qdata(type, quark);

	if ((NULL == table) && create)
	{
		table = g_hash_table_new_full(g_str_hash, g_str_equal,
		                              g_free, g_free);
		g_type_set_qdata(type, quark, table);
	}

	return table;
}

/*
 * Looks a key up in this type's table, then each ancestor's, stopping at
 * VentureEntity itself.
 */
static gpointer
venture_entity_lookup_metadata(
	GType		 type,
	GQuark		 quark,
	const gchar	*key,
	gboolean	*out_found
){
	GType cursor;

	*out_found = FALSE;

	for (cursor = type;
	     (G_TYPE_INVALID != cursor) && g_type_is_a(cursor, VENTURE_TYPE_ENTITY);
	     cursor = g_type_parent(cursor))
	{
		GHashTable *table;
		gpointer value;

		table = venture_entity_get_metadata_table(cursor, quark, FALSE);

		if (NULL == table)
			continue;

		if (g_hash_table_lookup_extended(table, key, NULL, &value))
		{
			*out_found = TRUE;
			return value;
		}
	}

	return NULL;
}

/*
 * The position a field occupies in its type's field table.
 *
 * GObject returns properties in an order of its own choosing, so without
 * this every generated surface -- the list columns, the form, the schema --
 * falls back to something close to alphabetical, and the first seven columns
 * of a list end up being whichever fields happen to sort early rather than
 * the ones that identify a record.
 */
static GQuark
venture_entity_field_order_quark(void)
{
	static GQuark quark = 0;

	if (0 == quark)
		quark = g_quark_from_static_string("venture-entity-field-order");

	return quark;
}

/*
 * The kind a field was declared with.
 *
 * Most kinds can be recovered from the property's value type, but not all:
 * a long text field and a short string field are both G_TYPE_STRING, and
 * recomputing loses the distinction -- so a Notes field renders as a
 * one-line box. The declaration is the only place that difference exists.
 */
static GQuark
venture_entity_field_kind_quark(void)
{
	static GQuark quark = 0;

	if (0 == quark)
		quark = g_quark_from_static_string("venture-entity-field-kind");

	return quark;
}

void
venture_entity_class_set_field_kind(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	VentureFieldKind	 kind
){
	GHashTable *table;

	g_return_if_fail(VENTURE_IS_ENTITY_CLASS(klass));
	g_return_if_fail(NULL != property_name);

	table = venture_entity_get_metadata_table(G_OBJECT_CLASS_TYPE(klass),
	                                          venture_entity_field_kind_quark(),
	                                          TRUE);

	g_hash_table_insert(table, g_strdup(property_name),
	                    g_memdup2(&kind, sizeof(kind)));
}

gboolean
venture_entity_class_get_field_kind(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	VentureFieldKind	*out_kind
){
	GHashTable *table;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_ENTITY_CLASS(klass), FALSE);
	g_return_val_if_fail(NULL != property_name, FALSE);

	table = venture_entity_get_metadata_table(G_OBJECT_CLASS_TYPE(klass),
	                                          venture_entity_field_kind_quark(),
	                                          FALSE);
	value = (NULL != table)
		? g_hash_table_lookup(table, property_name) : NULL;

	if (NULL == value)
		return FALSE;

	if (NULL != out_kind)
		*out_kind = *(VentureFieldKind *)value;

	return TRUE;
}

void
venture_entity_class_set_field_order(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	guint			 order
){
	GHashTable *table;

	g_return_if_fail(VENTURE_IS_ENTITY_CLASS(klass));
	g_return_if_fail(NULL != property_name);

	table = venture_entity_get_metadata_table(G_OBJECT_CLASS_TYPE(klass),
	                                          venture_entity_field_order_quark(),
	                                          TRUE);

	g_hash_table_insert(table, g_strdup(property_name),
	                    g_memdup2(&order, sizeof(order)));
}

guint
venture_entity_class_get_field_order(
	VentureEntityClass	*klass,
	const gchar		*property_name
){
	GHashTable *table;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_ENTITY_CLASS(klass), G_MAXUINT);
	g_return_val_if_fail(NULL != property_name, G_MAXUINT);

	table = venture_entity_get_metadata_table(G_OBJECT_CLASS_TYPE(klass),
	                                          venture_entity_field_order_quark(),
	                                          FALSE);
	value = (NULL != table)
		? g_hash_table_lookup(table, property_name) : NULL;

	/* Undeclared fields sort after declared ones rather than before. */
	return (NULL != value) ? *(guint *)value : G_MAXUINT;
}

void
venture_entity_class_set_column_flags(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	VentureColumnFlags	 flags
){
	GHashTable *table;

	g_return_if_fail(VENTURE_IS_ENTITY_CLASS(klass));
	g_return_if_fail(NULL != property_name);

	table = venture_entity_get_metadata_table(G_OBJECT_CLASS_TYPE(klass),
	                                          venture_entity_column_flags_quark(),
	                                          TRUE);

	/* Stored as a heap integer rather than by pointer-stuffing so that a
	 * flags value of zero is distinguishable from "not declared". */
	g_hash_table_insert(table, g_strdup(property_name),
	                    g_memdup2(&flags, sizeof(flags)));
}

VentureColumnFlags
venture_entity_class_get_column_flags(
	VentureEntityClass	*klass,
	const gchar		*property_name
){
	gpointer value;
	gboolean found;

	g_return_val_if_fail(VENTURE_IS_ENTITY_CLASS(klass), VENTURE_COLUMN_FLAG_NONE);
	g_return_val_if_fail(NULL != property_name, VENTURE_COLUMN_FLAG_NONE);

	value = venture_entity_lookup_metadata(G_OBJECT_CLASS_TYPE(klass),
	                                       venture_entity_column_flags_quark(),
	                                       property_name, &found);

	if (!found || (NULL == value))
		return VENTURE_COLUMN_FLAG_NONE;

	return *(VentureColumnFlags *)value;
}

void
venture_entity_class_set_reference(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	const gchar		*target_entity_name
){
	GHashTable *table;

	g_return_if_fail(VENTURE_IS_ENTITY_CLASS(klass));
	g_return_if_fail(NULL != property_name);
	g_return_if_fail(NULL != target_entity_name);

	table = venture_entity_get_metadata_table(G_OBJECT_CLASS_TYPE(klass),
	                                          venture_entity_reference_quark(),
	                                          TRUE);

	g_hash_table_insert(table, g_strdup(property_name),
	                    g_strdup(target_entity_name));
}

const gchar *
venture_entity_class_get_reference(
	VentureEntityClass	*klass,
	const gchar		*property_name
){
	gboolean found;

	g_return_val_if_fail(VENTURE_IS_ENTITY_CLASS(klass), NULL);
	g_return_val_if_fail(NULL != property_name, NULL);

	return venture_entity_lookup_metadata(G_OBJECT_CLASS_TYPE(klass),
	                                      venture_entity_reference_quark(),
	                                      property_name, &found);
}

GParamSpec **
venture_entity_class_list_persistent_properties(
	VentureEntityClass	*klass,
	guint			*n_properties
){
	g_autofree GParamSpec **all = NULL;
	GParamSpec **kept;
	guint n_all;
	guint n_kept;
	guint i;

	g_return_val_if_fail(VENTURE_IS_ENTITY_CLASS(klass), NULL);
	g_return_val_if_fail(NULL != n_properties, NULL);

	all = g_object_class_list_properties(G_OBJECT_CLASS(klass), &n_all);
	kept = g_new0(GParamSpec *, n_all + 1);
	n_kept = 0;

	for (i = 0; i < n_all; i++)
	{
		VentureColumnFlags flags;

		/* A property that cannot be read cannot be saved, and one that
		 * cannot be written cannot be loaded back, so both directions
		 * are required for persistence. */
		if (0 == (all[i]->flags & G_PARAM_READABLE))
			continue;

		if (0 == (all[i]->flags & G_PARAM_WRITABLE))
			continue;

		flags = venture_entity_class_get_column_flags(klass, all[i]->name);

		if (0 != (flags & VENTURE_COLUMN_FLAG_TRANSIENT))
			continue;

		kept[n_kept] = all[i];
		n_kept++;
	}

	*n_properties = n_kept;

	return kept;
}

/* --- Name conversion ----------------------------------------------------- */

gchar *
venture_entity_property_to_column(const gchar *property_name)
{
	gchar *column;

	g_return_val_if_fail(NULL != property_name, NULL);

	column = g_strdup(property_name);
	g_strdelimit(column, "-", '_');

	return column;
}

gchar *
venture_entity_column_to_property(const gchar *column_name)
{
	gchar *property;

	g_return_val_if_fail(NULL != column_name, NULL);

	property = g_strdup(column_name);
	g_strdelimit(property, "_", '-');

	return property;
}

/* --- Default virtual method implementations ------------------------------ */

/*
 * Derives an entity name from the GType name: "VentureInventoryTxn" becomes
 * "inventory_txn". A subclass that wants a different name overrides
 * get_entity_name; most do not need to.
 *
 * The result is cached on the type, because it is asked for constantly --
 * once per row when serialising a list -- and recomputing it every time
 * would be pointless work.
 */
static const gchar *
venture_entity_real_get_entity_name(VentureEntity *self)
{
	static GQuark quark = 0;
	GType type;
	const gchar *cached;
	const gchar *type_name;
	g_autoptr(GString) name = NULL;
	gsize i;

	if (0 == quark)
		quark = g_quark_from_static_string("venture-entity-name");

	type = G_OBJECT_TYPE(self);
	cached = g_type_get_qdata(type, quark);

	if (NULL != cached)
		return cached;

	type_name = g_type_name(type);

	/* Drop the "Venture" prefix if present; a plugin's type may not have
	 * one, in which case the whole name is used. */
	if (g_str_has_prefix(type_name, "Venture"))
		type_name += strlen("Venture");

	name = g_string_new(NULL);

	for (i = 0; '\0' != type_name[i]; i++)
	{
		if (g_ascii_isupper(type_name[i]) && (i > 0))
			g_string_append_c(name, '_');

		g_string_append_c(name, g_ascii_tolower(type_name[i]));
	}

	g_type_set_qdata(type, quark, g_strdup(name->str));

	return g_type_get_qdata(type, quark);
}

static const gchar *
venture_entity_real_get_table_name(VentureEntity *self)
{
	static GQuark quark = 0;
	GType type;
	const gchar *cached;

	if (0 == quark)
		quark = g_quark_from_static_string("venture-entity-table");

	type = G_OBJECT_TYPE(self);
	cached = g_type_get_qdata(type, quark);

	if (NULL != cached)
		return cached;

	g_type_set_qdata(type, quark,
		venture_pluralise(venture_entity_get_entity_name(self)));

	return g_type_get_qdata(type, quark);
}

/*
 * Finds the first property that reads like a human-facing name. Trying
 * several in order means an entity does not have to declare anything for its
 * records to be identifiable in a list, an audit entry or an AI answer.
 */
static gchar *
venture_entity_real_get_display_name(VentureEntity *self)
{
	static const gchar *const candidates[] = {
		"name", "title", "label", "subject", "email", "sku",
		"description", "reference", NULL
	};
	GObjectClass *object_class;
	gsize i;

	object_class = G_OBJECT_GET_CLASS(self);

	for (i = 0; NULL != candidates[i]; i++)
	{
		GParamSpec *pspec;
		g_autofree gchar *text = NULL;

		pspec = g_object_class_find_property(object_class, candidates[i]);

		if ((NULL == pspec) || (G_TYPE_STRING != pspec->value_type))
			continue;

		g_object_get(self, candidates[i], &text, NULL);

		if (!venture_string_is_empty(text))
			return g_steal_pointer(&text);
	}

	return g_strdup_printf("%s #%" G_GINT64_FORMAT,
	                       venture_entity_get_entity_name(self),
	                       venture_entity_get_id(self));
}

static gchar *
venture_entity_real_get_search_text(VentureEntity *self)
{
	g_autoptr(GString) text = NULL;
	g_autofree GParamSpec **properties = NULL;
	VentureEntityClass *klass;
	guint n_properties;
	guint i;

	klass = VENTURE_ENTITY_GET_CLASS(self);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);
	text = g_string_new(NULL);

	for (i = 0; i < n_properties; i++)
	{
		VentureColumnFlags flags;
		g_autofree gchar *value = NULL;

		flags = venture_entity_class_get_column_flags(klass,
		                                              properties[i]->name);

		if (0 == (flags & VENTURE_COLUMN_FLAG_SEARCHABLE))
			continue;

		/* Only string properties contribute usefully to a text search;
		 * a searchable integer is almost certainly a mistake in the
		 * declaration rather than something to stringify. */
		if (G_TYPE_STRING != properties[i]->value_type)
			continue;

		g_object_get(self, properties[i]->name, &value, NULL);

		if (venture_string_is_empty(value))
			continue;

		if (text->len > 0)
			g_string_append_c(text, ' ');

		g_string_append(text, value);
	}

	if (0 == text->len)
		return NULL;

	return g_string_free(g_steal_pointer(&text), FALSE);
}

/*
 * The default validation applies the constraints that the property metadata
 * already implies, so an entity gets required-field and bounds checking
 * purely from its declarations.
 */
static gboolean
venture_entity_real_validate(
	VentureEntity	 *self,
	GError		**error
){
	g_autofree GParamSpec **properties = NULL;
	VentureEntityClass *klass;
	guint n_properties;
	guint i;

	klass = VENTURE_ENTITY_GET_CLASS(self);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);

	for (i = 0; i < n_properties; i++)
	{
		VentureColumnFlags flags;

		flags = venture_entity_class_get_column_flags(klass,
		                                              properties[i]->name);

		if (0 == (flags & VENTURE_COLUMN_FLAG_NOT_NULL))
			continue;

		/* NOT NULL on a string means "must have content": an empty
		 * string satisfies a database constraint but is never what the
		 * declaration meant. */
		if (G_TYPE_STRING == properties[i]->value_type)
		{
			g_autofree gchar *value = NULL;

			g_object_get(self, properties[i]->name, &value, NULL);

			if (venture_string_is_empty(value))
			{
				venture_set_error_validation(error,
					g_param_spec_get_nick(properties[i]),
					"is required");
				return FALSE;
			}

			continue;
		}

		if (G_TYPE_DATE_TIME == properties[i]->value_type)
		{
			g_autoptr(GDateTime) value = NULL;

			g_object_get(self, properties[i]->name, &value, NULL);

			if (NULL == value)
			{
				venture_set_error_validation(error,
					g_param_spec_get_nick(properties[i]),
					"is required");
				return FALSE;
			}
		}
	}

	return TRUE;
}

static gboolean
venture_entity_real_before_save(
	VentureEntity	 *self,
	GError		**error
){
	/* Nothing to do by default. Subclasses that compute a derived total
	 * or normalise a field override this and chain up. */
	return TRUE;
}

static void
venture_entity_real_after_load(VentureEntity *self)
{
}

static GPtrArray *
venture_entity_real_get_field_specs(VentureEntity *self)
{
	g_autoptr(GPtrArray) specs = NULL;
	g_autofree GParamSpec **properties = NULL;
	VentureEntityClass *klass;
	guint n_properties;
	guint i;

	klass = VENTURE_ENTITY_GET_CLASS(self);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);
	specs = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_field_spec_free);

	for (i = 0; i < n_properties; i++)
	{
		VentureFieldSpec *spec;
		VentureFieldKind kind;
		VentureColumnFlags flags;
		GType value_type;
		const gchar *reference;

		/* The identity spine is machinery, not data the operator
		 * edits, so it is left out of generated forms and schemas. */
		if (g_strv_contains(venture_entity_identity_properties,
		                    properties[i]->name) ||
		    g_strv_contains(venture_entity_generated_ui_exclusions,
		                    properties[i]->name))
			continue;

		value_type = properties[i]->value_type;
		flags = venture_entity_class_get_column_flags(klass,
		                                              properties[i]->name);
		reference = venture_entity_class_get_reference(klass,
		                                              properties[i]->name);

		/*
		 * What the type actually declared, where it said so. Falling
		 * back to the value type covers properties installed directly
		 * rather than through a field table.
		 */
		if (venture_entity_class_get_field_kind(klass, properties[i]->name,
		                                        &kind))
		{
			/* Declared. */
		}
		else if (NULL != reference)
			kind = VENTURE_FIELD_KIND_REFERENCE;
		else if (VENTURE_TYPE_MONEY == value_type)
			kind = VENTURE_FIELD_KIND_MONEY;
		else if (G_TYPE_DATE_TIME == value_type)
			kind = VENTURE_FIELD_KIND_DATETIME;
		else if (G_TYPE_BOOLEAN == value_type)
			kind = VENTURE_FIELD_KIND_BOOLEAN;
		else if (G_TYPE_DOUBLE == value_type)
			kind = VENTURE_FIELD_KIND_DOUBLE;
		else if (G_TYPE_IS_ENUM(value_type))
			kind = VENTURE_FIELD_KIND_ENUM;
		else if ((G_TYPE_INT64 == value_type) || (G_TYPE_INT == value_type))
			kind = VENTURE_FIELD_KIND_INTEGER;
		else
			kind = VENTURE_FIELD_KIND_STRING;

		spec = venture_field_spec_new(properties[i]->name,
		                              g_param_spec_get_nick(properties[i]),
		                              kind);

		/* The order the type declared, so every generated surface reads
		 * in the order somebody actually wrote the fields down. */
		spec->display_order = venture_entity_class_get_field_order(
			klass, properties[i]->name);

		/* Carry the declared persistence hints across so that the
		 * generated form marks required fields, the list view shows the
		 * right columns, and a sensitive field is never rendered into
		 * an input the browser would then echo back. */
		spec->flags = flags;
		spec->required = (0 != (flags & VENTURE_COLUMN_FLAG_NOT_NULL));
		spec->show_in_list = (0 == (flags & VENTURE_COLUMN_FLAG_SENSITIVE));

		if (NULL != reference)
			spec->reference_type = g_strdup(reference);

		if (G_TYPE_IS_ENUM(value_type))
		{
			/* An enum property's permitted values come straight from
			 * the registered GEnum, so the form's select options and
			 * the AI tool's schema can never drift from the C
			 * definition. */
			spec->choices = venture_enum_list_nicks(value_type);
		}

		if (NULL != g_param_spec_get_blurb(properties[i]))
			spec->help = g_strdup(g_param_spec_get_blurb(properties[i]));

		g_ptr_array_add(specs, spec);
	}

	return g_steal_pointer(&specs);
}

/* --- Serialisation ------------------------------------------------------- */

static gboolean
attribute_shadows_secret(VentureEntity *self, const gchar *name)
{
	GParamSpec *property = g_object_class_find_property(G_OBJECT_GET_CLASS(self), name);
	return property != NULL && (venture_entity_class_get_column_flags(VENTURE_ENTITY_GET_CLASS(self),
		property->name) & VENTURE_COLUMN_FLAG_SENSITIVE) != 0;
}

static gboolean
attributes_contain_secret(VentureEntity *self)
{
	VentureEntityPrivate *priv = venture_entity_get_instance_private(self);
	GHashTableIter iter;
	gpointer key;
	if (priv->attributes == NULL)
		return FALSE;
	g_hash_table_iter_init(&iter, priv->attributes);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		if (attribute_shadows_secret(self, key))
			return TRUE;
	return FALSE;
}

static JsonNode *
venture_entity_serializable_to_json(
	VentureSerializable	*serializable,
	gboolean		 include_sensitive
){
	VentureEntity *self;
	VentureEntityPrivate *priv;
	VentureEntityClass *klass;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree GParamSpec **properties = NULL;
	g_autofree gchar *display_name = NULL;
	guint n_properties;
	guint i;

	self = VENTURE_ENTITY(serializable);
	priv = venture_entity_get_instance_private(self);
	klass = VENTURE_ENTITY_GET_CLASS(self);

	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);
	builder = json_builder_new();
	json_builder_begin_object(builder);

	/* The type name comes first so a consumer reading a heterogeneous
	 * list -- a search result, an audit trail -- can dispatch on it
	 * without inspecting the rest of the object. */
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder,
		venture_entity_get_entity_name(self));

	for (i = 0; i < n_properties; i++)
	{
		VentureColumnFlags flags;
		g_autofree gchar *member = NULL;
		g_auto(GValue) value = G_VALUE_INIT;

		flags = venture_entity_class_get_column_flags(klass,
		                                              properties[i]->name);

		/* A sensitive field is omitted entirely rather than nulled, so
		 * a client cannot distinguish "no password set" from "password
		 * set but withheld" by looking at the response. */
		if (!include_sensitive && (0 != (flags & VENTURE_COLUMN_FLAG_SENSITIVE)))
			continue;

		/* The attributes bag is handled below as a nested object; its
		 * string property is an implementation detail. */
		if (0 == g_strcmp0(properties[i]->name, "attributes"))
			continue;

		member = venture_entity_property_to_column(properties[i]->name);

		g_value_init(&value, properties[i]->value_type);
		g_object_get_property(G_OBJECT(self), properties[i]->name, &value);

		json_builder_set_member_name(builder, member);
		json_builder_add_value(builder, venture_json_node_from_value(&value));
	}

	/* Custom attributes are exposed as a nested object rather than
	 * flattened, so a custom attribute can never shadow a real field. */
	if ((NULL != priv->attributes) && (g_hash_table_size(priv->attributes) > 0))
	{
		g_autoptr(GList) keys = NULL;
		GList *iter;

		json_builder_set_member_name(builder, "attributes");
		json_builder_begin_object(builder);

		keys = g_hash_table_get_keys(priv->attributes);
		keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);

		for (iter = keys; NULL != iter; iter = iter->next)
		{
			/* Older colliding definitions may already have copied secrets. */
			if (!include_sensitive && attribute_shadows_secret(self, iter->data))
				continue;
			json_builder_set_member_name(builder, iter->data);
			json_builder_add_string_value(builder,
				g_hash_table_lookup(priv->attributes, iter->data));
		}

		json_builder_end_object(builder);
	}

	/* A rendered label travels with every record so that a list view, an
	 * audit entry or an AI answer can name it without a second lookup. */
	display_name = venture_entity_get_display_name(self);

	json_builder_set_member_name(builder, "display_name");
	json_builder_add_string_value(builder, display_name);

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

static gboolean
venture_entity_serializable_from_json(
	VentureSerializable	 *serializable,
	JsonNode		 *node,
	GError			**error
){
	VentureEntity *self;
	VentureEntityClass *klass;
	JsonObject *object;
	g_autofree GParamSpec **properties = NULL;
	guint n_properties;
	guint i;

	self = VENTURE_ENTITY(serializable);
	klass = VENTURE_ENTITY_GET_CLASS(self);

	if (!JSON_NODE_HOLDS_OBJECT(node))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "A %s must be a JSON object",
		            venture_entity_get_entity_name(self));
		return FALSE;
	}

	object = json_node_get_object(node);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);

	for (i = 0; i < n_properties; i++)
	{
		g_autofree gchar *member = NULL;
		g_auto(GValue) value = G_VALUE_INIT;
		JsonNode *member_node;

		if (0 == g_strcmp0(properties[i]->name, "attributes"))
			continue;

		/*
		 * A sensitive field is never accepted from a payload.
		 *
		 * The flag already keeps these values out of every response,
		 * every export, every form and the AI. Letting one back in
		 * through a POST body makes that one-way barrier a
		 * write-only one, which is not what any caller reading the
		 * flag would assume -- and the generic surfaces are exactly
		 * where the assumption gets made. A password is set through
		 * the account page, a forge token through the forge page:
		 * each has a route of its own precisely so that setting a
		 * credential is a deliberate act with its own authorisation,
		 * rather than a member somebody added to a JSON object.
		 */
		if (0 != (venture_entity_class_get_column_flags(klass,
		                                                properties[i]->name) &
		          VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;

		member = venture_entity_property_to_column(properties[i]->name);

		/* An absent member leaves the current value alone. That makes
		 * PATCH semantics fall out for free, and means a client sending
		 * a partial object cannot accidentally blank fields it never
		 * knew about. */
		if (!json_object_has_member(object, member))
			continue;

		member_node = json_object_get_member(object, member);

		if (!venture_json_value_from_node(member_node,
		                                  properties[i]->value_type,
		                                  &value, error))
		{
			g_prefix_error(error, "%s: ",
			               g_param_spec_get_nick(properties[i]));
			return FALSE;
		}

		g_object_set_property(G_OBJECT(self), properties[i]->name, &value);
	}

	if (json_object_has_member(object, "attributes"))
	{
		JsonNode *attributes_node;

		attributes_node = json_object_get_member(object, "attributes");

		if (JSON_NODE_HOLDS_OBJECT(attributes_node))
		{
			JsonObject *attributes;
			g_autoptr(GList) members = NULL;
			GList *iter;

			attributes = json_node_get_object(attributes_node);
			members = json_object_get_members(attributes);

			for (iter = members; NULL != iter; iter = iter->next)
			{
				g_autofree gchar *text = NULL;
				JsonNode *value_node;

				value_node = json_object_get_member(attributes,
				                                    iter->data);

				if (JSON_NODE_HOLDS_NULL(value_node))
				{
					venture_entity_set_attribute(self, iter->data, NULL);
					continue;
				}

				if (JSON_NODE_HOLDS_VALUE(value_node) &&
				    (G_TYPE_STRING == json_node_get_value_type(value_node)))
				{
					venture_entity_set_attribute(self, iter->data,
						json_node_get_string(value_node));
					continue;
				}

				text = venture_json_to_string(value_node, FALSE);
				venture_entity_set_attribute(self, iter->data, text);
			}
		}
	}

	g_signal_emit(self, venture_entity_signals[SIGNAL_CHANGED], 0);

	return TRUE;
}

static const gchar * const *
venture_entity_serializable_get_sensitive_fields(VentureSerializable *serializable)
{
	static GQuark quark = 0;
	VentureEntity *self;
	VentureEntityClass *klass;
	g_autofree GParamSpec **properties = NULL;
	g_autoptr(GPtrArray) names = NULL;
	const gchar * const *cached;
	GType type;
	guint n_properties;
	guint i;

	if (0 == quark)
		quark = g_quark_from_static_string("venture-entity-sensitive-fields");

	self = VENTURE_ENTITY(serializable);
	type = G_OBJECT_TYPE(self);
	cached = g_type_get_qdata(type, quark);

	if (NULL != cached)
		return cached;

	klass = VENTURE_ENTITY_GET_CLASS(self);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);
	names = g_ptr_array_new();

	for (i = 0; i < n_properties; i++)
	{
		VentureColumnFlags flags;

		flags = venture_entity_class_get_column_flags(klass,
		                                              properties[i]->name);

		if (0 != (flags & VENTURE_COLUMN_FLAG_SENSITIVE))
		{
			g_ptr_array_add(names,
				venture_entity_property_to_column(properties[i]->name));
		}
	}

	g_ptr_array_add(names, NULL);

	/* Cached on the type: the answer depends only on the class, and this
	 * is consulted on every serialisation of every record. */
	g_type_set_qdata(type, quark,
		g_ptr_array_free(g_steal_pointer(&names), FALSE));

	return g_type_get_qdata(type, quark);
}

static void
venture_entity_serializable_init(VentureSerializableInterface *iface)
{
	iface->to_json = venture_entity_serializable_to_json;
	iface->from_json = venture_entity_serializable_from_json;
	iface->get_sensitive_fields = venture_entity_serializable_get_sensitive_fields;
}

/* --- GObject boilerplate ------------------------------------------------- */

/*
 * Renders the attributes table as a compact JSON object, which is the form
 * stored in the database column.
 */
static gchar *
venture_entity_attributes_to_string(VentureEntity *self)
{
	VentureEntityPrivate *priv;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GList) keys = NULL;
	GList *iter;

	priv = venture_entity_get_instance_private(self);

	if ((NULL == priv->attributes) || (0 == g_hash_table_size(priv->attributes)))
		return NULL;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	/* Sorted so the stored text is stable: an unsorted hash table would
	 * make every save look like a change to a diff or a backup. */
	keys = g_hash_table_get_keys(priv->attributes);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);

	for (iter = keys; NULL != iter; iter = iter->next)
	{
		json_builder_set_member_name(builder, iter->data);
		json_builder_add_string_value(builder,
			g_hash_table_lookup(priv->attributes, iter->data));
	}

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_json_to_string(node, FALSE);
}

static void
venture_entity_attributes_from_string(
	VentureEntity	*self,
	const gchar	*text
){
	VentureEntityPrivate *priv;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) local_error = NULL;
	JsonObject *object;
	g_autoptr(GList) members = NULL;
	GList *iter;

	priv = venture_entity_get_instance_private(self);

	g_hash_table_remove_all(priv->attributes);

	if (venture_string_is_empty(text))
		return;

	node = venture_json_parse(text, &local_error);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
	{
		/* Corrupt attribute JSON is worth a warning but must not stop
		 * the record loading: the real fields are still intact and
		 * losing them too would turn a small problem into a big one. */
		g_warning("Ignoring unreadable attributes on %s: %s",
		          G_OBJECT_TYPE_NAME(self),
		          (NULL != local_error) ? local_error->message
		                                : "not a JSON object");
		return;
	}

	object = json_node_get_object(node);
	members = json_object_get_members(object);

	for (iter = members; NULL != iter; iter = iter->next)
	{
		JsonNode *value_node;

		value_node = json_object_get_member(object, iter->data);

		if (JSON_NODE_HOLDS_VALUE(value_node) &&
		    (G_TYPE_STRING == json_node_get_value_type(value_node)))
		{
			g_hash_table_insert(priv->attributes, g_strdup(iter->data),
			                    g_strdup(json_node_get_string(value_node)));
			continue;
		}

		g_hash_table_insert(priv->attributes, g_strdup(iter->data),
		                    venture_json_to_string(value_node, FALSE));
	}
}

static void
venture_entity_get_property(
	GObject		*object,
	guint		 prop_id,
	GValue		*value,
	GParamSpec	*pspec
){
	VentureEntity *self;
	VentureEntityPrivate *priv;

	self = VENTURE_ENTITY(object);
	priv = venture_entity_get_instance_private(self);

	switch (prop_id)
	{
	case PROP_ID:
		g_value_set_int64(value, priv->id);
		break;

	case PROP_UUID:
		g_value_set_string(value, venture_entity_get_uuid(self));
		break;

	case PROP_ORGANIZATION_ID:
		g_value_set_int64(value, priv->organization_id);
		break;

	case PROP_CREATED_AT:
		g_value_set_boxed(value, priv->created_at);
		break;

	case PROP_UPDATED_AT:
		g_value_set_boxed(value, priv->updated_at);
		break;

	case PROP_DELETED_AT:
		g_value_set_boxed(value, priv->deleted_at);
		break;

	case PROP_VERSION:
		g_value_set_int64(value, priv->version);
		break;

	case PROP_ATTRIBUTES:
		g_value_take_string(value, venture_entity_attributes_to_string(self));
		break;

	default:
		/* Anything above the base range belongs to a declaratively
		 * installed field, whose value this class stores on the
		 * subclass's behalf. */
		if (prop_id >= VENTURE_ENTITY_FIELD_PROP_BASE)
		{
			const GValue *stored;

			stored = g_hash_table_lookup(priv->fields, pspec->name);

			if (NULL != stored)
				g_value_copy(stored, value);

			/* No stored value means the field was never set, and the
			 * GValue the caller handed us is already at its type
			 * default, which is the right answer. */
			break;
		}

		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
venture_entity_set_property(
	GObject		*object,
	guint		 prop_id,
	const GValue	*value,
	GParamSpec	*pspec
){
	VentureEntity *self;
	VentureEntityPrivate *priv;

	self = VENTURE_ENTITY(object);
	priv = venture_entity_get_instance_private(self);

	switch (prop_id)
	{
	case PROP_ID:
		priv->id = g_value_get_int64(value);
		break;

	case PROP_UUID:
		g_free(priv->uuid);
		priv->uuid = g_value_dup_string(value);
		break;

	case PROP_ORGANIZATION_ID:
		priv->organization_id = g_value_get_int64(value);
		break;

	case PROP_CREATED_AT:
		g_clear_pointer(&priv->created_at, g_date_time_unref);
		priv->created_at = g_value_dup_boxed(value);
		break;

	case PROP_UPDATED_AT:
		g_clear_pointer(&priv->updated_at, g_date_time_unref);
		priv->updated_at = g_value_dup_boxed(value);
		break;

	case PROP_DELETED_AT:
		g_clear_pointer(&priv->deleted_at, g_date_time_unref);
		priv->deleted_at = g_value_dup_boxed(value);
		break;

	case PROP_VERSION:
		priv->version = g_value_get_int64(value);
		break;

	case PROP_ATTRIBUTES:
		venture_entity_attributes_from_string(self,
		                                      g_value_get_string(value));
		break;

	default:
		if (prop_id >= VENTURE_ENTITY_FIELD_PROP_BASE)
		{
			GValue *stored;

			stored = g_new0(GValue, 1);
			g_value_init(stored, pspec->value_type);
			g_value_copy(value, stored);

			g_hash_table_insert(priv->fields, g_strdup(pspec->name),
			                    stored);
			break;
		}

		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

/*
 * Frees a boxed GValue held in the field store.
 */
static void
venture_entity_free_stored_value(gpointer data)
{
	GValue *value;

	value = data;

	g_value_unset(value);
	g_free(value);
}

void
venture_entity_class_install_fields(
	VentureEntityClass		*klass,
	const VentureFieldDecl		*fields,
	guint				 n_fields
){
	GObjectClass *object_class;
	guint next_prop_id;
	guint i;

	g_return_if_fail(VENTURE_IS_ENTITY_CLASS(klass));
	g_return_if_fail((NULL != fields) || (0 == n_fields));

	object_class = G_OBJECT_CLASS(klass);

	/*
	 * Point the subclass at the base accessors explicitly.
	 *
	 * A derived class does not reliably see its parent's get_property and
	 * set_property at the moment its own class_init runs -- GObject copies
	 * the parent class struct before the parent's class_init has
	 * necessarily populated it -- and g_object_class_install_property()
	 * asserts that a writable property has a set_property to reach. Since
	 * every declarative subclass wants exactly these implementations,
	 * assigning them here is both correct and the only thing a subclass
	 * would ever have written by hand.
	 */
	object_class->get_property = venture_entity_get_property;
	object_class->set_property = venture_entity_set_property;

	/* Each class gets its own contiguous block of identifiers above the
	 * base. Offsetting by the number of properties already installed --
	 * including any the parent declared -- keeps blocks from overlapping
	 * when a declarative type is subclassed again. */
	{
		g_autofree GParamSpec **existing = NULL;
		guint n_existing;

		existing = g_object_class_list_properties(object_class, &n_existing);
		next_prop_id = VENTURE_ENTITY_FIELD_PROP_BASE + n_existing;
	}

	for (i = 0; i < n_fields; i++)
	{
		const VentureFieldDecl *decl;
		GParamSpec *pspec;
		const gchar *label;
		const gchar *blurb;

		decl = &fields[i];

		g_return_if_fail(NULL != decl->name);

		/*
		 * A field may not take a name the identity spine owns. A
		 * release with a field called "version" would install a string
		 * property over the optimistic-concurrency counter, and every
		 * second save of one would fail as a conflict with itself.
		 * That happened; this is what stops it happening again, at
		 * class initialisation rather than on the second save.
		 */
		if (NULL != g_object_class_find_property(G_OBJECT_CLASS(klass),
		                                         decl->name))
		{
			g_error("%s declares a field named \"%s\", which %s already "
			        "owns; pick another name",
			        G_OBJECT_CLASS_NAME(klass), decl->name,
			        g_strv_contains(venture_entity_identity_properties,
			                        decl->name)
			                ? "the entity spine" : "a parent class");
		}

		label = (NULL != decl->label) ? decl->label : decl->name;
		blurb = (NULL != decl->help) ? decl->help : label;

		switch (decl->kind)
		{
		case VENTURE_FIELD_KIND_INTEGER:
		case VENTURE_FIELD_KIND_REFERENCE:
			pspec = g_param_spec_int64(decl->name, label, blurb,
			                           G_MININT64, G_MAXINT64, 0,
			                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
			break;

		case VENTURE_FIELD_KIND_DOUBLE:
			pspec = g_param_spec_double(decl->name, label, blurb,
			                            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
			                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
			break;

		case VENTURE_FIELD_KIND_BOOLEAN:
			pspec = g_param_spec_boolean(decl->name, label, blurb, FALSE,
			                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
			break;

		case VENTURE_FIELD_KIND_MONEY:
			pspec = g_param_spec_boxed(decl->name, label, blurb,
			                           VENTURE_TYPE_MONEY,
			                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
			break;

		case VENTURE_FIELD_KIND_DATE:
		case VENTURE_FIELD_KIND_DATETIME:
			pspec = g_param_spec_boxed(decl->name, label, blurb,
			                           G_TYPE_DATE_TIME,
			                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
			break;

		case VENTURE_FIELD_KIND_ENUM:
			/* An enum field backed by a registered GEnum becomes a
			 * real enum property, so its permitted values reach the
			 * form and the AI schema straight from the C definition.
			 * One declared without a GType falls back to a string,
			 * which is how YAML-declared enumerations work. */
			if (NULL != decl->enum_type_func)
			{
				pspec = g_param_spec_enum(decl->name, label, blurb,
				                          decl->enum_type_func(), 0,
				                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
			}
			else
			{
				pspec = g_param_spec_string(decl->name, label, blurb, NULL,
				                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
			}
			break;

		case VENTURE_FIELD_KIND_STRING:
		case VENTURE_FIELD_KIND_TEXT:
		case VENTURE_FIELD_KIND_JSON:
		default:
			pspec = g_param_spec_string(decl->name, label, blurb, NULL,
			                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
			break;
		}

		g_object_class_install_property(object_class, next_prop_id, pspec);
		next_prop_id++;

		/* Position in the table, so lists and forms read in the order
		 * the type was written rather than in GObject's. */
		venture_entity_class_set_field_order(klass, decl->name, (guint)i);

		/* The declared kind, because value_type cannot tell a long text
		 * field from a short string one. */
		venture_entity_class_set_field_kind(klass, decl->name, decl->kind);

		if (VENTURE_COLUMN_FLAG_NONE != decl->flags)
			venture_entity_class_set_column_flags(klass, decl->name, decl->flags);

		if (NULL != decl->reference)
			venture_entity_class_set_reference(klass, decl->name, decl->reference);
	}
}

gboolean
venture_entity_get_field(
	VentureEntity	*self,
	const gchar	*name,
	GValue		*out_value
){
	GParamSpec *pspec;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), FALSE);
	g_return_val_if_fail(NULL != name, FALSE);
	g_return_val_if_fail(NULL != out_value, FALSE);

	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(self), name);

	if (NULL == pspec)
		return FALSE;

	g_value_init(out_value, pspec->value_type);
	g_object_get_property(G_OBJECT(self), name, out_value);

	return TRUE;
}

gboolean
venture_entity_set_field_from_string(
	VentureEntity	 *self,
	const gchar	 *name,
	const gchar	 *text,
	GError		**error
){
	g_auto(GValue) value = G_VALUE_INIT;
	g_autoptr(JsonNode) node = NULL;
	GParamSpec *pspec;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), FALSE);
	g_return_val_if_fail(NULL != name, FALSE);

	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(self), name);

	if (NULL == pspec)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s has no field \"%s\"",
		            venture_entity_get_entity_name(self), name);
		return FALSE;
	}

	/* Routing through a JSON string node reuses the one tolerant decoder
	 * that already understands every textual form these types accept --
	 * money with a symbol, a date as YYYY-MM-DD, a boolean spelled "on". */
	node = (NULL != text)
		? json_node_init_string(json_node_alloc(), text)
		: json_node_new(JSON_NODE_NULL);

	if (!venture_json_value_from_node(node, pspec->value_type, &value, error))
	{
		g_prefix_error(error, "%s: ", g_param_spec_get_nick(pspec));
		return FALSE;
	}

	g_object_set_property(G_OBJECT(self), name, &value);

	return TRUE;
}

static void
venture_entity_finalize(GObject *object)
{
	VentureEntity *self;
	VentureEntityPrivate *priv;

	self = VENTURE_ENTITY(object);
	priv = venture_entity_get_instance_private(self);

	g_clear_pointer(&priv->uuid, g_free);
	g_clear_pointer(&priv->created_at, g_date_time_unref);
	g_clear_pointer(&priv->updated_at, g_date_time_unref);
	g_clear_pointer(&priv->deleted_at, g_date_time_unref);
	g_clear_pointer(&priv->attributes, g_hash_table_unref);
	g_clear_pointer(&priv->fields, g_hash_table_unref);

	G_OBJECT_CLASS(venture_entity_parent_class)->finalize(object);
}

static void
venture_entity_class_init(VentureEntityClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = venture_entity_get_property;
	object_class->set_property = venture_entity_set_property;
	object_class->finalize = venture_entity_finalize;

	klass->get_entity_name = venture_entity_real_get_entity_name;
	klass->get_table_name = venture_entity_real_get_table_name;
	klass->get_display_name = venture_entity_real_get_display_name;
	klass->get_search_text = venture_entity_real_get_search_text;
	klass->validate = venture_entity_real_validate;
	klass->before_save = venture_entity_real_before_save;
	klass->after_load = venture_entity_real_after_load;
	klass->get_field_specs = venture_entity_real_get_field_specs;

	/**
	 * VentureEntity:id:
	 *
	 * The surrogate primary key. Zero until the record is first saved.
	 */
	venture_entity_properties[PROP_ID] =
		g_param_spec_int64("id", "ID", "Primary key",
		                   0, G_MAXINT64, 0,
		                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * VentureEntity:uuid:
	 *
	 * A stable external identifier, generated on first access.
	 */
	venture_entity_properties[PROP_UUID] =
		g_param_spec_string("uuid", "UUID", "Stable external identifier",
		                    NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * VentureEntity:organization-id:
	 *
	 * The owning organisation. Every query is scoped by this, which is
	 * what keeps a personal entity's records out of a business's reports.
	 */
	venture_entity_properties[PROP_ORGANIZATION_ID] =
		g_param_spec_int64("organization-id", "Organization",
		                   "Owning organization",
		                   0, G_MAXINT64, 0,
		                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	venture_entity_properties[PROP_CREATED_AT] =
		g_param_spec_boxed("created-at", "Created", "When the record was created",
		                   G_TYPE_DATE_TIME,
		                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	venture_entity_properties[PROP_UPDATED_AT] =
		g_param_spec_boxed("updated-at", "Updated", "When the record last changed",
		                   G_TYPE_DATE_TIME,
		                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * VentureEntity:deleted-at:
	 *
	 * When the record was soft deleted, or %NULL if it is live. Records
	 * are never physically removed: an expense that appeared in a filed
	 * return has to remain reconstructable.
	 */
	venture_entity_properties[PROP_DELETED_AT] =
		g_param_spec_boxed("deleted-at", "Deleted", "When the record was deleted",
		                   G_TYPE_DATE_TIME,
		                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * VentureEntity:version:
	 *
	 * An optimistic-concurrency counter, incremented on every save. A
	 * save whose version does not match the stored one is rejected, so
	 * two windows editing the same record cannot silently overwrite each
	 * other.
	 */
	venture_entity_properties[PROP_VERSION] =
		g_param_spec_int64("version", "Version", "Optimistic concurrency version",
		                   0, G_MAXINT64, 0,
		                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * VentureEntity:attributes:
	 *
	 * Free-form attributes, serialised as a JSON object. Use
	 * venture_entity_set_attribute() rather than this property directly.
	 */
	venture_entity_properties[PROP_ATTRIBUTES] =
		g_param_spec_string("attributes", "Attributes",
		                    "Custom attributes as JSON",
		                    NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPERTIES,
	                                  venture_entity_properties);

	/* The identity spine's persistence hints. Declared here once so every
	 * subclass inherits them. */
	venture_entity_class_set_column_flags(klass, "id",
		VENTURE_COLUMN_FLAG_PRIMARY_KEY | VENTURE_COLUMN_FLAG_NOT_NULL);
	venture_entity_class_set_column_flags(klass, "uuid",
		VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_INDEXED |
		VENTURE_COLUMN_FLAG_IMMUTABLE | VENTURE_COLUMN_FLAG_NOT_NULL);
	venture_entity_class_set_column_flags(klass, "organization-id",
		VENTURE_COLUMN_FLAG_INDEXED);
	venture_entity_class_set_column_flags(klass, "created-at",
		VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_IMMUTABLE);
	venture_entity_class_set_column_flags(klass, "deleted-at",
		VENTURE_COLUMN_FLAG_INDEXED);
	venture_entity_class_set_reference(klass, "organization-id", "organization");

	/**
	 * VentureEntity::changed:
	 * @self: the entity
	 *
	 * Emitted when the record's data has been replaced wholesale, as
	 * happens when it is populated from JSON. Individual property changes
	 * raise the usual #GObject::notify instead.
	 */
	venture_entity_signals[SIGNAL_CHANGED] =
		g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 0);

	/**
	 * VentureEntity::validated:
	 * @self: the entity
	 * @valid: whether validation passed
	 *
	 * Emitted after venture_entity_validate() runs. Plugins connect to
	 * this to add cross-record rules without subclassing.
	 */
	venture_entity_signals[SIGNAL_VALIDATED] =
		g_signal_new("validated", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
venture_entity_init(VentureEntity *self)
{
	VentureEntityPrivate *priv;

	priv = venture_entity_get_instance_private(self);

	priv->id = 0;
	priv->version = 0;
	priv->attributes = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                         g_free, g_free);
	priv->fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     venture_entity_free_stored_value);

	/* Stamping creation time here rather than at save time means a record
	 * built and held in memory reports when it was actually made. */
	priv->created_at = venture_time_now();
	priv->updated_at = g_date_time_ref(priv->created_at);
}

/* --- Identity ------------------------------------------------------------ */

gint64
venture_entity_get_id(VentureEntity *self)
{
	VentureEntityPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), 0);

	priv = venture_entity_get_instance_private(self);

	return priv->id;
}

void
venture_entity_set_id(
	VentureEntity	*self,
	gint64		 id
){
	VentureEntityPrivate *priv;

	g_return_if_fail(VENTURE_IS_ENTITY(self));

	priv = venture_entity_get_instance_private(self);

	if (priv->id == id)
		return;

	priv->id = id;
	g_object_notify_by_pspec(G_OBJECT(self),
	                         venture_entity_properties[PROP_ID]);
}

gboolean
venture_entity_is_persisted(VentureEntity *self)
{
	return (0 != venture_entity_get_id(self));
}

const gchar *
venture_entity_get_uuid(VentureEntity *self)
{
	VentureEntityPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	priv = venture_entity_get_instance_private(self);

	/* Generated lazily rather than in init, so constructing a throwaway
	 * entity to inspect its schema does not burn entropy. */
	if (NULL == priv->uuid)
		priv->uuid = g_uuid_string_random();

	return priv->uuid;
}

gint64
venture_entity_get_organization_id(VentureEntity *self)
{
	VentureEntityPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), 0);

	priv = venture_entity_get_instance_private(self);

	return priv->organization_id;
}

void
venture_entity_set_organization_id(
	VentureEntity	*self,
	gint64		 organization_id
){
	g_return_if_fail(VENTURE_IS_ENTITY(self));

	g_object_set(self, "organization-id", organization_id, NULL);
}

GDateTime *
venture_entity_get_created_at(VentureEntity *self)
{
	VentureEntityPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	priv = venture_entity_get_instance_private(self);

	return priv->created_at;
}

GDateTime *
venture_entity_get_updated_at(VentureEntity *self)
{
	VentureEntityPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	priv = venture_entity_get_instance_private(self);

	return priv->updated_at;
}

GDateTime *
venture_entity_get_deleted_at(VentureEntity *self)
{
	VentureEntityPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	priv = venture_entity_get_instance_private(self);

	return priv->deleted_at;
}

gboolean
venture_entity_is_deleted(VentureEntity *self)
{
	return (NULL != venture_entity_get_deleted_at(self));
}

gint64
venture_entity_get_version(VentureEntity *self)
{
	VentureEntityPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), 0);

	priv = venture_entity_get_instance_private(self);

	return priv->version;
}

void
venture_entity_touch(VentureEntity *self)
{
	VentureEntityPrivate *priv;

	g_return_if_fail(VENTURE_IS_ENTITY(self));

	priv = venture_entity_get_instance_private(self);

	g_clear_pointer(&priv->updated_at, g_date_time_unref);
	priv->updated_at = venture_time_now();
	priv->version += 1;

	g_object_notify_by_pspec(G_OBJECT(self),
	                         venture_entity_properties[PROP_UPDATED_AT]);
	g_object_notify_by_pspec(G_OBJECT(self),
	                         venture_entity_properties[PROP_VERSION]);
}

/* --- Naming -------------------------------------------------------------- */

const gchar *
venture_entity_get_entity_name(VentureEntity *self)
{
	VentureEntityClass *klass;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	klass = VENTURE_ENTITY_GET_CLASS(self);
	g_return_val_if_fail(NULL != klass->get_entity_name, NULL);

	return klass->get_entity_name(self);
}

const gchar *
venture_entity_get_table_name(VentureEntity *self)
{
	VentureEntityClass *klass;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	klass = VENTURE_ENTITY_GET_CLASS(self);
	g_return_val_if_fail(NULL != klass->get_table_name, NULL);

	return klass->get_table_name(self);
}

gchar *
venture_entity_get_display_name(VentureEntity *self)
{
	VentureEntityClass *klass;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	klass = VENTURE_ENTITY_GET_CLASS(self);
	g_return_val_if_fail(NULL != klass->get_display_name, NULL);

	return klass->get_display_name(self);
}

gchar *
venture_entity_get_search_text(VentureEntity *self)
{
	VentureEntityClass *klass;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	klass = VENTURE_ENTITY_GET_CLASS(self);

	if (NULL == klass->get_search_text)
		return NULL;

	return klass->get_search_text(self);
}

/* --- Lifecycle ----------------------------------------------------------- */

gboolean
venture_entity_validate(
	VentureEntity	 *self,
	GError		**error
){
	VentureEntityClass *klass;
	gboolean valid;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), FALSE);

	klass = VENTURE_ENTITY_GET_CLASS(self);
	g_return_val_if_fail(NULL != klass->validate, FALSE);

	valid = klass->validate(self, error);

	g_signal_emit(self, venture_entity_signals[SIGNAL_VALIDATED], 0, valid);

	return valid;
}

gboolean
venture_entity_before_save(
	VentureEntity	 *self,
	GError		**error
){
	VentureEntityClass *klass;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), FALSE);

	klass = VENTURE_ENTITY_GET_CLASS(self);

	if (NULL == klass->before_save)
		return TRUE;

	return klass->before_save(self, error);
}

void
venture_entity_after_load(VentureEntity *self)
{
	VentureEntityClass *klass;

	g_return_if_fail(VENTURE_IS_ENTITY(self));

	klass = VENTURE_ENTITY_GET_CLASS(self);

	if (NULL != klass->after_load)
		klass->after_load(self);
}

GPtrArray *
venture_entity_get_field_specs(VentureEntity *self)
{
	VentureEntityClass *klass;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	klass = VENTURE_ENTITY_GET_CLASS(self);
	g_return_val_if_fail(NULL != klass->get_field_specs, NULL);

	return klass->get_field_specs(self);
}

/* --- Custom attributes --------------------------------------------------- */

void
venture_entity_set_attribute(
	VentureEntity	*self,
	const gchar	*key,
	const gchar	*value
){
	VentureEntityPrivate *priv;

	g_return_if_fail(VENTURE_IS_ENTITY(self));
	g_return_if_fail(NULL != key);

	priv = venture_entity_get_instance_private(self);

	if (NULL == value)
		g_hash_table_remove(priv->attributes, key);
	else
		g_hash_table_insert(priv->attributes, g_strdup(key), g_strdup(value));

	g_object_notify_by_pspec(G_OBJECT(self),
	                         venture_entity_properties[PROP_ATTRIBUTES]);
}

const gchar *
venture_entity_get_attribute(
	VentureEntity	*self,
	const gchar	*key
){
	VentureEntityPrivate *priv;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);
	g_return_val_if_fail(NULL != key, NULL);

	priv = venture_entity_get_instance_private(self);

	return g_hash_table_lookup(priv->attributes, key);
}

GList *
venture_entity_list_attributes(VentureEntity *self)
{
	VentureEntityPrivate *priv;
	GList *keys;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	priv = venture_entity_get_instance_private(self);
	keys = g_hash_table_get_keys(priv->attributes);

	return g_list_sort(keys, (GCompareFunc)g_strcmp0);
}

/* --- Copying and comparison ---------------------------------------------- */

void
venture_entity_copy_properties_from(
	VentureEntity	*self,
	VentureEntity	*source,
	gboolean	 skip_identity
){
	VentureEntityClass *klass;
	g_autofree GParamSpec **properties = NULL;
	guint n_properties;
	guint i;

	g_return_if_fail(VENTURE_IS_ENTITY(self));
	g_return_if_fail(VENTURE_IS_ENTITY(source));
	g_return_if_fail(G_OBJECT_TYPE(self) == G_OBJECT_TYPE(source));

	klass = VENTURE_ENTITY_GET_CLASS(self);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);

	for (i = 0; i < n_properties; i++)
	{
		g_auto(GValue) value = G_VALUE_INIT;

		if (skip_identity &&
		    g_strv_contains(venture_entity_identity_properties,
		                    properties[i]->name))
			continue;

		g_value_init(&value, properties[i]->value_type);
		g_object_get_property(G_OBJECT(source), properties[i]->name, &value);
		g_object_set_property(G_OBJECT(self), properties[i]->name, &value);
	}
}

VentureEntity *
venture_entity_duplicate(VentureEntity *self)
{
	VentureEntity *copy;
	VentureEntityPrivate *source_priv;
	VentureEntityPrivate *copy_priv;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);

	copy = g_object_new(G_OBJECT_TYPE(self), NULL);

	/* Skipping the identity spine is the whole point: the result must
	 * insert as a new row, not update the original. */
	venture_entity_copy_properties_from(copy, self, TRUE);

	source_priv = venture_entity_get_instance_private(self);
	copy_priv = venture_entity_get_instance_private(copy);

	g_hash_table_iter_init(&iter, source_priv->attributes);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_hash_table_insert(copy_priv->attributes,
		                    g_strdup(key), g_strdup(value));
	}

	/* A duplicate is a new record even though its data is old, so it is
	 * live and unversioned regardless of the original's state. */
	g_clear_pointer(&copy_priv->deleted_at, g_date_time_unref);
	copy_priv->version = 0;

	return copy;
}

/*
 * Compares two property values for equality.
 *
 * GObject's g_param_values_cmp() compares boxed values by pointer, which is
 * useless here: a #VentureMoney read back from the database is a different
 * allocation from the one in memory even when it holds the same amount, and
 * a pointer comparison would report every field as changed on every save.
 * That would fill the audit log with noise and make an AI confirmation
 * diff show fields nobody touched.
 *
 * Boxed types VENTURE knows about are therefore compared by value, and
 * everything else falls back to GObject's comparison.
 */
static gboolean
venture_entity_values_equal(
	GParamSpec	*pspec,
	const GValue	*a,
	const GValue	*b
){
	if (VENTURE_TYPE_MONEY == pspec->value_type)
	{
		return venture_money_equal(g_value_get_boxed(a),
		                          g_value_get_boxed(b));
	}

	if (G_TYPE_DATE_TIME == pspec->value_type)
	{
		return venture_time_equal(g_value_get_boxed(a),
		                         g_value_get_boxed(b));
	}

	if (VENTURE_TYPE_DATE_RANGE == pspec->value_type)
	{
		return venture_date_range_equal(g_value_get_boxed(a),
		                               g_value_get_boxed(b));
	}

	if (G_TYPE_STRV == pspec->value_type)
	{
		const gchar * const *left;
		const gchar * const *right;
		gsize i;

		left = g_value_get_boxed(a);
		right = g_value_get_boxed(b);

		if ((NULL == left) || (NULL == right))
			return (left == right);

		for (i = 0; (NULL != left[i]) && (NULL != right[i]); i++)
		{
			if (0 != g_strcmp0(left[i], right[i]))
				return FALSE;
		}

		return (NULL == left[i]) && (NULL == right[i]);
	}

	return (0 == g_param_values_cmp(pspec, a, b));
}

JsonNode *
venture_entity_diff(
	VentureEntity	*self,
	VentureEntity	*other
){
	VentureEntityClass *klass;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree GParamSpec **properties = NULL;
	guint n_properties;
	guint i;

	g_return_val_if_fail(VENTURE_IS_ENTITY(self), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(other), NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	if (G_OBJECT_TYPE(self) != G_OBJECT_TYPE(other))
	{
		/* Comparing different types is meaningless; report it as a
		 * whole-object replacement rather than silently returning an
		 * empty diff that would read as "no change". */
		json_builder_set_member_name(builder, "type");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "from");
		json_builder_add_string_value(builder, G_OBJECT_TYPE_NAME(self));
		json_builder_set_member_name(builder, "to");
		json_builder_add_string_value(builder, G_OBJECT_TYPE_NAME(other));
		json_builder_end_object(builder);
		json_builder_end_object(builder);

		return json_builder_get_root(builder);
	}

	klass = VENTURE_ENTITY_GET_CLASS(self);
	properties = venture_entity_class_list_persistent_properties(klass,
	                                                             &n_properties);

	for (i = 0; i < n_properties; i++)
	{
		VentureColumnFlags flags;
		g_auto(GValue) before = G_VALUE_INIT;
		g_auto(GValue) after = G_VALUE_INIT;
		g_autofree gchar *member = NULL;

		/* Timestamps and the version counter change on every save and
		 * would drown the real differences. */
		if (g_strv_contains(venture_entity_identity_properties,
		                    properties[i]->name))
			continue;

		g_value_init(&before, properties[i]->value_type);
		g_value_init(&after, properties[i]->value_type);
		g_object_get_property(G_OBJECT(self), properties[i]->name, &before);
		g_object_get_property(G_OBJECT(other), properties[i]->name, &after);

		if (venture_entity_values_equal(properties[i], &before, &after))
			continue;

		member = venture_entity_property_to_column(properties[i]->name);
		flags = venture_entity_class_get_column_flags(klass,
		                                              properties[i]->name);

		json_builder_set_member_name(builder, member);
		json_builder_begin_object(builder);

		if (0 != (flags & VENTURE_COLUMN_FLAG_SENSITIVE) ||
			(g_str_equal(properties[i]->name, "attributes") &&
			(attributes_contain_secret(self) || attributes_contain_secret(other))))
		{
			/* The operator needs to know a password or token changed;
			 * they do not need either value written into an audit log
			 * that is itself readable. */
			json_builder_set_member_name(builder, "changed");
			json_builder_add_boolean_value(builder, TRUE);
			json_builder_set_member_name(builder, "redacted");
			json_builder_add_boolean_value(builder, TRUE);
		}
		else
		{
			json_builder_set_member_name(builder, "from");
			json_builder_add_value(builder,
				venture_json_node_from_value(&before));
			json_builder_set_member_name(builder, "to");
			json_builder_add_value(builder,
				venture_json_node_from_value(&after));
		}

		json_builder_end_object(builder);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

gboolean
venture_entity_equal(
	VentureEntity	*a,
	VentureEntity	*b
){
	g_autoptr(JsonNode) differences = NULL;

	if (a == b)
		return TRUE;

	if ((NULL == a) || (NULL == b))
		return FALSE;

	if (G_OBJECT_TYPE(a) != G_OBJECT_TYPE(b))
		return FALSE;

	differences = venture_entity_diff(a, b);

	return (0 == json_object_get_size(json_node_get_object(differences)));
}
