/*
 * venture-deal-contact.c - Contact roles on deals
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The rules are save validators, so the deal page, the REST API, an
 * approved staged change and the assistant all meet the same refusal for
 * a second primary. The deal's own `contact-id` stays what it was: it
 * must be somebody at the deal's company, and a buying committee often
 * is not (a consultant, a parent company's buyer), so the primary is
 * read from here and the old field is only the fallback.
 */

#include "venture.h"

#define VENTURE_DEAL_CONTACT_STATE_KEY "venture-deal-contact-installed"

const gchar *
venture_deal_contact_role_label(VentureDealContactRole role)
{
	static const gchar *const labels[] = {
		"Decision maker", "Economic buyer", "Champion", "Influencer", "Technical evaluator", "User", "Other"
	};

	if ((guint)role >= G_N_ELEMENTS(labels))
		return "Other";

	return labels[role];
}

static gint64
deal_contact_number(VentureEntity *entity, const gchar *name)
{
	gint64 value = 0;

	g_object_get(entity, name, &value, NULL);

	return value;
}

static gboolean
deal_contact_is_primary(VentureEntity *entity)
{
	gboolean primary = FALSE;

	g_object_get(entity, "primary", &primary, NULL);

	return primary;
}

/* A live record of @type in @organization, or a refusal naming @what. */
static VentureEntity *
deal_contact_live(
	VentureDatabase	 *database,
	GType		  type,
	gint64		  id,
	gint64		  organization,
	const gchar	 *what,
	GError		**error
){
	g_autoptr(VentureEntity) row = NULL;

	if (id > 0)
		row = venture_database_get(database, type, id, NULL);

	if ((NULL == row) || venture_entity_is_deleted(row) ||
	    ((organization > 0) && (venture_entity_get_organization_id(row) != organization)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "Choose a %s in this business", what);
		return NULL;
	}

	return g_steal_pointer(&row);
}

/* ==========================================================================
 * The rule
 * ========================================================================== */

static gboolean
venture_deal_contact_validate(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureEntity) deal = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(GPtrArray) others = NULL;
	gint64 deal_id, contact_id, organization;
	guint i;

	(void)user_data;

	if (venture_entity_is_deleted(entity))
		return TRUE;

	deal_id = deal_contact_number(entity, "deal-id");
	contact_id = deal_contact_number(entity, "contact-id");

	/* A role moved to another deal would carry its primary flag past the
	 * check below on the deal it left; remove it and add the person. */
	if ((NULL != previous) && (deal_contact_number(previous, "deal-id") != deal_id))
	{
		venture_set_error_validation(error, "deal_id",
			"A role stays on its deal; remove it there and add the person to the other deal");
		return FALSE;
	}

	organization = venture_entity_get_organization_id(entity);
	deal = deal_contact_live(database, VENTURE_TYPE_DEAL, deal_id, organization, "deal", error);
	if (NULL == deal)
		return FALSE;
	if (organization <= 0)
		g_object_set(entity, "organization-id", venture_entity_get_organization_id(deal), NULL);
	organization = venture_entity_get_organization_id(deal);

	contact = deal_contact_live(database, VENTURE_TYPE_CONTACT, contact_id, organization, "contact", error);
	if (NULL == contact)
		return FALSE;

	others = venture_deal_contact_list(database, "deal-id", deal_id, error);
	if (NULL == others)
		return FALSE;

	for (i = 0; i < others->len; i++)
	{
		VentureEntity *other = g_ptr_array_index(others, i);
		g_autoptr(VentureEntity) holder = NULL;
		g_autofree gchar *name = NULL;

		if (venture_entity_get_id(other) == venture_entity_get_id(entity))
			continue;

		if (deal_contact_number(other, "contact-id") == contact_id)
		{
			name = venture_entity_get_display_name(contact);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s is already on this deal; change their role there", name);
			return FALSE;
		}

		if (!deal_contact_is_primary(entity) || !deal_contact_is_primary(other))
			continue;

		holder = venture_database_get(database, VENTURE_TYPE_CONTACT,
		                              deal_contact_number(other, "contact-id"), NULL);
		name = (NULL != holder) ? venture_entity_get_display_name(holder) : g_strdup("somebody else");
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "This deal already has a primary contact, %s; make them not primary first", name);
		return FALSE;
	}

	return TRUE;
}

/* A deleted contact is on no deal any more. The deal itself stays: it
 * is the business's, and the person was only one of its people. */
