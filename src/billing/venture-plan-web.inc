/*
 * venture-plan-web.inc - A plan and what it costs, on one page
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c, before the subscription pages, which
 * offer a plan through venture_plan_web_append_price_options().
 *
 * A plan is what a customer signs up to; its prices are what it costs for
 * each period it is sold by -- the same plan by the month and by the
 * year, say. Both were already records. What was missing was a page that
 * treats them as one thing: a plan made with its prices, a plan's page
 * that lists and adds them, and a subscription page that offers every
 * priced plan by name and says which plans cannot be sold yet.
 */

/* How many price rows one plan sheet may carry. */
#define VENTURE_PLAN_MAX_PRICES	24

/* The periods a price is sold by, as a person picks them. */
static const struct
{
	const gchar *nick;
	const gchar *label;
} venture_plan_periods[] = {
	{ "month", "Every month" },
	{ "quarter", "Every 3 months" },
	{ "half_year", "Every 6 months" },
	{ "year", "Every year" },
};

static void
venture_plan_append_period_select(GString *html, const gchar *name, const gchar *selected)
{
	guint i;

	g_string_append_printf(html, "<select name=\"%s\" aria-label=\"How often\" data-no-picker>", name);
	for (i = 0; i < G_N_ELEMENTS(venture_plan_periods); i++)
		g_string_append_printf(html, "<option value=\"%s\"%s>%s</option>", venture_plan_periods[i].nick,
			g_strcmp0(selected, venture_plan_periods[i].nick) == 0 ? " selected" : "",
			venture_plan_periods[i].label);
	g_string_append(html, "</select>");
}

/*
 * Whose product the plan is. A plan belongs to one venture and is sold
 * only to that venture's customers; "Shared" is for a plan every venture
 * sells. With one venture it is chosen already.
 */
static void
venture_plan_append_venture_select(VentureWebServer *self, GString *html, gint64 organization)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VENTURE);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	if (organization > 0)
		venture_query_set_organization(query, organization);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);

	g_string_append(html, "<label class=\"doc-field\"><span class=\"field-label\">Venture"
		"<span class=\"required\">*</span></span><select name=\"venture-id\" required>");
	if (rows == NULL || rows->len != 1)
		g_string_append(html, "<option value=\"\">Whose plan is it?</option>");
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = venture_entity_get_display_name(row);

		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\"%s>", venture_entity_get_id(row),
			rows->len == 1 ? " selected" : "");
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
	g_string_append(html, "<option value=\"0\">Shared by every venture</option></select>"
		"<span class=\"field-help\">Only this venture's customers can be put on it.</span></label>");
}

/* One price row of the plan sheet. */
static void
venture_plan_append_price_row(GString *html, const gchar *index, const gchar *period, const gchar *currency)
{
	g_autofree gchar *name = g_strdup_printf("price-%s-interval", index);

	g_string_append(html, "<tr class=\"line-row\" data-line><td>");
	venture_plan_append_period_select(html, name, period);
	g_string_append_printf(html, "</td>"
		"<td class=\"num\"><span class=\"money-input\"><input type=\"text\" name=\"price-%s-amount\" "
		"placeholder=\"0.00\" inputmode=\"decimal\" aria-label=\"Price\"><span class=\"unit\">%s</span></span></td>"
		"<td><label class=\"checkbox\"><input type=\"checkbox\" name=\"price-%s-per-seat\" value=\"true\"> Per seat</label></td>"
		"<td class=\"num\"><input type=\"number\" name=\"price-%s-trial-days\" min=\"0\" step=\"1\" placeholder=\"0\" "
		"aria-label=\"Free trial days\"></td>"
		VENTURE_WEB_LINE_DELETE "</tr>", index, currency, index, index);
}

/* "team-plus" from "Team Plus", unique in the organization. */
static gchar *
venture_plan_unique_code(VentureDatabase *database, gint64 organization, const gchar *name)
{
	GString *base = g_string_new(NULL);
	gchar *code;
	const gchar *p;
	guint attempt;

	for (p = name; *p != '\0'; p++)
	{
		if (g_ascii_isalnum(*p))
			g_string_append_c(base, g_ascii_tolower(*p));
		else if (base->len > 0 && base->str[base->len - 1] != '-')
			g_string_append_c(base, '-');
	}
	while (base->len > 0 && base->str[base->len - 1] == '-')
		g_string_truncate(base, base->len - 1);
	if (base->len == 0)
		g_string_append(base, "plan");

	code = g_strdup(base->str);
	for (attempt = 2; attempt < 100; attempt++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PLAN);
		g_autoptr(VentureEntity) existing = NULL;

		venture_query_set_organization(query, organization);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
		existing = venture_database_find_one(database, query, NULL);
		if (existing == NULL)
			break;
		g_free(code);
		code = g_strdup_printf("%s-%u", base->str, attempt);
	}
	g_string_free(base, TRUE);
	return code;
}

