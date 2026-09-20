/*
 * venture-entity.h - The abstract base class every persisted record derives from
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * #VentureEntity is the single extension point of the whole system. Define a
 * GObject subclass of it, install GObject properties, register it, and you
 * automatically get:
 *
 *   - a database table, with columns, types, constraints and indexes derived
 *     from the properties
 *   - full REST CRUD at /api/v1/<name> with filtering, sorting and paging
 *   - JSON and YAML serialisation that round-trips
 *   - a web UI list view and edit form
 *   - AI tools that can query and (subject to policy) modify it
 *   - audit records for every change
 *   - CLI subcommands in venturectl
 *
 * without writing SQL, a route, a form, a tool definition or a serialiser.
 * That is the point: adding "I now also sell laser-cut art" should be a
 * couple of dozen lines, or a YAML file, not a project.
 *
 * The mechanism is GObject property introspection plus a small amount of
 * per-property metadata attached with venture_entity_class_set_column_flags().
 * Property names use lowercase-with-hyphens as GObject requires; column and
 * JSON names use lowercase_with_underscores. The translation is automatic and
 * total, so the two vocabularies never have to be maintained side by side.
 *
 * Every entity carries the same spine:
 *
 *   id          the surrogate primary key; zero until first saved
 *   uuid        a stable external identifier, safe to expose and to
 *               reference from another system
 *   created-at  when the record first existed
 *   updated-at  when it last changed
 *   version     an optimistic-concurrency counter; a save whose version
 *               does not match the stored one fails with
 *               %VENTURE_ERROR_CONFLICT rather than silently clobbering
 *   deleted-at  soft deletion, so nothing that touched a tax filing is
 *               ever truly gone
 *   organization-id  the owning entity, which scopes every query
 */

#ifndef VENTURE_ENTITY_H
#define VENTURE_ENTITY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_ENTITY (venture_entity_get_type())

G_DECLARE_DERIVABLE_TYPE(VentureEntity, venture_entity, VENTURE, ENTITY, GObject)

/**
 * VentureEntityClass:
 * @parent_class: the parent class
 * @get_entity_name: the singular machine name, e.g. "sale"
 * @get_table_name: the database table name; defaults to the plural of the
 *   entity name
 * @get_display_name: a human label for one record, used in lists, audit
 *   entries and AI responses
 * @get_search_text: text this record should match on in a free-text search
 * @validate: check the record's own invariants before it is saved
 * @before_save: last chance to compute derived fields
 * @after_load: fix up state after the record is read back
 * @get_field_specs: describe the record's fields for form and schema
 *   generation; the default derives them from the GObject properties
 *
 * The class vtable for #VentureEntity. Every method has a working default,
 * so a subclass that only installs properties is already fully functional.
 */
struct _VentureEntityClass
{
	GObjectClass parent_class;

	const gchar *(*get_entity_name)  (VentureEntity *self);
	const gchar *(*get_table_name)   (VentureEntity *self);
	gchar       *(*get_display_name) (VentureEntity *self);
	gchar       *(*get_search_text)  (VentureEntity *self);

	gboolean     (*validate)         (VentureEntity  *self,
	                                  GError        **error);

	gboolean     (*before_save)      (VentureEntity  *self,
	                                  GError        **error);

	void         (*after_load)       (VentureEntity *self);

	GPtrArray   *(*get_field_specs)  (VentureEntity *self);

	/*< private >*/
	gpointer padding[8];
};

/**
 * venture_entity_class_set_unique_partition:
 * @klass: entity class with installed fields
 * @property: persistent integer property partitioning organization uniqueness
 *
 * Extends UNIQUE_ORGANIZATION identifiers to (organization, partition, value).
 * NULL and zero share the legacy unbound partition. Declare once in class_init;
 * partition values must be checked by the owning subsystem's save validator.
 */
void venture_entity_class_set_unique_partition(VentureEntityClass *klass, const gchar *property);
/**
 * venture_entity_class_get_unique_partition:
 * @klass: entity class
 * Returns: (nullable): borrowed partition property, or NULL for organization scope
 */
const gchar *venture_entity_class_get_unique_partition(VentureEntityClass *klass);

