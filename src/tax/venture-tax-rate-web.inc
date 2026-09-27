/*
 * venture-tax-rate-web.inc - A tax rate from a name and a percent
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. A tax code stores its rate as an exact
 * fraction, which is right for the arithmetic and wrong for a person: this
 * page asks for "8.875" and writes 8875 over 100000. The code the return
 * needs is made from the name. Everything else about the record is the
 * ordinary record, edited on its ordinary page.
 */

/* How many decimals of a percent a rate may carry. */
#define VENTURE_TAX_RATE_MAX_DECIMALS	6

/*
 * "8.875" or "8.875%" as an exact fraction of the net amount. Refuses
 * anything that is not a plain non-negative decimal, rather than guessing
 * what "eight" or "8,875" meant.
 */
static gboolean
venture_tax_rate_parse(const gchar *text, gint64 *numerator, gint64 *denominator, GError **error)
{
	g_autofree gchar *clean = g_strstrip(g_strdup(text != NULL ? text : ""));
	gint64 value = 0, scale = 100;
	gboolean point = FALSE;
	guint decimals = 0, digits = 0;
	gsize length = strlen(clean);
	const gchar *p;

	if (length > 0 && clean[length - 1] == '%')
		clean[--length] = '\0';
	for (p = clean; *p != '\0'; p++)
	{
		if (*p == '.' && !point)
		{
			point = TRUE;
			continue;
		}
		if (!g_ascii_isdigit(*p) || digits >= 12 || (point && decimals >= VENTURE_TAX_RATE_MAX_DECIMALS))
			goto refuse;
		value = value * 10 + (*p - '0');
		digits++;
		if (point)
		{
			decimals++;
			scale *= 10;
		}
	}
	if (digits == 0)
		goto refuse;
	*numerator = value;
	*denominator = scale;
	return TRUE;

refuse:
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"The rate should be a percent such as 8.875, not \"%s\"", text != NULL ? text : "");
	return FALSE;
}

/* NEW-YORK-CITY from "New York City": the code the return is filed under. */
static gchar *
venture_tax_rate_code(const gchar *name)
{
	GString *code = g_string_new(NULL);
	const gchar *p;

	for (p = name; *p != '\0'; p++)
	{
		if (g_ascii_isalnum(*p))
			g_string_append_c(code, g_ascii_toupper(*p));
		else if (code->len > 0 && code->str[code->len - 1] != '-')
			g_string_append_c(code, '-');
	}
	while (code->len > 0 && code->str[code->len - 1] == '-')
		g_string_truncate(code, code->len - 1);
	if (code->len == 0)
		g_string_append(code, "TAX");

	return g_string_free(code, FALSE);
}

static gchar *
venture_tax_rate_page(void)
{
	GString *html = g_string_new("<form method=\"post\" action=\"/tax-rates/new\" class=\"doc-compose\">"
		"<div class=\"page-head\"><div class=\"page-title\">");

	venture_web_append_crumbs(html, VENTURE_TYPE_TAX_CODE, "tax_code", TRUE);
	g_string_append(html, "<h1>New tax rate</h1></div><div class=\"page-actions\">"
		"<a class=\"btn btn-ghost\" href=\"/e/tax_code\">Cancel</a>"
		"<button class=\"btn btn-primary\" type=\"submit\">Save rate</button></div></div>"
		"<section class=\"card\"><div class=\"card-body\">"
		"<label class=\"doc-field\"><span class=\"field-label\">Name<span class=\"required\">*</span></span>"
		"<input type=\"text\" name=\"name\" required placeholder=\"For example: New York sales tax\" autofocus></label>"
		"<label class=\"doc-field\"><span class=\"field-label\">Rate<span class=\"required\">*</span></span>"
		"<input type=\"text\" name=\"rate\" required inputmode=\"decimal\" placeholder=\"8.875\">"
		"<span class=\"field-help\">A percent. Kept exactly, to six decimals.</span></label>"
		"<label class=\"doc-field\"><span class=\"field-label\">Where it applies</span>"
		"<input type=\"text\" name=\"jurisdiction\" placeholder=\"For example: US-NY\">"
		"<span class=\"field-help\">The authority the tax is filed with. Optional.</span></label>"
		"<p class=\"muted small\">Once saved, the rate is offered on every invoice line; "
		"\xe2\x80\x9cNo tax\xe2\x80\x9d is always there too.</p></div></section></form>");

	return g_string_free(html, FALSE);
}

/* GET and POST /tax-rates/new. Editors, like any record they may create. */
static HtmxResponse *
venture_tax_rate_new(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) rate = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = NULL, *code = NULL, *path = NULL;
	const gchar *name, *jurisdiction;
	VentureDatabase *database = venture_context_get_database(self->context);
	HtmxResponse *gate;
	VentureActor actor;
	gint64 organization, numerator = 0, denominator = 0;
	guint attempt;

	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "finance");
	if (gate != NULL)
		return gate;
	if (htmx_request_get_method(request) == HTMX_METHOD_GET)
		return venture_web_html_response(venture_web_page(self, request, "/e/tax_code", "New tax rate",
			venture_tax_rate_page()), 200);

	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	organization = venture_web_active_organization(self, request);
	if (organization == 0)
		organization = venture_context_get_default_organization_id(self->context);

	name = htmx_request_get_form_value(request, "name");
	jurisdiction = htmx_request_get_form_value(request, "jurisdiction");
	if (name == NULL || *name == '\0')
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A tax rate needs a name");
		return venture_web_error_response(error);
	}
	if (!venture_tax_rate_parse(htmx_request_get_form_value(request, "rate"), &numerator, &denominator, &error))
		return venture_web_error_response(error);

	/* The code is unique in the organization; a second "Sales tax" is
	 * SALES-TAX-2 rather than a refusal about a field nobody typed. */
	base = venture_tax_rate_code(name);
	code = g_strdup(base);
	for (attempt = 2; attempt < 100; attempt++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TAX_CODE);
		g_autoptr(VentureEntity) existing = NULL;

		venture_query_set_organization(query, organization);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
		existing = venture_database_find_one(database, query, NULL);
		if (existing == NULL)
			break;
		g_free(code);
		code = g_strdup_printf("%s-%u", base, attempt);
	}

	rate = g_object_new(VENTURE_TYPE_TAX_CODE, "code", code, "name", name,
		"jurisdiction", jurisdiction != NULL ? jurisdiction : "",
		"rate-numerator", numerator, "rate-denominator", denominator, "active", TRUE, NULL);
	venture_entity_set_organization_id(rate, organization);
	if (!venture_database_save(database, rate, &actor, &error))
		return venture_web_error_response(error);

	path = g_strdup_printf("/e/tax_code/%" G_GINT64_FORMAT, venture_entity_get_id(rate));
	return venture_web_redirect_to(path);
}