/*
 * A price for @plan from typed values. The amount takes the business's
 * currency unless it names one, so "49" is enough.
 */
static VentureEntity *
venture_plan_price_from_form(VentureEntity *plan, const gchar *currency, const gchar *interval,
	const gchar *amount, gboolean per_seat, const gchar *trial_days, GError **error)
{
	g_autoptr(VentureEntity) price = g_object_new(VENTURE_TYPE_PLAN_PRICE, NULL);
	g_autofree gchar *money = NULL;
	gint64 trial = 0;

	venture_entity_set_organization_id(price, venture_entity_get_organization_id(plan));
	money = strpbrk(amount, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != NULL
		? g_strdup(amount) : g_strdup_printf("%s %s", amount, currency);
	if (trial_days != NULL && *trial_days != '\0')
		trial = g_ascii_strtoll(trial_days, NULL, 10);
	if (trial < 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A free trial cannot be negative");
		return NULL;
	}
	g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", currency,
		"per-seat", per_seat, "trial-days", trial, "active", TRUE, NULL);
	if (!venture_entity_set_field_from_string(price, "interval",
			interval != NULL && *interval != '\0' ? interval : "month", error) ||
		!venture_entity_set_field_from_string(price, "amount", money, error))
		return NULL;
	return g_steal_pointer(&price);
}

static HtmxResponse *
venture_plan_see_other(const gchar *path)
{
	HtmxResponse *response = htmx_response_new();

	htmx_response_set_status(response, 303);
	htmx_response_add_header(response, "Location", path);
	return response;
}

static gchar *
venture_plan_new_page(VentureWebServer *self, gint64 organization)
{
	GString *html = g_string_new("<form method=\"post\" action=\"/plans/new\" class=\"doc-compose\" data-doc-compose>"
		"<div class=\"page-head\"><div class=\"page-title\">");
	g_autofree gchar *currency = venture_bill_currency(self, organization);

	venture_web_append_crumbs(html, VENTURE_TYPE_PLAN, "plan", TRUE);
	g_string_append(html, "<h1>New plan</h1><span class=\"subtitle\">What a customer signs up to, and what it "
		"costs for each period you sell it by.</span></div><div class=\"page-actions\">"
		"<a class=\"btn btn-ghost\" href=\"/e/plan\">Cancel</a>"
		"<button class=\"btn btn-primary\" type=\"submit\">Save plan</button></div></div>"
		"<div class=\"doc-layout\"><section class=\"card doc-sheet\"><div class=\"card-body\">"
		"<div class=\"doc-parties\"><label class=\"doc-field doc-field-wide\"><span class=\"field-label\">Name"
		"<span class=\"required\">*</span></span><input type=\"text\" name=\"name\" required "
		"placeholder=\"For example: Team\" autofocus></label>");
	venture_plan_append_venture_select(self, html, organization);
	g_string_append(html, "</div>"
		"<label class=\"doc-field\"><span class=\"field-label\">What it includes</span>"
		"<textarea name=\"description\" rows=\"2\" placeholder=\"Shown to you when choosing a plan\"></textarea></label>"
		"<table class=\"line-items\"><thead><tr><th>Billed</th><th class=\"num\">Price</th><th></th>"
		"<th class=\"num\">Free trial (days)</th><th></th></tr></thead><tbody data-lines>");
	venture_plan_append_price_row(html, "0", "month", currency);
	g_string_append(html, "</tbody></table>"
		"<button type=\"button\" class=\"btn btn-ghost add-line\" data-add-line>+ Add a price</button>"
		"<template data-line-template>");
	venture_plan_append_price_row(html, "__N__", "year", currency);
	g_string_append(html, "</template></div></section>"
		"<aside class=\"doc-aside\"><section class=\"card\"><div class=\"card-body explainer\">"
		"<h3>One plan, several prices</h3>"
		"<p>Sell the same plan by the month and by the year \xe2\x80\x94 say $49 a month or $490 a year. "
		"Each is a price; the customer is put on one of them.</p>"
		"<p><strong>Per seat</strong> multiplies the price by the seats on the subscription. "
		"<strong>Free trial</strong> starts the subscription without an invoice until the trial ends.</p>"
		"<p>A price in use is never edited in place: retire it and add the new one, and the customers "
		"on the old price keep it until you move them.</p>"
		"</div></section></aside></div></form>"
		"<script>(function(){var f=document.querySelector('[data-doc-compose]');if(!f)return;"
		"var body=f.querySelector('[data-lines]'),tpl=f.querySelector('[data-line-template]'),n=1;"
		"f.querySelector('[data-add-line]').addEventListener('click',function(){"
		"body.insertAdjacentHTML('beforeend',tpl.innerHTML.replace(/__N__/g,String(n++)));"
		"body.lastElementChild.querySelector('input').focus();});"
		"body.addEventListener('click',function(e){var b=e.target.closest('[data-remove-line]');"
		"if(b&&body.querySelectorAll('[data-line]').length>1)b.closest('[data-line]').remove();});})();</script>");
	return g_string_free(html, FALSE);
}