/* --- Identity ------------------------------------------------------------ */

/**
 * venture_entity_get_id:
 * @self: a #VentureEntity
 *
 * Returns: the primary key, or 0 if the record has never been saved
 */
gint64
venture_entity_get_id(VentureEntity *self);

/**
 * venture_entity_set_id:
 * @self: a #VentureEntity
 * @id: the primary key
 *
 * Sets the primary key. Called by the repository after an insert; setting it
 * by hand on a record you intend to save will attempt an update of that row.
 */
void
venture_entity_set_id(
	VentureEntity	*self,
	gint64		 id
);

/**
 * venture_entity_is_persisted:
 * @self: a #VentureEntity
 *
 * Returns: %TRUE if the record has a primary key and therefore exists in
 *   the database
 */
gboolean
venture_entity_is_persisted(VentureEntity *self);

/**
 * venture_entity_get_uuid:
 * @self: a #VentureEntity
 *
 * Retrieves the stable external identifier, generating one on first access
 * if the record does not have one yet.
 *
 * Returns: (transfer none): the UUID
 */
const gchar *
venture_entity_get_uuid(VentureEntity *self);

/**
 * venture_entity_get_organization_id:
 * @self: a #VentureEntity
 *
 * Returns: the owning organisation's id, or 0 if the record is global
 */
gint64
venture_entity_get_organization_id(VentureEntity *self);

/**
 * venture_entity_set_organization_id:
 * @self: a #VentureEntity
 * @organization_id: the owning organisation
 */
void
venture_entity_set_organization_id(
	VentureEntity	*self,
	gint64		 organization_id
);

/**
 * venture_entity_get_created_at:
 * @self: a #VentureEntity
 *
 * Returns: (transfer none) (nullable): when the record was created
 */
GDateTime *
venture_entity_get_created_at(VentureEntity *self);

/**
 * venture_entity_get_updated_at:
 * @self: a #VentureEntity
 *
 * Returns: (transfer none) (nullable): when the record last changed
 */
GDateTime *
venture_entity_get_updated_at(VentureEntity *self);

/**
 * venture_entity_get_deleted_at:
 * @self: a #VentureEntity
 *
 * Returns: (transfer none) (nullable): when the record was soft deleted, or
 *   %NULL if it is live
 */
GDateTime *
venture_entity_get_deleted_at(VentureEntity *self);

/**
 * venture_entity_is_deleted:
 * @self: a #VentureEntity
 *
 * Returns: %TRUE if the record has been soft deleted
 */
gboolean
venture_entity_is_deleted(VentureEntity *self);

/**
 * venture_entity_get_version:
 * @self: a #VentureEntity
 *
 * Returns: the optimistic-concurrency version
 */
gint64
venture_entity_get_version(VentureEntity *self);

/**
 * venture_entity_touch:
 * @self: a #VentureEntity
 *
 * Stamps the update time and increments the version. The repository calls
 * this; it is exposed so a bulk operation that bypasses the repository can
 * keep the bookkeeping honest.
 */
void
venture_entity_touch(VentureEntity *self);

/* --- Naming and description ---------------------------------------------- */

/**
 * venture_entity_get_entity_name:
 * @self: a #VentureEntity
 *
 * Retrieves the singular machine name, for example "sale". This is the name
 * used in REST paths, CLI subcommands, AI tool arguments and audit records.
 *
 * Returns: (transfer none): the entity name
 */
const gchar *
venture_entity_get_entity_name(VentureEntity *self);

/**
 * venture_entity_get_table_name:
 * @self: a #VentureEntity
 *
 * Returns: (transfer none): the database table name
 */
const gchar *
venture_entity_get_table_name(VentureEntity *self);

/**
 * venture_entity_get_display_name:
 * @self: a #VentureEntity
 *
 * Produces a human label for this record. The default uses a "name",
 * "title", "label" or "description" property if one exists, and falls back
 * to "<entity name> #<id>".
 *
 * Returns: (transfer full): the label
 */
gchar *
venture_entity_get_display_name(VentureEntity *self);

