/*
 * venture-entity-macros.h - Boilerplate for declarative record types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A record type in VENTURE is a field table plus a name. These macros supply
 * the GObject scaffolding around that, so defining one looks like:
 *
 * |[<!-- language="C" -->
 * // in the header
 * VENTURE_DECLARE_ENTITY(VentureWidget, venture_widget, WIDGET)
 *
 * // in the source
 * static const VentureFieldDecl venture_widget_fields[] = {
 *     { "name",  "Name",  "What it is called", VENTURE_FIELD_KIND_STRING,
 *       NULL, NULL, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_SEARCHABLE },
 *     { "price", "Price", "What it sells for",  VENTURE_FIELD_KIND_MONEY,
 *       NULL, NULL, VENTURE_COLUMN_FLAG_NONE },
 * };
 *
 * VENTURE_DEFINE_ENTITY(VentureWidget, venture_widget, venture_widget_fields)
 * ]|
 *
 * That is the whole type. It gets a table, REST CRUD, JSON and YAML
 * serialisation, a web form, AI tools, audit diffs and CLI subcommands from
 * the base class, with no further code.
 *
 * These macros are public because plugin authors want exactly the same
 * shortcut a built-in type gets.
 */

#ifndef VENTURE_ENTITY_MACROS_H
#define VENTURE_ENTITY_MACROS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_DECLARE_ENTITY:
 * @TypeName: the camel-case type name, e.g. `VentureWidget`
 * @type_name: the lowercase function prefix, e.g. `venture_widget`
 * @TYPE_NAME: the uppercase name fragment, e.g. `WIDGET`
 *
 * Declares a final record type deriving from #VentureEntity, along with its
 * constructor. Use in a header -- or, for a plugin that is one source file
 * with no header of its own, at the top of that file: the constructor
 * declaration is what keeps such a plugin building warning-free under
 * -Wmissing-declarations.
 */
#define VENTURE_DECLARE_ENTITY(TypeName, type_name, TYPE_NAME)                \
	G_DECLARE_FINAL_TYPE(TypeName, type_name, VENTURE, TYPE_NAME,         \
	                     VentureEntity)                                    \
                                                                              \
	TypeName *type_name##_new(void);

/**
 * VENTURE_DEFINE_ENTITY:
 * @TypeName: the camel-case type name
 * @type_name: the lowercase function prefix
 * @fields: a static #VentureFieldDecl array
 *
 * Defines the type declared by %VENTURE_DECLARE_ENTITY, installing @fields.
 * Use in a source file.
 *
 * The instance struct holds nothing but the parent: field values live in the
 * base class's store, which is what removes the need for a get_property, a
 * set_property, a property enumeration and a finalize.
 *
 * The constructor is declared by %VENTURE_DECLARE_ENTITY, which every type
 * using this macro has already invoked.
 */
#define VENTURE_DEFINE_ENTITY(TypeName, type_name, fields)                    \
	struct _##TypeName                                                    \
	{                                                                     \
		VentureEntity parent_instance;                                \
	};                                                                    \
                                                                              \
	G_DEFINE_FINAL_TYPE(TypeName, type_name, VENTURE_TYPE_ENTITY)         \
                                                                              \
	static void                                                           \
	type_name##_init(TypeName *self)                                      \
	{                                                                     \
		(void)self;                                                   \
	}                                                                     \
                                                                              \
	static void                                                           \
	type_name##_class_init(TypeName##Class *klass)                        \
	{                                                                     \
		venture_entity_class_install_fields(                          \
			VENTURE_ENTITY_CLASS(klass), fields,                  \
			G_N_ELEMENTS(fields));                                \
	}                                                                     \
                                                                              \
	TypeName *                                                            \
	type_name##_new(void)                                                 \
	{                                                                     \
		return g_object_new(type_name##_get_type(), NULL);            \
	}

/**
 * VENTURE_DEFINE_ENTITY_WITH_CODE:
 * @TypeName: the camel-case type name
 * @type_name: the lowercase function prefix
 * @fields: a static #VentureFieldDecl array
 * @code: extra statements to run at the end of class_init
 *
 * As %VENTURE_DEFINE_ENTITY, but lets a type override a virtual method or
 * add a constraint after its fields are installed. Inside @code, `klass` is
 * the type's own class pointer.
 */
#define VENTURE_DEFINE_ENTITY_WITH_CODE(TypeName, type_name, fields, code)    \
	struct _##TypeName                                                    \
	{                                                                     \
		VentureEntity parent_instance;                                \
	};                                                                    \
                                                                              \
	G_DEFINE_FINAL_TYPE(TypeName, type_name, VENTURE_TYPE_ENTITY)         \
                                                                              \
	static void                                                           \
	type_name##_init(TypeName *self)                                      \
	{                                                                     \
		(void)self;                                                   \
	}                                                                     \
                                                                              \
	static void                                                           \
	type_name##_class_init(TypeName##Class *klass)                        \
	{                                                                     \
		venture_entity_class_install_fields(                          \
			VENTURE_ENTITY_CLASS(klass), fields,                  \
			G_N_ELEMENTS(fields));                                \
		{ code }                                                      \
	}                                                                     \
                                                                              \
	TypeName *                                                            \
	type_name##_new(void)                                                 \
	{                                                                     \
		return g_object_new(type_name##_get_type(), NULL);            \
	}

/*
 * Shorthands for the field table itself. Written as macros rather than
 * spelled out because a table of twenty fields is far easier to read -- and
 * to check against the schema -- when each row fits on one line.
 */

/** VENTURE_FIELD: a field with no reference and no enum backing. */
#define VENTURE_FIELD(name, label, help, kind, flags)                         \
	{ name, label, help, kind, NULL, NULL, flags }

/** VENTURE_FIELD_ENUM: a field backed by a registered #GEnum. */
#define VENTURE_FIELD_ENUM(name, label, help, enum_type_func, flags)          \
	{ name, label, help, VENTURE_FIELD_KIND_ENUM, enum_type_func, NULL,   \
	  flags }

/** VENTURE_FIELD_REF: a foreign key to another entity type. */
#define VENTURE_FIELD_REF(name, label, help, target, flags)                   \
	{ name, label, help, VENTURE_FIELD_KIND_REFERENCE, NULL, target,      \
	  (flags) | VENTURE_COLUMN_FLAG_INDEXED }

/** VENTURE_FIELD_TEXT: a long free-text field, searchable by default. */
#define VENTURE_FIELD_TEXT(name, label, help)                                 \
	{ name, label, help, VENTURE_FIELD_KIND_TEXT, NULL, NULL,             \
	  VENTURE_COLUMN_FLAG_SEARCHABLE }

/** VENTURE_FIELD_MONEY: a monetary amount. */
#define VENTURE_FIELD_MONEY(name, label, help)                                \
	{ name, label, help, VENTURE_FIELD_KIND_MONEY, NULL, NULL,            \
	  VENTURE_COLUMN_FLAG_NONE }

/** VENTURE_FIELD_NAME: the primary human label -- required and searchable. */
#define VENTURE_FIELD_NAME(name, label, help)                                 \
	{ name, label, help, VENTURE_FIELD_KIND_STRING, NULL, NULL,           \
	  VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_SEARCHABLE |     \
	  VENTURE_COLUMN_FLAG_INDEXED }

G_END_DECLS

#endif /* VENTURE_ENTITY_MACROS_H */