static void
venture_deal_contact_on_deleted(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gpointer	 user_data
){
	g_autoptr(GPtrArray) roles = NULL;
	guint i;

	(void)user_data;

	if (!VENTURE_IS_CONTACT(entity))
		return;

	roles = venture_deal_contact_list(database, "contact-id", venture_entity_get_id(entity), NULL);
	if (NULL == roles)
		return;

	for (i = 0; i < roles->len; i++)
	{
		g_autoptr(GError) error = NULL;

		if (!venture_database_delete(database, g_ptr_array_index(roles, i), NULL, &error))
			g_message("deal_contact: a deleted contact's deal role was kept: %s", error->message);
	}
}

void
venture_deal_contact_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; the rule and
	 * the handler are per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_DEAL_CONTACT_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_DEAL_CONTACT_STATE_KEY, GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_DEAL_CONTACT,
	                                    venture_deal_contact_validate, NULL, NULL);
	g_signal_connect(database, "entity-deleted",
	                 G_CALLBACK(venture_deal_contact_on_deleted), NULL);
}

/* ==========================================================================
 * Reading
 * ========================================================================== */

GPtrArray *
venture_deal_contact_list(
	VentureDatabase	 *database,
	const gchar	 *field,
	gint64		  id,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(NULL != field, NULL);

	/* A module switched off leaves no table to read: nobody is on a deal. */
	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "deal_contact"))
		return g_ptr_array_new_with_free_func(g_object_unref);

	query = venture_query_new(VENTURE_TYPE_DEAL_CONTACT);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "primary", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);

	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;

	return venture_database_find(database, query, error);
}

VentureEntity *
venture_deal_contact_primary(
	VentureDatabase	 *database,
	VentureEntity	 *deal,
	GError		**error
){
	g_autoptr(GPtrArray) roles = NULL;
	g_autoptr(VentureEntity) fallback = NULL;
	gint64 contact_id;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_DEAL(deal), NULL);

	roles = venture_deal_contact_list(database, "deal-id", venture_entity_get_id(deal), error);
	if (NULL == roles)
		return NULL;

	for (i = 0; i < roles->len; i++)
	{
		VentureEntity *role = g_ptr_array_index(roles, i);
		g_autoptr(VentureEntity) contact = NULL;

		if (!deal_contact_is_primary(role))
			continue;

		contact = venture_database_get(database, VENTURE_TYPE_CONTACT,
		                               deal_contact_number(role, "contact-id"), NULL);
		if ((NULL != contact) && !venture_entity_is_deleted(contact))
			return g_steal_pointer(&contact);
	}

	contact_id = deal_contact_number(deal, "contact-id");
	if (contact_id > 0)
		fallback = venture_database_get(database, VENTURE_TYPE_CONTACT, contact_id, NULL);
	if ((NULL != fallback) && venture_entity_is_deleted(fallback))
		g_clear_object(&fallback);

	return g_steal_pointer(&fallback);
}

gboolean
venture_deal_contact_is_on_deal(
	VentureDatabase	*database,
	VentureEntity	*deal,
	gint64		 contact_id
){
	g_autoptr(GPtrArray) roles = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_DEAL(deal), FALSE);

	if (contact_id <= 0)
		return FALSE;
	if (deal_contact_number(deal, "contact-id") == contact_id)
		return TRUE;

	roles = venture_deal_contact_list(database, "deal-id", venture_entity_get_id(deal), NULL);
	if (NULL == roles)
		return FALSE;

	for (i = 0; i < roles->len; i++)
		if (deal_contact_number(g_ptr_array_index(roles, i), "contact-id") == contact_id)
			return TRUE;

	return FALSE;
}

/* ==========================================================================
 * The report
 * ========================================================================== */

/*
 * Open deals -- neither won nor lost -- that nobody is recorded on as the
 * decision maker, the question a pipeline review asks first. It reads the
 * deals as they stand; the period is not used, because "who signs?" is
 * asked of today's pipeline, not last quarter's.
 */