/**
 * venture_entity_get_search_text:
 * @self: a #VentureEntity
 *
 * Produces the text a free-text search should match against. The default
 * concatenates every property flagged %VENTURE_COLUMN_FLAG_SEARCHABLE.
 *
 * Returns: (transfer full) (nullable): the searchable text
 */
gchar *
venture_entity_get_search_text(VentureEntity *self);

/* --- Validation and lifecycle -------------------------------------------- */

/**
 * venture_entity_validate:
 * @self: a #VentureEntity
 * @error: (out) (optional): return location for a #GError
 *
 * Checks the record's invariants. The default enforces the constraints
 * implied by the property metadata -- required fields present, string
 * lengths, numeric bounds -- and a subclass override should chain up before
 * adding its own rules.
 *
 * Returns: %TRUE if the record is valid
 */
gboolean
venture_entity_validate(
	VentureEntity	 *self,
	GError		**error
);

/**
 * venture_entity_before_save:
 * @self: a #VentureEntity
 * @error: (out) (optional): return location for a #GError
 *
 * Invoked by the repository immediately before an insert or update, after
 * validation. Use it to compute derived fields.
 *
 * Returns: %TRUE to proceed with the save
 */
gboolean
venture_entity_before_save(
	VentureEntity	 *self,
	GError		**error
);

/**
 * venture_entity_after_load:
 * @self: a #VentureEntity
 *
 * Invoked by the repository after a record is populated from the database.
 */
void
venture_entity_after_load(VentureEntity *self);

/* --- Declarative field installation --------------------------------------- */

/**
 * VentureFieldDecl:
 * @name: the property name, in lowercase-with-hyphens
 * @label: (nullable): a human label; derived from @name when %NULL
 * @help: (nullable): a one-line description, shown as form help text and
 *   included in the AI tool schema
 * @kind: the field type
 * @enum_type_func: (nullable) (scope call): for %VENTURE_FIELD_KIND_ENUM
 *   backed by a registered #GEnum, the accessor returning its #GType
 * @reference: (nullable): for %VENTURE_FIELD_KIND_REFERENCE, the entity name
 *   this points at
 * @flags: persistence hints
 *
 * One row of a record type's field table.
 *
 * Declaring fields this way rather than writing GObject boilerplate is not
 * merely shorter: it is what makes storage generic. #VentureEntity keeps the
 * values itself, so a subclass needs no instance struct, no property
 * enumeration, no get_property and no set_property. A record type becomes a
 * static table and four lines of class_init, and a plugin defining one gets
 * exactly the same treatment as a built-in.
 */
typedef struct
{
	const gchar		*name;
	const gchar		*label;
	const gchar		*help;
	VentureFieldKind	 kind;
	GType			(*enum_type_func) (void);
	const gchar		*reference;
	VentureColumnFlags	 flags;
} VentureFieldDecl;

/**
 * venture_entity_class_install_fields:
 * @klass: a #VentureEntityClass
 * @fields: (array length=n_fields): the field table
 * @n_fields: the number of entries
 *
 * Installs GObject properties for a field table and arranges for
 * #VentureEntity to store their values. Call this from class_init.
 *
 * Property identifiers are assigned automatically, starting above those the
 * base class uses, so a subclass never has to coordinate numbering with its
 * parent.
 */
void
venture_entity_class_install_fields(
	VentureEntityClass		*klass,
	const VentureFieldDecl		*fields,
	guint				 n_fields
);

/**
 * venture_entity_get_field:
 * @self: a #VentureEntity
 * @name: the property name
 * @out_value: (out caller-allocates): an uninitialised #GValue to fill
 *
 * Reads a declaratively-installed field into @out_value.
 *
 * Returns: %TRUE if the field exists
 */
gboolean
venture_entity_get_field(
	VentureEntity	*self,
	const gchar	*name,
	GValue		*out_value
);

/**
 * venture_entity_set_field_from_string:
 * @self: a #VentureEntity
 * @name: the property name
 * @text: (nullable): the value, in the textual form the field's type accepts
 * @error: (out) (optional): return location for a #GError
 *
 * Sets a field from text, converting to the property's type. Used by the
 * form decoder, the CSV importer and the CLI, all of which have strings.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_entity_set_field_from_string(
	VentureEntity	 *self,
	const gchar	 *name,
	const gchar	 *text,
	GError		**error
);

/* --- Property and column introspection ----------------------------------- */