/* GET and POST /plans/new: the plan and its prices, saved together. */
static HtmxResponse *
venture_plan_web_new(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) plan = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *currency = NULL, *code = NULL, *path = NULL;
	const gchar *name, *description;
	HtmxResponse *gate;
	VentureActor actor;
	gint64 organization;
	guint i, priced = 0;

	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "billing");
	if (gate != NULL)
		return gate;
	organization = venture_web_active_organization(self, request);
	if (organization == 0)
		organization = venture_context_get_default_organization_id(self->context);
	if (htmx_request_get_method(request) == HTMX_METHOD_GET)
		return venture_web_html_response(venture_web_page(self, request, "/e/plan", "New plan",
			venture_plan_new_page(self, organization)), 200);

	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	name = htmx_request_get_form_value(request, "name");
	description = htmx_request_get_form_value(request, "description");
	if (name == NULL || *name == '\0')
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A plan needs a name");
		return venture_web_error_response(error);
	}
	currency = venture_bill_currency(self, organization);
	code = venture_plan_unique_code(database, organization, name);

	if (!venture_database_begin(database, &error))
		return venture_web_error_response(error);
	{
		const gchar *venture = htmx_request_get_form_value(request, "venture-id");

		plan = g_object_new(VENTURE_TYPE_PLAN, "name", name, "code", code,
			"description", description != NULL ? description : "", "active", TRUE,
			"venture-id", venture != NULL ? g_ascii_strtoll(venture, NULL, 10) : (gint64)0, NULL);
	}
	venture_entity_set_organization_id(plan, organization);
	if (!venture_database_save(database, plan, &actor, &error))
		goto fail;
	/* Rows are numbered by the page; a gap is a row somebody removed. */
	for (i = 0; i < VENTURE_PLAN_MAX_PRICES; i++)
	{
		g_autofree gchar *amount_name = g_strdup_printf("price-%u-amount", i);
		g_autofree gchar *interval_name = g_strdup_printf("price-%u-interval", i);
		g_autofree gchar *seat_name = g_strdup_printf("price-%u-per-seat", i);
		g_autofree gchar *trial_name = g_strdup_printf("price-%u-trial-days", i);
		const gchar *amount = htmx_request_get_form_value(request, amount_name);
		g_autoptr(VentureEntity) price = NULL;

		if (amount == NULL || *amount == '\0')
			continue;
		price = venture_plan_price_from_form(plan, currency, htmx_request_get_form_value(request, interval_name),
			amount, htmx_request_get_form_value(request, seat_name) != NULL,
			htmx_request_get_form_value(request, trial_name), &error);
		if (price == NULL || !venture_database_save(database, price, &actor, &error))
			goto fail;
		priced++;
	}
	if (priced == 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A plan needs at least one price, or no customer can be put on it");
		goto fail;
	}
	if (!venture_database_commit(database, &error))
		goto fail;
	path = g_strdup_printf("/e/plan/%" G_GINT64_FORMAT, venture_entity_get_id(plan));
	return venture_plan_see_other(path);

fail:
	venture_database_rollback(database);
	return venture_web_error_response(error);
}

