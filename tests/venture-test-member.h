/*
 * venture-test-member.h - Make a name somebody who may be given work
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of VENTURE.
 *
 * Header-only, for the same reason as venture-test-util.h: rules.mk
 * builds one tests/test-*.c into one binary.
 *
 * An assignee is checked at the save (venture_assignee_check()): it must
 * be an active user and, with organization access on, an active member
 * of the record's organization. A test that writes "assignee=dave" has
 * to make dave first; this does that, and nothing else.
 */

#pragma once

#include <venture.h>

/*
 * venture_test_member:
 * @database: the database
 * @organization_id: the organization @username will be given work in;
 *   0 for none, which makes only the user
 * @username: the name a test is about to assign
 *
 * Makes @username somebody venture_assignee_is_member() accepts for
 * @organization_id: an active editor if there is no user of that name,
 * and an active editor membership of @organization_id if memberships are
 * on and there is none. A user who already exists keeps its role.
 *
 * Saved through the internal scope: setting up who belongs where is the
 * test's premise, not something under test.
 *
 * Returns: the user's id
 */
static inline gint64
venture_test_member(
	VentureDatabase	*database,
	gint64		 organization_id,
	const gchar	*username
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) user = NULL;
	gint64 user_id;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);

	/* The user, made if it is not there yet. */
	query = venture_query_new(VENTURE_TYPE_USER);
	g_assert_true(venture_query_add_filter_string(query, "username",
		VENTURE_FILTER_OP_EQ, username, NULL));
	user = venture_database_find_one(database, query, NULL);

	if (NULL == user)
	{
		user = VENTURE_ENTITY(venture_user_new());
		g_object_set(user, "username", username,
		             "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
		g_assert_true(venture_database_save(database, user, NULL, NULL));
	}

	user_id = venture_entity_get_id(user);

	/* The membership, when memberships are read at all. */
	if ((organization_id > 0) &&
	    venture_entity_registry_is_type_enabled(
	        venture_entity_registry_get_default(), "organization_membership") &&
	    !venture_assignee_is_member(database, organization_id, username))
	{
		g_autoptr(VentureEntity) membership = NULL;

		membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
			"user-id", user_id, "organization-id", organization_id,
			"role", VENTURE_ORGANIZATION_ROLE_EDITOR, "active", TRUE, NULL);
		g_assert_true(venture_database_save(database, membership, NULL,
		                                    NULL));
	}

	if (organization_id > 0)
		g_assert_true(venture_assignee_is_member(database, organization_id,
		                                         username));

	return user_id;
}
