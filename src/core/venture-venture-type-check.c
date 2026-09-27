/*
 * venture-venture-type-check.c - Declarative venture types, held at the save
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture_venture_type_validate_venture() existed from the start and had no
 * caller, so every shipped YAML type declared rules that nothing enforced.
 * It lives in the model (which venturectl links); this file is the server
 * half that wires it into the database.
 */

#include "venture.h"

#include <string.h>

/* The registry the validator consults, hung off the database. */
#define VENTURE_VENTURE_TYPE_CHECK_KEY "venture-venture-type-registry"

static gboolean
venture_venture_type_check_validate(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureVentureTypeRegistry *registry;
	VentureVentureType *type;
	g_autofree gchar *name = NULL;
	g_autofree gchar *previous_name = NULL;
	gboolean type_written;

	registry = g_object_get_data(G_OBJECT(database),
	                             VENTURE_VENTURE_TYPE_CHECK_KEY);

	if (NULL == registry)
		return TRUE;

	g_object_get(entity, "venture-type", &name, NULL);

	if (NULL != previous)
		g_object_get(previous, "venture-type", &previous_name, NULL);

	/* An untyped venture declares nothing to be held to. */
	if (venture_string_is_empty(name))
		return TRUE;

	type_written = (NULL == previous) ||
	               (0 != g_strcmp0(name, previous_name));
	type = venture_venture_type_registry_lookup(registry, name);

	if (NULL == type)
	{
		g_autoptr(GPtrArray) known = NULL;
		g_autoptr(GString) names = NULL;
		guint i;

		/*
		 * Kept, not written: a type file removed from the directory
		 * must not make every venture of that kind uneditable. And an
		 * empty registry -- plugins switched off, so nothing loaded --
		 * is not evidence that a type is wrong.
		 */
		if (!type_written)
			return TRUE;

		known = venture_venture_type_registry_list(registry);

		if (0 == known->len)
			return TRUE;

		names = g_string_new(NULL);

		for (i = 0; i < known->len; i++)
		{
			if (i > 0)
				g_string_append(names, ", ");

			g_string_append(names, venture_venture_type_get_name(
				g_ptr_array_index(known, i)));
		}

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "venture_type: \"%s\" is not a registered venture "
		            "type; registered: %s", name, names->str);
		return FALSE;
	}

	/* A new type is a new set of rules, all of which apply at once. */
	return venture_venture_type_validate_changes(type, entity,
		type_written ? NULL : previous, error);
}

void
venture_venture_type_check_install(VentureContext *context)
{
	VentureDatabase *database;
	VentureVentureTypeRegistry *registry;
	gboolean installed;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);
	registry = venture_context_get_venture_types(context);

	installed = (NULL != g_object_get_data(G_OBJECT(database),
	                                       VENTURE_VENTURE_TYPE_CHECK_KEY));

	/* A reference, so the registry outlives a context the tests drop
	 * before the database; the registry holds nothing back. */
	g_object_set_data_full(G_OBJECT(database), VENTURE_VENTURE_TYPE_CHECK_KEY,
	                       g_object_ref(registry), g_object_unref);

	if (installed)
		return;

	venture_database_add_save_validator(database, VENTURE_TYPE_VENTURE,
	                                    venture_venture_type_check_validate,
	                                    NULL, NULL);
}