static VentureReportResult *
deals_without_decision_maker(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) deals_query = NULL;
	g_autoptr(VentureQuery) roles_query = NULL;
	g_autoptr(GPtrArray) deals = NULL;
	g_autoptr(GPtrArray) roles = NULL;
	g_autoptr(GHashTable) decided = NULL;
	g_autoptr(GHashTable) people = NULL;
	g_autoptr(GHashTable) companies = NULL;
	GEnumClass *stages;
	gint64 organization;
	gint64 found = 0;
	guint i;

	organization = (NULL != options) ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	if (0 == organization)
		organization = venture_context_get_default_organization_id(context);

	deals_query = venture_query_new(VENTURE_TYPE_DEAL);
	venture_query_set_organization(deals_query, organization);
	venture_query_set_limit(deals_query, 0);
	venture_query_add_order(deals_query, "name", VENTURE_SORT_ASCENDING, NULL);
	deals = venture_database_find(database, deals_query, error);
	if (NULL == deals)
		return NULL;

	roles_query = venture_query_new(VENTURE_TYPE_DEAL_CONTACT);
	venture_query_set_organization(roles_query, organization);
	venture_query_set_limit(roles_query, 0);
	roles = venture_database_find(database, roles_query, error);
	if (NULL == roles)
		return NULL;

	decided = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	people = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (i = 0; i < roles->len; i++)
	{
		VentureEntity *role = g_ptr_array_index(roles, i);
		VentureDealContactRole kind = VENTURE_DEAL_CONTACT_ROLE_OTHER;
		gint64 deal_id = deal_contact_number(role, "deal-id");
		gint64 *key = g_new(gint64, 1);

		*key = deal_id;
		g_hash_table_insert(people, key, GINT_TO_POINTER(GPOINTER_TO_INT(g_hash_table_lookup(people, &deal_id)) + 1));
		g_object_get(role, "role", &kind, NULL);
		if (VENTURE_DEAL_CONTACT_ROLE_DECISION_MAKER == kind)
		{
			key = g_new(gint64, 1);
			*key = deal_id;
			g_hash_table_add(decided, key);
		}
	}

	result = venture_report_result_new("Open deals with no decision maker", period);
	venture_report_result_add_column(result, "deal_id", "Deal id", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "deal", "Deal", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "company", "Company", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "stage", "Stage", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "owner", "Owner", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "value", "Value", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "people", "People on the deal", VENTURE_REPORT_COLUMN_NUMBER);

	companies = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	stages = g_type_class_ref(VENTURE_TYPE_DEAL_STAGE);
	for (i = 0; i < deals->len; i++)
	{
		VentureEntity *deal = g_ptr_array_index(deals, i);
		g_autoptr(VentureMoney) value = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *owner = NULL;
		const gchar *company_name = "";
		VentureDealStage stage = VENTURE_DEAL_STAGE_LEAD;
		GEnumValue *stage_value;
		gint64 deal_id = venture_entity_get_id(deal);
		gint64 company_id;

		g_object_get(deal, "stage", &stage, "name", &name, "owner", &owner, "value", &value, NULL);
		if ((VENTURE_DEAL_STAGE_WON == stage) || (VENTURE_DEAL_STAGE_LOST == stage))
			continue;
		if (g_hash_table_contains(decided, &deal_id))
			continue;

		company_id = deal_contact_number(deal, "company-id");
		if (company_id > 0)
		{
			if (!g_hash_table_contains(companies, &company_id))
			{
				g_autoptr(VentureEntity) company = venture_database_get(database, VENTURE_TYPE_COMPANY, company_id, NULL);
				gint64 *key = g_new(gint64, 1);

				*key = company_id;
				g_hash_table_insert(companies, key,
					(NULL != company) ? venture_entity_get_display_name(company) : g_strdup(""));
			}
			company_name = g_hash_table_lookup(companies, &company_id);
		}

		stage_value = g_enum_get_value(stages, stage);
		venture_report_result_begin_row(result);
		venture_report_result_set_number(result, "deal_id", (gdouble)deal_id);
		venture_report_result_set_text(result, "deal", NULL != name ? name : "");
		venture_report_result_set_text(result, "company", company_name);
		venture_report_result_set_text(result, "stage", NULL != stage_value ? stage_value->value_nick : "");
		venture_report_result_set_text(result, "owner", NULL != owner ? owner : "");
		if (NULL != value)
			venture_report_result_set_money(result, "value", value);
		venture_report_result_set_number(result, "people",
			GPOINTER_TO_INT(g_hash_table_lookup(people, &deal_id)));
		found++;
	}
	g_type_class_unref(stages);

	venture_report_result_add_metric(result, venture_metric_new_number("deals_without_decision_maker",
		"Open deals with no decision maker", (gdouble)found));

	return g_steal_pointer(&result);
}

void
venture_deal_contact_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"deals_without_decision_maker", "Open deals with no decision maker",
		"Open deals nobody is recorded on as the decision maker", deals_without_decision_maker)));
}