/* POST /plans/:id/prices -- add a price, retire one, or offer it again. */
static HtmxResponse *
venture_plan_web_prices(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) plan = NULL;
	g_autoptr(VentureEntity) price = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL, *currency = NULL;
	const gchar *action = htmx_request_get_form_value(request, "action");
	HtmxResponse *gate;
	VentureActor actor;

	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "billing");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	plan = venture_database_get(database, VENTURE_TYPE_PLAN,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (plan == NULL)
		return venture_web_error_response(error);

	if (g_strcmp0(action, "add") == 0)
	{
		const gchar *amount = htmx_request_get_form_value(request, "amount");

		if (amount == NULL || *amount == '\0')
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A price needs an amount");
			return venture_web_error_response(error);
		}
		currency = venture_bill_currency(self, venture_entity_get_organization_id(plan));
		price = venture_plan_price_from_form(plan, currency, htmx_request_get_form_value(request, "interval"),
			amount, htmx_request_get_form_value(request, "per-seat") != NULL,
			htmx_request_get_form_value(request, "trial-days"), &error);
	}
	else if (g_strcmp0(action, "retire") == 0 || g_strcmp0(action, "restore") == 0)
	{
		const gchar *id = htmx_request_get_form_value(request, "price");
		gint64 owner = 0;

		price = venture_database_get(database, VENTURE_TYPE_PLAN_PRICE,
			id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, &error);
		if (price != NULL)
			g_object_get(price, "plan-id", &owner, NULL);
		if (price != NULL && owner != venture_entity_get_id(plan))
		{
			g_clear_object(&price);
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "That price is not this plan's");
		}
		if (price != NULL)
			g_object_set(price, "active", g_strcmp0(action, "restore") == 0, NULL);
	}
	else
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Unknown price action");

	if (price == NULL || !venture_database_save(database, price, &actor, &error))
		return venture_web_error_response(error);
	path = g_strdup_printf("/e/plan/%" G_GINT64_FORMAT, venture_entity_get_id(plan));
	return venture_plan_see_other(path);
}

/* How many customers are on @price now: trialing, active or past due. */
static guint
venture_plan_price_customers(VentureWebServer *self, gint64 price_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CUSTOMER_SUBSCRIPTION);
	g_autoptr(GPtrArray) rows = NULL;
	guint i, count = 0;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "plan-price-id", VENTURE_FILTER_OP_EQ, price_id, NULL);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		gint status = 0;

		g_object_get(g_ptr_array_index(rows, i), "status", &status, NULL);
		if (status <= 2)
			count++;
	}
	return count;
}

/*
 * On a plan's page: its prices, who is on each, and a line to add one for
 * another period. Retiring a price stops it being offered; the customers
 * on it keep it.
 */
static void
venture_plan_web_panel(VentureWebServer *self, GString *html, VentureEntity *record, VentureAuthPrincipal *principal)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) prices = NULL;
	g_autofree gchar *currency = NULL;
	gboolean editor;
	gint64 id;
	guint i;

	if (!VENTURE_IS_PLAN(record) || !venture_web_module_enabled(self, "billing"))
		return;
	id = venture_entity_get_id(record);
	editor = venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, NULL);
	query = venture_query_new(VENTURE_TYPE_PLAN_PRICE);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "plan-id", VENTURE_FILTER_OP_EQ, id, NULL);
	venture_query_add_order(query, "interval", VENTURE_SORT_ASCENDING, NULL);
	prices = venture_database_find(venture_context_get_database(self->context), query, NULL);

	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Prices</h2></div><div class=\"card-body\">");
	if (prices == NULL || prices->len == 0)
		g_string_append(html, "<p class=\"next-up-lead\">No price yet, so no customer can be put on this plan. "
			"Add one below.</p>");
	else
	{
		g_string_append(html, "<table class=\"line-items\"><thead><tr><th>Price</th><th class=\"num\">Free trial</th>"
			"<th class=\"num\">Customers</th><th>Offered</th><th></th></tr></thead><tbody>");
		for (i = 0; i < prices->len; i++)
		{
			VentureEntity *price = g_ptr_array_index(prices, i);
			g_autofree gchar *label = venture_entity_get_display_name(price);
			gboolean active = FALSE;
			gint64 trial = 0;

			g_object_get(price, "active", &active, "trial-days", &trial, NULL);
			g_string_append_printf(html, "<tr><td><a href=\"/e/plan_price/%" G_GINT64_FORMAT "\">",
				venture_entity_get_id(price));
			venture_html_escape_append(html, label);
			g_string_append(html, "</a></td><td class=\"num\">");
			if (trial > 0)
				g_string_append_printf(html, "%" G_GINT64_FORMAT " days", trial);
			else
				g_string_append(html, "\xe2\x80\x94");
			g_string_append_printf(html, "</td><td class=\"num\">%u</td><td><span class=\"badge %s\">%s</span></td><td>",
				venture_plan_price_customers(self, venture_entity_get_id(price)),
				active ? "positive" : "muted", active ? "Offered" : "Retired");
			if (editor)
				g_string_append_printf(html, "<form method=\"post\" action=\"/plans/%" G_GINT64_FORMAT "/prices\">"
					"<input type=\"hidden\" name=\"action\" value=\"%s\">"
					"<input type=\"hidden\" name=\"price\" value=\"%" G_GINT64_FORMAT "\">"
					"<button class=\"btn btn-ghost btn-sm\" type=\"submit\">%s</button></form>",
					id, active ? "retire" : "restore", venture_entity_get_id(price),
					active ? "Retire" : "Offer again");
			g_string_append(html, "</td></tr>");
		}
		g_string_append(html, "</tbody></table>");
	}
	if (editor)
	{
		currency = venture_bill_currency(self, venture_entity_get_organization_id(record));
		g_string_append_printf(html, "<form method=\"post\" action=\"/plans/%" G_GINT64_FORMAT "/prices\" "
			"class=\"price-add\"><input type=\"hidden\" name=\"action\" value=\"add\">"
			"<h3>Add a price</h3><div class=\"doc-parties\"><label class=\"doc-field\">"
			"<span class=\"field-label\">Billed</span>", id);
		venture_plan_append_period_select(html, "interval", "month");
		g_string_append_printf(html, "</label><label class=\"doc-field\"><span class=\"field-label\">Price</span>"
			"<span class=\"money-input\"><input type=\"text\" name=\"amount\" required inputmode=\"decimal\" "
			"placeholder=\"0.00\"><span class=\"unit\">%s</span></span></label>"
			"<label class=\"doc-field\"><span class=\"field-label\">Free trial (days)</span>"
			"<input type=\"number\" name=\"trial-days\" min=\"0\" step=\"1\" placeholder=\"0\"></label></div>"
			"<label class=\"checkbox\"><input type=\"checkbox\" name=\"per-seat\" value=\"true\"> Per seat</label>"
			"<p><button class=\"btn btn-primary\" type=\"submit\">Add price</button></p></form>", currency);
	}
	g_string_append(html, "</div></section>");
}

