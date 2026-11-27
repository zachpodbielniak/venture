/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-lql.c - One caller's LQL environment, and VENTURE's functions
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * venture_lql_environment_new() is the whole of what a query can name in
 * VENTURE (docs/lql.org):
 *
 *   - LQL's built-in commands and functions, which compute on rows and
 *     never touch a file, a program or the network;
 *   - one VentureLqlSource per record type the caller may read;
 *   - me(), my_teams(), today(), period() and organization().
 *
 * Nothing else. LQL's file, exec, SQL, git and Kubernetes sources are
 * opt-in providers a host has to add, and none is added here: each would
 * read around the one reader path and its access policy.
 *
 * Every function takes the caller from its user data rather than from the
 * executor, because an operand is evaluated while the query is planned
 * too (to decide what can be pushed to the database), when there is no
 * executor yet. The environment is built per caller, so that is the same
 * thing.
 */

#include "lql/venture-lql-private.h"

/* --- The caller ----------------------------------------------------------- */

static void
venture_lql_caller_clear(gpointer data)
{
	VentureLqlCaller *caller = data;

	g_clear_object(&caller->context);
	g_clear_object(&caller->database);
	g_clear_object(&caller->registry);
	g_clear_pointer(&caller->reader, venture_access_snapshot_free);
	g_clear_pointer(&caller->principal, venture_auth_principal_free);
	g_clear_pointer(&caller->username, g_free);
	g_clear_pointer(&caller->organizations, g_array_unref);
	g_clear_pointer(&caller->readable, g_hash_table_unref);
	g_clear_pointer(&caller->teams, lql_value_unref);
}

VentureLqlCaller *
venture_lql_caller_ref(VentureLqlCaller *caller)
{
	return g_rc_box_acquire(caller);
}

void
venture_lql_caller_unref(VentureLqlCaller *caller)
{
	if (NULL != caller)
		g_rc_box_release_full(caller, venture_lql_caller_clear);
}

gboolean
venture_lql_caller_can_read(
	VentureLqlCaller	*caller,
	GType			 entity_type
){
	return g_hash_table_contains(caller->readable, GSIZE_TO_POINTER(entity_type));
}

/*
 * The caller's username, read with internal authority: it is the caller's
 * own account, and a viewer may not be able to list users. NULL for a
 * principal with no account (a token minted without a user).
 */
static gchar *
venture_lql_caller_username(
	VentureDatabase			*database,
	const VentureAuthPrincipal	*principal
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) user = NULL;
	gchar *username = NULL;

	if (principal->user_id <= 0)
		return NULL;

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);
	user = venture_database_get(database, VENTURE_TYPE_USER,
	                            principal->user_id, NULL);

	if (NULL == user)
		return NULL;

	g_object_get(user, "username", &username, NULL);

	return username;
}

/* --- Functions ------------------------------------------------------------ */

/* The execution's fixed now() as a GDateTime in UTC. */
static GDateTime *
venture_lql_call_now(LqlFunctionCall *call)
{
	return g_date_time_new_from_unix_utc_usec(lql_function_call_get_now(call));
}

/* A GDateTime as an LQL Timestamp; NULL stays NULL. */
static LqlValue *
venture_lql_timestamp_value(GDateTime *when)
{
	if (NULL == when)
		return lql_value_new_null();

	return lql_value_new_timestamp(g_date_time_to_unix_usec(when));
}

/*
 * me() -> String: the caller's username, the value an assignee field
 * holds, so `assignee == me()` reads "assigned to me".
 */
static LqlValue *
venture_lql_fn_me(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	VentureLqlCaller *caller = user_data;

	(void)call;
	(void)error;

	return lql_value_new_string(caller->username);
}

/*
 * my_id() -> Int: the caller's user id, the value an owning-user reference
 * holds, so `owner_user_id == my_id()` reads "owned by me". NULL for a
 * caller with no account (sign-in off), which matches nothing.
 */