/**
 * venture_entity_class_set_field_kind:
 * @klass: a #VentureEntityClass
 * @property_name: the property
 * @kind: the kind it was declared with
 *
 * Records the kind a field was declared with.
 *
 * Most kinds can be recovered from the property's value type, but not all: a
 * long text field and a short string field are both %G_TYPE_STRING, so
 * recomputing turns every Notes field into a one-line box. Set automatically
 * by venture_entity_class_install_fields().
 */
void
venture_entity_class_set_field_kind(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	VentureFieldKind	 kind
);

/**
 * venture_entity_class_get_field_kind:
 * @klass: a #VentureEntityClass
 * @property_name: the property
 * @out_kind: (out) (optional): the declared kind
 *
 * Returns: %TRUE if the field was declared through a field table
 */
gboolean
venture_entity_class_get_field_kind(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	VentureFieldKind	*out_kind
);

/**
 * venture_entity_class_set_field_order:
 * @klass: a #VentureEntityClass
 * @property_name: the property
 * @order: its position in the type's field table
 *
 * Records where a field sits in the table it was declared in.
 *
 * GObject returns properties in an order of its own, so without this every
 * generated surface falls back to something close to alphabetical -- and a
 * list's first columns become whichever fields sort early rather than the
 * ones that identify a record. Set automatically by
 * venture_entity_class_install_fields().
 */
void
venture_entity_class_set_field_order(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	guint			 order
);

/**
 * venture_entity_class_get_field_order:
 * @klass: a #VentureEntityClass
 * @property_name: the property
 *
 * Returns: the declared position, or %G_MAXUINT if the field was not
 *   declared through a field table
 */
guint
venture_entity_class_get_field_order(
	VentureEntityClass	*klass,
	const gchar		*property_name
);

/**
 * venture_entity_class_set_column_flags:
 * @klass: a #VentureEntityClass
 * @property_name: the GObject property name
 * @flags: the persistence hints for this property
 *
 * Attaches persistence metadata to a property. Call this from a subclass's
 * class_init after installing the property. Without it a property is a plain
 * nullable column; with it, it can be indexed, unique, immutable after
 * insert, withheld from serialisation, included in search, or excluded from
 * the database entirely.
 */
void
venture_entity_class_set_column_flags(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	VentureColumnFlags	 flags
);

/**
 * venture_entity_class_get_column_flags:
 * @klass: a #VentureEntityClass
 * @property_name: the GObject property name
 *
 * Returns: the persistence hints for @property_name
 */
VentureColumnFlags
venture_entity_class_get_column_flags(
	VentureEntityClass	*klass,
	const gchar		*property_name
);

/**
 * venture_entity_class_set_reference:
 * @klass: a #VentureEntityClass
 * @property_name: the GObject property name holding a foreign key
 * @target_entity_name: the entity the key points at, e.g. "contact"
 *
 * Declares that a property is a foreign key. This is what lets the schema
 * builder emit a real constraint, the UI render a picker instead of a number
 * box, and the AI resolve "the sale for contact 12" without guessing.
 */
void
venture_entity_class_set_reference(
	VentureEntityClass	*klass,
	const gchar		*property_name,
	const gchar		*target_entity_name
);

/**
 * venture_entity_class_get_reference:
 * @klass: a #VentureEntityClass
 * @property_name: the GObject property name
 *
 * Returns: (transfer none) (nullable): the referenced entity name, or %NULL
 */
const gchar *
venture_entity_class_get_reference(
	VentureEntityClass	*klass,
	const gchar		*property_name
);

/**
 * venture_entity_class_list_persistent_properties:
 * @klass: a #VentureEntityClass
 * @n_properties: (out): return location for the count
 *
 * Lists every property that maps to a database column: readable, writable
 * and not flagged %VENTURE_COLUMN_FLAG_TRANSIENT.
 *
 * Returns: (transfer container) (array length=n_properties): the properties.
 *   Free with g_free(); the #GParamSpec elements are owned by the class.
 */
GParamSpec **
venture_entity_class_list_persistent_properties(
	VentureEntityClass	*klass,
	guint			*n_properties
);