/*
 * Every price a customer can be put on, under its plan's name, for a
 * subscription's plan picker; @except is left out (the price already
 * held), and with @venture set only that venture's plans and the shared
 * ones are offered. Each option carries its plan's venture, so the page
 * can narrow the list to the customer chosen. Plans that are offered but have no price are returned in
 * @unpriced, so the page can say so rather than leave them out silently.
 */
static void
venture_plan_web_append_price_options(VentureWebServer *self, GString *html, gint64 organization,
	gint64 except, gint64 venture, GPtrArray *unpriced)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) plans_query = venture_query_new(VENTURE_TYPE_PLAN);
	g_autoptr(GPtrArray) plans = NULL;
	guint i, j;

	venture_query_set_limit(plans_query, 0);
	venture_query_add_order(plans_query, "name", VENTURE_SORT_ASCENDING, NULL);
	if (organization > 0)
		venture_query_set_organization(plans_query, organization);
	plans = venture_database_find(database, plans_query, NULL);
	for (i = 0; plans != NULL && i < plans->len; i++)
	{
		VentureEntity *plan = g_ptr_array_index(plans, i);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PLAN_PRICE);
		g_autoptr(GPtrArray) prices = NULL;
		g_autofree gchar *name = NULL;
		gboolean active = FALSE, opened = FALSE, offered = FALSE;
		gint64 plan_venture = 0;

		g_object_get(plan, "name", &name, "active", &active, "venture-id", &plan_venture, NULL);
		if (!active || (venture > 0 && plan_venture > 0 && plan_venture != venture))
			continue;
		venture_query_set_limit(query, 0);
		venture_query_add_filter_int(query, "plan-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(plan), NULL);
		venture_query_add_order(query, "interval", VENTURE_SORT_ASCENDING, NULL);
		prices = venture_database_find(database, query, NULL);
		for (j = 0; prices != NULL && j < prices->len; j++)
		{
			VentureEntity *price = g_ptr_array_index(prices, j);
			g_autofree gchar *label = NULL;
			gboolean price_active = FALSE;

			g_object_get(price, "active", &price_active, NULL);
			if (!price_active)
				continue;
			offered = TRUE;
			if (venture_entity_get_id(price) == except)
				continue;
			if (!opened)
			{
				g_string_append(html, "<optgroup label=\"");
				venture_html_escape_append(html, name);
				g_string_append(html, "\">");
				opened = TRUE;
			}
			label = venture_entity_get_display_name(price);
			g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\" data-venture=\"%" G_GINT64_FORMAT "\">",
				venture_entity_get_id(price), plan_venture);
			venture_html_escape_append(html, name);
			g_string_append(html, " \xe2\x80\x94 ");
			venture_html_escape_append(html, label);
			g_string_append(html, "</option>");
		}
		if (opened)
			g_string_append(html, "</optgroup>");
		if (!offered && unpriced != NULL)
			g_ptr_array_add(unpriced, g_object_ref(plan));
	}
}