static LqlValue *
venture_lql_fn_my_id(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	VentureLqlCaller *caller = user_data;

	(void)call;
	(void)error;

	if (caller->principal->user_id <= 0)
		return lql_value_new_null();

	return lql_value_new_int(caller->principal->user_id);
}

/*
 * my_teams() -> Array: the ids of the teams the caller is an active member
 * of, for `team_id in my_teams()`. Read once per environment, with
 * internal authority (they are the caller's own memberships), and empty
 * when the teams module is off.
 */
static LqlValue *
venture_lql_fn_my_teams(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	VentureLqlCaller *caller = user_data;
	g_autoptr(GPtrArray) items = NULL;

	(void)call;

	if (NULL != caller->teams)
		return lql_value_ref(caller->teams);

	items = g_ptr_array_new_with_free_func((GDestroyNotify)lql_value_unref);

	if ((caller->principal->user_id > 0) &&
	    (G_TYPE_INVALID != venture_entity_registry_lookup(caller->registry, "team_membership")))
	{
		g_autoptr(VentureAccessScope) internal = NULL;
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		guint i;

		internal = venture_access_policy_enter(
			venture_database_get_access_policy(caller->database), NULL);
		query = venture_query_new(VENTURE_TYPE_TEAM_MEMBERSHIP);

		if (!venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ,
		                                  caller->principal->user_id, error) ||
		    !venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
		                                     "true", error))
			return NULL;

		venture_query_add_order(query, "team-id", VENTURE_SORT_ASCENDING, NULL);
		rows = venture_database_find(caller->database, query, error);

		if (NULL == rows)
			return NULL;

		for (i = 0; i < rows->len; i++)
		{
			gint64 team = 0;

			g_object_get(g_ptr_array_index(rows, i), "team-id", &team, NULL);

			if (team > 0)
				g_ptr_array_add(items, lql_value_new_int(team));
		}
	}

	caller->teams = lql_value_new_array(items);

	return lql_value_ref(caller->teams);
}

/*
 * The period @text names, read on the operator's calendar at the
 * execution's now(): the same vocabulary, timezone and fiscal year every
 * report uses (venture_context_parse_period()).
 */
static VentureDateRange *
venture_lql_period(
	VentureLqlCaller	 *caller,
	LqlFunctionCall		 *call,
	const gchar		 *text,
	GError			**error
){
	g_autoptr(GDateTime) now = venture_lql_call_now(call);
	g_autoptr(GError) local_error = NULL;
	VentureDateRange *range;

	range = venture_date_range_parse_at(text,
		venture_context_get_timezone(caller->context),
		venture_context_get_fiscal_year_start_month(caller->context),
		now, &local_error);

	if (NULL == range)
	{
		/* LQL adds the call's position; the message says what is known. */
		g_set_error(error, LQL_ERROR, LQL_ERROR_ARGUMENT,
		            "period(\"%s\"): %s (periods are the report period names: "
		            "today, this_week, this_month, last_month, this_quarter, "
		            "this_year, ytd, last_30_days, 2026-03, ...)",
		            text, local_error->message);
		return NULL;
	}

	return range;
}

/*
 * today() -> Timestamp: the start of the operator's current day, stored
 * the way VENTURE stores a date (midnight UTC of the local date), so
 * `due_at < today()` means "due before today" on the operator's calendar.
 */
static LqlValue *
venture_lql_fn_today(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureDateRange) range = NULL;

	range = venture_lql_period(user_data, call, "today", error);

	if (NULL == range)
		return NULL;

	return venture_lql_timestamp_value(venture_date_range_get_start(range));
}

/*
 * period(name: String) -> Record: {start, end, label} of a report period,
 * half-open like every VENTURE period: `created_at >= period("this_month")
 * .start and created_at < period("this_month").end`. "all" has NULL ends.
 */