/**
 * venture_entity_property_to_column:
 * @property_name: a GObject property name such as "unit-price"
 *
 * Converts a property name to its column and JSON member name by replacing
 * hyphens with underscores.
 *
 * Returns: (transfer full): the column name
 */
gchar *
venture_entity_property_to_column(const gchar *property_name);

/**
 * venture_entity_column_to_property:
 * @column_name: a column or JSON member name such as "unit_price"
 *
 * The inverse of venture_entity_property_to_column().
 *
 * Returns: (transfer full): the property name
 */
gchar *
venture_entity_column_to_property(const gchar *column_name);

/**
 * venture_entity_get_field_specs:
 * @self: a #VentureEntity
 *
 * Describes the record's fields, deriving a #VentureFieldSpec from each
 * persistent property. Used to render forms, build AI tool schemas and
 * document the type.
 *
 * Returns: (transfer full) (element-type VentureFieldSpec): the field specs
 */
GPtrArray *
venture_entity_get_field_specs(VentureEntity *self);

/* --- Custom attributes --------------------------------------------------- */

/**
 * venture_entity_set_attribute:
 * @self: a #VentureEntity
 * @key: the attribute name
 * @value: (nullable): the value; %NULL removes the attribute
 *
 * Sets a free-form attribute stored in the record's JSON extension column.
 *
 * Custom attributes exist so that a one-off piece of information -- the ISBN
 * of a book, an Etsy listing id, the ASIN a sale came from -- can be
 * attached to a record without a schema change. When an attribute proves it
 * deserves a real column, promoting it to a declared field is a migration,
 * and the values survive it.
 */
void
venture_entity_set_attribute(
	VentureEntity	*self,
	const gchar	*key,
	const gchar	*value
);

/**
 * venture_entity_get_attribute:
 * @self: a #VentureEntity
 * @key: the attribute name
 *
 * Returns: (transfer none) (nullable): the attribute value, or %NULL
 */
const gchar *
venture_entity_get_attribute(
	VentureEntity	*self,
	const gchar	*key
);

/**
 * venture_entity_list_attributes:
 * @self: a #VentureEntity
 *
 * Returns: (transfer container) (element-type utf8): the attribute names.
 *   Free with g_list_free().
 */
GList *
venture_entity_list_attributes(VentureEntity *self);

/* --- Copying and comparison ---------------------------------------------- */

/**
 * venture_entity_duplicate:
 * @self: a #VentureEntity
 *
 * Creates an unsaved copy of @self: every property is copied except the
 * identity spine, so the result inserts as a new row rather than updating
 * the original. This is what the "duplicate" action in the UI uses.
 *
 * Returns: (transfer full): a new unsaved entity of the same type
 */
VentureEntity *
venture_entity_duplicate(VentureEntity *self);

/**
 * venture_entity_copy_properties_from:
 * @self: the destination
 * @source: the source; must be of the same type
 * @skip_identity: whether to leave the destination's id, uuid, timestamps
 *   and version alone
 *
 * Copies property values between two entities of the same type.
 */
void
venture_entity_copy_properties_from(
	VentureEntity	*self,
	VentureEntity	*source,
	gboolean	 skip_identity
);

/**
 * venture_entity_diff:
 * @self: the current state
 * @other: the proposed state
 *
 * Produces a JSON object describing which fields differ, in the form
 * `{"field": {"from": ..., "to": ...}}`.
 *
 * This is what an AI write confirmation shows the operator before anything
 * is applied, and what the audit log records afterwards. Sensitive fields
 * are reported as changed without disclosing either value.
 *
 * Returns: (transfer full): the differences
 */
JsonNode *
venture_entity_diff(
	VentureEntity	*self,
	VentureEntity	*other
);

/**
 * venture_entity_equal:
 * @a: (nullable): the first entity
 * @b: (nullable): the second entity
 *
 * Compares two entities by type and persistent property values, ignoring
 * timestamps and version.
 *
 * Returns: %TRUE if they hold the same data
 */
gboolean
venture_entity_equal(
	VentureEntity	*a,
	VentureEntity	*b
);

G_END_DECLS

#endif /* VENTURE_ENTITY_H */
