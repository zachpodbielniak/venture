/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-marketdata-web.inc - /api/v1/market/quote and /api/v1/market/promote
 *
 * Included by venture-web-server.c and registered before the generic
 * /api/v1/:type routes, which would otherwise read /api/v1/market/quote
 * as a record type called "market". The records themselves -- venue,
 * instrument, watchlist, watchlist_entry -- are served by the generic
 * routes; these two are what is not a record: asking the price oracle,
 * and promoting a store's venue or instrument into one. Both answer for
 * one organization -- organization_id, else the active one -- and both
 * are module-gated. Built
 * with or without SQLite: without a store the oracle answers from
 * observations only, and promotion says why it cannot.
 */

/* An optional positive id from the query string; 0 when absent. */
static gboolean
venture_marketdata_web_id(
	HtmxRequest	 *request,
	const gchar	 *name,
	gint64		 *out,
	GError		**error
){
	const gchar *text;

	*out = 0;
	text = htmx_request_get_query_param(request, name);

	if (venture_string_is_empty(text))
		return TRUE;

	if (!g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, out, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s must be a positive number", name);
		return FALSE;
	}

	return TRUE;
}

/*
 * GET /api/v1/market/quote: the price oracle, as JSON. Query string:
 * organization_id (else the active one); instrument_id or product_id; venue_id or where; basis (market by
 * default); at; currency; prefer_currency; fallback (true/false) and
 * source (the observation source a fallback may read).
 */
static HtmxResponse *
venture_marketdata_web_quote(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) at = NULL;
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(VentureMarketdataEvidence) evidence = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureMarketdataQuestion question;
	const gchar *text;
	gdouble value;
	gboolean number;
	gint basis;
	HtmxResponse *gate;

	(void)params;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	gate = venture_web_require_module_api(self, "marketdata");

	if (NULL != gate)
		return gate;

	venture_marketdata_question_init(&question);

	if (!venture_web_request_organization(self, request, NULL, &question.organization_id, &error))
		return venture_web_error_response(error);

	if (!venture_marketdata_web_id(request, "instrument_id", &question.instrument_id, &error) ||
	    !venture_marketdata_web_id(request, "product_id", &question.product_id, &error) ||
	    !venture_marketdata_web_id(request, "venue_id", &question.venue_id, &error))
		return venture_web_error_response(error);

	text = htmx_request_get_query_param(request, "basis");
	basis = (gint)VENTURE_MARKETDATA_BASIS_MARKET;

	if (!venture_string_is_empty(text) &&
	    !venture_enum_from_nick(VENTURE_TYPE_MARKETDATA_BASIS, text, &basis))
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "basis \"%s\" is not a basis", text);
		return venture_web_error_response(error);
	}

	question.basis = (VentureMarketdataBasis)basis;
	text = htmx_request_get_query_param(request, "at");

	if (!venture_string_is_empty(text))
	{
		at = venture_time_from_string(text, &error);

		if (NULL == at)
			return venture_web_error_response(error);

		question.at = at;
	}

	text = htmx_request_get_query_param(request, "where");
	question.where = venture_string_is_empty(text) ? NULL : text;
	text = htmx_request_get_query_param(request, "currency");
	question.currency = venture_string_is_empty(text) ? NULL : text;
	text = htmx_request_get_query_param(request, "prefer_currency");
	question.prefer_currency = venture_string_is_empty(text) ? NULL : text;
	text = htmx_request_get_query_param(request, "source");
	question.fallback_source = venture_string_is_empty(text) ? NULL : text;
	text = htmx_request_get_query_param(request, "fallback");
	question.allow_fallback = (NULL != text) &&
		((0 == g_ascii_strcasecmp(text, "true")) || (0 == g_strcmp0(text, "1")));

	number = venture_marketdata_basis_is_number(question.basis);
	value = NAN;

	if (number ? !venture_marketdata_reference_number(self->context, &question, &value,
	                                                  &evidence, &error)
	           : !venture_marketdata_reference_price(self->context, &question, &price,
	                                                 &evidence, &error))
		return venture_web_error_response(error);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "basis");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_MARKETDATA_BASIS, (gint)question.basis));
	json_builder_set_member_name(builder, "found");
	json_builder_add_boolean_value(builder, number ? !isnan(value) : (NULL != price));
	json_builder_set_member_name(builder, "price");

	if (NULL != price)
		json_builder_add_value(builder, venture_money_to_json(price));
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "value");

	if (number && !isnan(value))
		json_builder_add_double_value(builder, value);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "evidence");
	json_builder_add_value(builder, venture_marketdata_evidence_to_json(evidence));
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 200);
}

/*
 * POST /api/v1/market/promote: {"data_source_id": N, "kind": "instrument"
 * or "venue", "key": "...", "organization_id": N (optional)} makes (or finds, or restores) the record and
 * answers with it. An editor's: it creates a record like any other write.
 */
static HtmxResponse *
venture_marketdata_web_promote(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *body;
	const gchar *kind;
	const gchar *key;
	const gchar *bytes;
	VentureActor actor;
	GBytes *raw;
	gsize length;
	gint64 source_id;
	gint64 organization_id;
	gboolean ok;
	HtmxResponse *gate;

	(void)params;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != gate)
		return gate;

	gate = venture_web_require_module_api(self, "marketdata");

	if (NULL != gate)
		return gate;

	raw = htmx_request_get_body_bytes(request);
	length = 0;
	bytes = (NULL != raw) ? g_bytes_get_data(raw, &length) : NULL;
	parser = json_parser_new();

	if ((NULL == bytes) || (0 == length) || (length > 16384) || (NULL != memchr(bytes, 0, length)) ||
	    !json_parser_load_from_data(parser, bytes, (gssize)length, NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Send a JSON object: data_source_id, kind and key");
		return venture_web_error_response(error);
	}

	body = json_node_get_object(json_parser_get_root(parser));
	source_id = venture_json_object_get_int(body, "data_source_id", 0);
	kind = venture_json_object_get_string(body, "kind", "instrument");
	key = venture_json_object_get_string(body, "key", NULL);

	if ((source_id <= 0) || venture_string_is_empty(key) ||
	    ((0 != g_strcmp0(kind, "instrument")) && (0 != g_strcmp0(kind, "venue"))))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "data_source_id (a positive number), kind (instrument or venue) "
		                    "and key are required");
		return venture_web_error_response(error);
	}

	if (!venture_web_request_organization(self, request, body, &organization_id, &error))
		return venture_web_error_response(error);

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	ok = (0 == g_strcmp0(kind, "venue"))
		? venture_marketdata_promote_venue(self->context, organization_id, source_id, key,
		                                   &actor, &record, &error)
		: venture_marketdata_promote_instrument(self->context, organization_id, source_id, key,
		                                        &actor, &record, &error);

	if (!ok)
		return venture_web_error_response(error);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE);

	return venture_web_json_response(node, 200);
}

static void
venture_marketdata_web_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/market/quote",
	                                        VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE,
	                                        venture_marketdata_web_quote, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/market/promote",
	                                        VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE,
	                                        venture_marketdata_web_promote, self);
}