static LqlValue *
venture_lql_fn_period(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(LqlRecord) record = NULL;
	const gchar *text;

	text = lql_value_get_string(lql_function_call_get_arg(call, 0));
	range = venture_lql_period(user_data, call, text, error);

	if (NULL == range)
		return NULL;

	record = lql_record_new();
	lql_record_take(record, "start",
		venture_lql_timestamp_value(venture_date_range_get_start(range)));
	lql_record_take(record, "end",
		venture_lql_timestamp_value(venture_date_range_get_end(range)));
	lql_record_take(record, "label",
		lql_value_new_string(venture_date_range_get_label(range)));

	return lql_value_new_record(record);
}

/*
 * organization() -> Int: the business selected in the browser, the one an
 * unqualified fetch is scoped to (with everything beneath it); NULL when
 * none is selected ("all businesses").
 */
static LqlValue *
venture_lql_fn_organization(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	VentureLqlCaller *caller = user_data;

	(void)call;
	(void)error;

	if (0 == caller->organizations->len)
		return lql_value_new_null();

	return lql_value_new_int(g_array_index(caller->organizations, gint64, 0));
}

/* One VENTURE function, registered with its documentation. */
static gboolean
venture_lql_register_function(
	LqlEnvironment		 *environment,
	VentureLqlCaller	 *caller,
	const gchar		 *name,
	LqlValueType		  return_type,
	LqlScalarFunc		  func,
	guint			  n_args,
	LqlFunctionFlags	  flags,
	const gchar		 *signature,
	const gchar		 *description,
	GError			**error
){
	static const LqlValueType string_arg[] = { LQL_VALUE_TYPE_STRING };
	g_autoptr(LqlFunction) function = NULL;

	function = lql_function_new_scalar(name, return_type, func,
		venture_lql_caller_ref(caller),
		(GDestroyNotify)venture_lql_caller_unref);
	lql_function_set_arguments(function, n_args, n_args,
		(n_args > 0) ? string_arg : NULL, n_args);
	lql_function_set_flags(function, flags);
	lql_function_set_doc(function, signature, description);
	lql_function_set_category(function, "venture");

	return lql_environment_register_function(environment, function, error);
}

gboolean
venture_lql_register_functions(
	LqlEnvironment		 *environment,
	VentureLqlCaller	 *caller,
	GError			**error
){
	/* today() and period() read now(), which is fixed per execution but
	 * not across them: volatile, like now() itself. */
	return venture_lql_register_function(environment, caller, "me",
		LQL_VALUE_TYPE_STRING, venture_lql_fn_me, 0, 0, "me() -> String",
		"Your username: what an assignee field holds.", error) &&
	       venture_lql_register_function(environment, caller, "my_id",
		LQL_VALUE_TYPE_INT, venture_lql_fn_my_id, 0, 0, "my_id() -> Int",
		"Your user id: what an owner_user_id field holds.", error) &&
	       venture_lql_register_function(environment, caller, "my_teams",
		LQL_VALUE_TYPE_ARRAY, venture_lql_fn_my_teams, 0, 0, "my_teams() -> Array",
		"The ids of the teams you are an active member of.", error) &&
	       venture_lql_register_function(environment, caller, "today",
		LQL_VALUE_TYPE_TIMESTAMP, venture_lql_fn_today, 0,
		LQL_FUNCTION_FLAG_VOLATILE, "today() -> Timestamp",
		"The start of today on the business calendar, as dates are stored.", error) &&
	       venture_lql_register_function(environment, caller, "period",
		LQL_VALUE_TYPE_RECORD, venture_lql_fn_period, 1,
		LQL_FUNCTION_FLAG_VOLATILE, "period(name: String) -> Record",
		"{start, end, label} of a report period (\"this_month\", \"last_quarter\", "
		"\"ytd\", \"2026-03\", ...); end is exclusive.", error) &&
	       venture_lql_register_function(environment, caller, "organization",
		LQL_VALUE_TYPE_INT, venture_lql_fn_organization, 0, 0, "organization() -> Int",
		"The id of the business selected in the sidebar; NULL for all.", error);
}

/* --- The environment ------------------------------------------------------ */

