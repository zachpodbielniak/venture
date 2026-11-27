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
	g_clear_pointer(&caller->timezone, g_time_zone_unref);
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
 * The ids of the teams the caller is an active member of, read once while
 * the environment is built (on the database's thread), with internal
 * authority: they are the caller's own memberships. Empty when the teams
 * module is off or the principal has no account.
 */
static LqlValue *
venture_lql_caller_read_teams(
	VentureLqlCaller	 *caller,
	GError			**error
){
	g_autoptr(GPtrArray) items = NULL;

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

	return lql_value_new_array(items);
}

/*
 * my_teams() -> Array: the ids of the teams the caller is an active member
 * of, for `team_id in my_teams()` (venture_lql_caller_read_teams()). Pure:
 * it may be evaluated on a query worker.
 */
static LqlValue *
venture_lql_fn_my_teams(
	LqlFunctionCall	 *call,
	gpointer	  user_data,
	GError		**error
){
	VentureLqlCaller *caller = user_data;

	(void)call;
	(void)error;

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

	/* The calendar captured with the environment, not the context's:
	 * this may run on a query worker. */
	range = venture_date_range_parse_at(text, caller->timezone,
		caller->fiscal_year_start_month, now, &local_error);

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

/* --- Money ---------------------------------------------------------------- */

/*
 * An LQL money record -- {amount, currency, exponent, formatted}, as every
 * money field reads -- back as a #VentureMoney, through the API's own JSON
 * decoder so a record that is not money is refused the way the API
 * refuses it.
 */
static VentureMoney *
venture_lql_money_from_value(
	LqlValue	 *value,
	GError		**error
){
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) local_error = NULL;
	VentureMoney *money;

	if (LQL_VALUE_TYPE_RECORD != lql_value_get_value_type(value))
	{
		g_set_error(error, LQL_ERROR, LQL_ERROR_TYPE,
		            "sum_money() adds money fields ({amount, currency, exponent}), "
		            "not %s; for a number use sum()",
		            lql_value_type_to_string(lql_value_get_value_type(value)));
		return NULL;
	}

	text = lql_value_to_json(value);
	node = venture_json_parse(text, NULL);
	money = (NULL != node) && JSON_NODE_HOLDS_OBJECT(node) &&
	        json_object_has_member(json_node_get_object(node), "currency")
		? venture_money_from_json(node, NULL, &local_error) : NULL;

	if (NULL == money)
	{
		g_set_error(error, LQL_ERROR, LQL_ERROR_TYPE,
		            "sum_money() adds money fields ({amount, currency, exponent}); "
		            "this record is not money%s%s",
		            (NULL != local_error) ? ": " : "",
		            (NULL != local_error) ? local_error->message : "");
		return NULL;
	}

	return money;
}

/* A #VentureMoney as the LQL record a money field reads as. */
static LqlValue *
venture_lql_money_to_value(
	const VentureMoney	 *money,
	GError			**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;

	node = venture_money_to_json(money);
	text = venture_json_to_string(node, FALSE);

	return lql_json_parse(text, -1, error);
}

/*
 * sum_money(value: Record) -> Array: the total of a money field, one per
 * currency, never added across two. It is the reports' accumulator
 * (venture_money_totals_add(), what the sum widget and the aggregate
 * report keep apart with): dollars and gold are two totals, and the
 * array holds both, sorted by currency code. NULL is skipped; no money at
 * all is an empty array, because nothing has no currency to be zero in.
 */
static LqlValue *
venture_lql_fn_sum_money(
	LqlAggregateCall	 *call,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(GPtrArray) totals = NULL;
	g_autoptr(GPtrArray) items = NULL;
	guint i;

	(void)user_data;

	totals = venture_money_totals_new();

	for (i = 0; i < lql_aggregate_call_get_n_rows(call); i++)
	{
		LqlValue *value = lql_aggregate_call_get_arg(call, i, 0);
		g_autoptr(VentureMoney) money = NULL;
		g_autoptr(GError) local_error = NULL;

		if ((NULL == value) || lql_value_is_null(value))
			continue;

		money = venture_lql_money_from_value(value, error);

		if (NULL == money)
			return NULL;

		if (!venture_money_totals_add(totals, money, &local_error))
		{
			g_set_error(error, LQL_ERROR, LQL_ERROR_EVALUATION,
			            "sum_money(): %s", local_error->message);
			return NULL;
		}
	}

	venture_money_totals_sort(totals, NULL);
	items = g_ptr_array_new_with_free_func((GDestroyNotify)lql_value_unref);

	for (i = 0; i < totals->len; i++)
	{
		LqlValue *item;

		item = venture_lql_money_to_value(g_ptr_array_index(totals, i), error);

		if (NULL == item)
			return NULL;

		g_ptr_array_add(items, item);
	}

	return lql_value_new_array(items);
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

/*
 * sum_money(), an aggregate: one total per currency (see
 * venture_lql_fn_sum_money()).
 */
static gboolean
venture_lql_register_sum_money(
	LqlEnvironment	 *environment,
	GError		**error
){
	static const LqlValueType record_arg[] = { LQL_VALUE_TYPE_ANY };
	g_autoptr(LqlFunction) function = NULL;

	function = lql_function_new_aggregate("sum_money", LQL_VALUE_TYPE_ARRAY,
		venture_lql_fn_sum_money, NULL, NULL);
	lql_function_set_arguments(function, 1, 1, record_arg, 1);
	lql_function_set_doc(function, "sum_money(value: Record) -> Array",
		"The total of a money field, one {amount, currency, exponent, formatted} "
		"per currency, never added across two.");
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
		"The id of the business selected in the sidebar; NULL for all.", error) &&
	       venture_lql_register_sum_money(environment, error);
}

/* --- The environment ------------------------------------------------------ */

/* Where an environment keeps its refusals: type name -> GError. */
static GQuark
venture_lql_refusals_quark(void)
{
	return g_quark_from_static_string("venture-lql-refusals");
}

/* Declared in lql/venture-lql.h. */
const GError *
venture_lql_environment_get_refusal(
	LqlEnvironment	*environment,
	const gchar	*name
){
	GHashTable *refusals;

	g_return_val_if_fail(LQL_IS_ENVIRONMENT(environment), NULL);

	if (NULL == name)
		return NULL;

	refusals = g_object_get_qdata(G_OBJECT(environment), venture_lql_refusals_quark());

	return (NULL != refusals) ? g_hash_table_lookup(refusals, name) : NULL;
}

/* The host's extension (venture_lql_set_environment_hook()); main thread. */
static VentureLqlEnvironmentHook venture_lql_environment_hook;
static gpointer venture_lql_environment_hook_data;

/* Declared in lql/venture-lql.h. */
void
venture_lql_set_environment_hook(
	VentureLqlEnvironmentHook	hook,
	gpointer			user_data
){
	venture_lql_environment_hook = hook;
	venture_lql_environment_hook_data = user_data;
}

/* Declared in lql/venture-lql.h. */
void
venture_lql_environment_prepare_sources(LqlEnvironment *environment)
{
	g_auto(GStrv) names = NULL;
	guint i;

	g_return_if_fail(LQL_IS_ENVIRONMENT(environment));

	names = lql_environment_list_sources(environment);

	for (i = 0; (NULL != names) && (NULL != names[i]); i++)
	{
		LqlSource *source = lql_environment_lookup_source(environment, names[i]);

		if (VENTURE_IS_LQL_SOURCE(source))
			venture_lql_source_prepare(VENTURE_LQL_SOURCE(source));
	}
}

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
	g_autoptr(GHashTable) refusals = NULL;
	g_auto(GStrv) names = NULL;
	VentureLqlCaller *caller;
	VentureAccessPolicy *policy;
	gint64 timeout_ms = VENTURE_LQL_DEFAULT_TIMEOUT_MS;
	gint64 max_bytes = VENTURE_LQL_DEFAULT_MAX_BYTES;
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
	caller->home_thread = g_thread_self();

	if (n_organizations > 0)
		g_array_append_vals(caller->organizations, organizations, (guint)n_organizations);

	g_object_get(venture_context_get_config(context),
	             "query-max-rows", &caller->max_rows,
	             "query-timeout-ms", &timeout_ms,
	             "query-max-bytes", &max_bytes, NULL);

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

	/* What the functions answer, captured here on the database's thread
	 * so they are pure where the query is evaluated: on a worker. */
	{
		GTimeZone *timezone = venture_context_get_timezone(context);

		caller->timezone = (NULL != timezone) ? g_time_zone_ref(timezone) : NULL;
	}
	caller->fiscal_year_start_month = venture_context_get_fiscal_year_start_month(context);
	caller->teams = venture_lql_caller_read_teams(caller, error);

	if (NULL == caller->teams)
	{
		venture_lql_caller_unref(caller);
		return NULL;
	}

	/* --- which record types are sources ------------------------------ */
	/* list_names() hides the types of a module that is off; the gate is
	 * the list API's own role check for the rest. */
	names = venture_entity_registry_list_names(caller->registry);
	refusals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                 (GDestroyNotify)g_error_free);

	for (i = 0; NULL != names[i]; i++)
	{
		GType entity_type = venture_entity_registry_lookup(caller->registry, names[i]);

		if (G_TYPE_INVALID == entity_type)
			continue;

		{
			GError *refusal = NULL;

			if (gate(entity_type, gate_data, &refusal))
				g_hash_table_add(caller->readable, GSIZE_TO_POINTER(entity_type));
			else
				g_hash_table_insert(refusals, g_strdup(names[i]),
					(NULL != refusal) ? refusal
					                  : g_error_new(VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
					                                "You may not read %s records", names[i]));
		}
	}

	/* --- the environment ---------------------------------------------- */
	environment = lql_environment_new();

	/* What the gate said, kept with the environment, so a query that
	 * names a refused type is answered with the list API's own refusal
	 * (venture_lql_environment_get_refusal()). */
	g_object_set_qdata_full(G_OBJECT(environment), venture_lql_refusals_quark(),
	                        g_steal_pointer(&refusals), (GDestroyNotify)g_hash_table_unref);

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

	/* A page load cannot scan everything, nor run for ever, nor take
	 * the process's memory: the same row bound for every later stage,
	 * the configured time limit, and the memory budget (VENTURE #44) --
	 * the string and array bytes one execution may create, counted while
	 * LQL evaluates. The configuration never lets the budget be 0, which
	 * to LQL would mean "none". */
	lql_environment_set_limit(environment, LQL_LIMIT_MAX_ROWS, caller->max_rows);
	lql_environment_set_limit(environment, LQL_LIMIT_TIMEOUT_MS, timeout_ms);
	lql_environment_set_limit(environment, LQL_LIMIT_MAX_BYTES, max_bytes);

	/* What the host adds, last, so it can see everything above. */
	if ((NULL != venture_lql_environment_hook) &&
	    !venture_lql_environment_hook(environment, venture_lql_environment_hook_data, error))
	{
		venture_lql_caller_unref(caller);
		return NULL;
	}

	venture_lql_caller_unref(caller);

	return g_steal_pointer(&environment);
}