LqlEnvironment *
venture_lql_environment_new(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	const gint64			 *organizations,
	gsize				  n_organizations,
	VentureLqlTypeGate		  gate,
	gpointer			  gate_data,
	GError				**error
){
	g_autoptr(LqlEnvironment) environment = NULL;
	g_auto(GStrv) names = NULL;
	VentureLqlCaller *caller;
	VentureAccessPolicy *policy;
	gint64 timeout_ms = VENTURE_LQL_DEFAULT_TIMEOUT_MS;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != principal, NULL);
	g_return_val_if_fail(NULL != gate, NULL);
	g_return_val_if_fail((NULL != organizations) || (0 == n_organizations), NULL);

	if (!principal->authenticated)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED,
		                    "Sign in to run a query");
		return NULL;
	}

	/* --- who is asking, captured once ------------------------------- */
	caller = g_rc_box_new0(VentureLqlCaller);
	caller->context = g_object_ref(context);
	caller->database = g_object_ref(venture_context_get_database(context));
	caller->registry = g_object_ref(venture_context_get_entity_registry(context));
	caller->principal = venture_auth_principal_copy(principal);
	caller->organizations = g_array_new(FALSE, FALSE, sizeof(gint64));
	caller->readable = g_hash_table_new(g_direct_hash, g_direct_equal);
	caller->max_rows = VENTURE_LQL_DEFAULT_MAX_ROWS;

	if (n_organizations > 0)
		g_array_append_vals(caller->organizations, organizations, (guint)n_organizations);

	g_object_get(venture_context_get_config(context),
	             "query-max-rows", &caller->max_rows,
	             "query-timeout-ms", &timeout_ms, NULL);

	/*
	 * The reader every fetch is judged as: the principal, inside whatever
	 * scope this is called in (a request's own scope keeps its
	 * organization pin and narrowing; nested entries keep the boundary).
	 */
	policy = venture_database_get_access_policy(caller->database);
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(policy, principal);

		caller->reader = venture_access_policy_snapshot(policy);
	}

	caller->username = venture_lql_caller_username(caller->database, principal);

	/* --- which record types are sources ------------------------------ */
	/* list_names() hides the types of a module that is off; the gate is
	 * the list API's own role check for the rest. */
	names = venture_entity_registry_list_names(caller->registry);

	for (i = 0; NULL != names[i]; i++)
	{
		GType entity_type = venture_entity_registry_lookup(caller->registry, names[i]);

		if (G_TYPE_INVALID == entity_type)
			continue;

		if (gate(entity_type, gate_data, NULL))
			g_hash_table_add(caller->readable, GSIZE_TO_POINTER(entity_type));
	}

	/* --- the environment ---------------------------------------------- */
	environment = lql_environment_new();

	for (i = 0; NULL != names[i]; i++)
	{
		GType entity_type = venture_entity_registry_lookup(caller->registry, names[i]);
		g_autoptr(VentureLqlSource) source = NULL;

		if ((G_TYPE_INVALID == entity_type) || !venture_lql_caller_can_read(caller, entity_type))
			continue;

		source = venture_lql_source_new(caller, entity_type, names[i]);

		if (!lql_environment_register_source(environment, LQL_SOURCE(source), error))
		{
			venture_lql_caller_unref(caller);
			return NULL;
		}
	}

	if (!venture_lql_register_functions(environment, caller, error))
	{
		venture_lql_caller_unref(caller);
		return NULL;
	}

	/* A page load cannot scan everything, nor run for ever: the same row
	 * bound for every later stage, and the configured time limit. */
	lql_environment_set_limit(environment, LQL_LIMIT_MAX_ROWS, caller->max_rows);
	lql_environment_set_limit(environment, LQL_LIMIT_TIMEOUT_MS, timeout_ms);

	/*
	 * VENTURE #44, the byte budget: query.max_bytes, set here as
	 * LQL_LIMIT_MAX_BYTES once the libre-query-language pin carries it.
	 * Until then one query's memory (a nested concat) is bounded by
	 * nothing but the row bound and the time limit above.
	 */

	venture_lql_caller_unref(caller);

	return g_steal_pointer(&environment);
}
