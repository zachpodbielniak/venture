/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included by the web server; all entry points select config names only. */
/* Expose configured names to viewers; reserve network probes and test pages for administrators. */
static HtmxResponse *
venture_web_printers(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	user_data
){
	VentureWebServer		*self = user_data;
	VentureConfig			*config = venture_context_get_config(self->context);
	const gchar				*name = g_hash_table_lookup(params, "name");
	g_autoptr(GError)		error = NULL;
	g_autoptr(JsonBuilder)	builder = json_builder_new();
	g_autoptr(JsonNode)		node = NULL;
	g_autoptr(GBytes)		bytes = NULL;
	g_autoptr(GBytes)		result = NULL;

	HtmxResponse				*gate = venture_web_api_require(
		self, request, name ? VENTURE_USER_ROLE_ADMIN : VENTURE_USER_ROLE_VIEWER);

	if (gate)
	{
		return gate;
	}
	if (!name)
	{
		const GPtrArray	*printers = venture_config_get_printers(config);
		guint			i;

		json_builder_begin_array(builder);

		for (i = 0; i < printers->len; i++)
		{
			const VenturePrinter	*printer = g_ptr_array_index(printers, i);

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "name");
			json_builder_add_string_value(builder, printer->name);
			json_builder_set_member_name(builder, "default");
			json_builder_add_boolean_value(
				builder, !g_strcmp0(printer->name, venture_config_get_default_printer(config)));
			/* Listing never probes the network on behalf of a viewer. */
			json_builder_set_member_name(builder, "state");
			json_builder_add_string_value(builder, "unknown");
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
	}
	else
	{
		const VenturePrinter	*printer = venture_config_find_printer(config, name, &error);
		gboolean				status = g_str_has_suffix(htmx_request_get_path(request), "/status");

		if (!printer)
		{
			return venture_web_error_response(error);
		}
		if (!status)
		{
			VentureEscposDocument	document;

			venture_escpos_document_init(&document, printer);
			document.title = "VENTURE printer test";
			document.body = "Receipt printer ready.\n0123456789\n";
			bytes = venture_escpos_render(&document, &error);

			if (!bytes)
			{
				return venture_web_error_response(error);
			}
		}
		result = venture_printer_exchange(printer, bytes, &error);

		if (!result)
		{
			/* Transport errors contain addresses. Keep them out of responses. */
			g_clear_error(&error);
			g_set_error_literal(
				&error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
				"Printer exchange failed or timed out; check paper before retrying");
			return venture_web_error_response(error);
		}
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "state");

		if (status)
		{
			const guint8	*values = g_bytes_get_data(result, NULL);

			json_builder_add_string_value(
				builder, values[0] & VENTURE_ESCPOS_STATUS_OFFLINE ? "offline" : "online");
			json_builder_set_member_name(builder, "paper");
			json_builder_add_string_value(builder,
										  (values[1] & VENTURE_ESCPOS_STATUS_PAPER_OUT) ==
												  VENTURE_ESCPOS_STATUS_PAPER_OUT
											  ? "out"
										  : (values[1] & VENTURE_ESCPOS_STATUS_PAPER_NEAR_END) ==
												  VENTURE_ESCPOS_STATUS_PAPER_NEAR_END
											  ? "near end"
											  : "ok");
		}
		else
		{
			json_builder_add_string_value(builder, "sent");
		}
		json_builder_end_object(builder);
	}
	node = json_builder_get_root(builder);
	return venture_web_json_response(node, 200);
}

/* Apply the record read boundary and accept only configured printer names. */
static HtmxResponse *
venture_web_print_record(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	user_data
){
	VentureWebServer				*self = user_data;
	gboolean						api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	g_autoptr(VentureAuthPrincipal)	principal = NULL;
	g_autoptr(VentureEntity)		record = NULL;
	g_autoptr(JsonNode)				body = NULL;
	g_autoptr(GBytes)				bytes = NULL;
	g_autoptr(GBytes)				result = NULL;
	g_autoptr(GError)				error = NULL;
	const gchar						*type_name = g_hash_table_lookup(params, "type");
	const gchar						*name = NULL;
	const VenturePrinter			*printer;
	GType							type;

	HtmxResponse				*gate = api ? venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER)
							 : venture_web_ui_require_session(self, request);

	if (gate)
	{
		return gate;
	}
	if (!g_strcmp0(type_name, "payment"))
	{
		type = VENTURE_TYPE_PAYMENT;
	}
	else if (!g_strcmp0(type_name, "invoice"))
	{
		type = VENTURE_TYPE_INVOICE;
	}
	else
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
							"Only payments and invoices can be printed");
		return venture_web_error_response(error);
	}
	gate = api ? venture_web_require_module_api(self, type == VENTURE_TYPE_PAYMENT ? "receivables"
																				   : "invoicing")
			   : venture_web_require_module_ui(
					 self, request, type == VENTURE_TYPE_PAYMENT ? "receivables" : "invoicing");

	if (gate)
	{
		return gate;
	}
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_require_for_type(self, principal, type, VENTURE_USER_ROLE_VIEWER, &error))
	{
		return venture_web_error_response(error);
	}
	if (api)
	{
		JsonObject *object;
		g_autoptr(GList)	members = NULL;
		GList				*iter;

		body = htmx_request_get_json(request, &error);

		if (!body || !JSON_NODE_HOLDS_OBJECT(body))
		{
			goto invalid;
		}
		object = json_node_get_object(body);
		members = json_object_get_members(object);

		for (iter = members; iter; iter = iter->next)
		{
			JsonNode	*value = json_object_get_member(object, iter->data);

			if (g_strcmp0(iter->data, "printer") || json_node_get_value_type(value) != G_TYPE_STRING)
			{
				goto invalid;
			}
			name = json_node_get_string(value);
		}
	}
	else
	{
		name = htmx_request_get_form_value(request, "printer");
	}
	printer = venture_config_find_printer(venture_context_get_config(self->context), name, &error);

	if (!printer)
	{
		return venture_web_error_response(error);
	}
	record =
		venture_database_get(venture_context_get_database(self->context), type,
							 g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);

	if (!record)
	{
		return venture_web_error_response(error);
	}
	bytes = venture_financial_documents_thermal(self->context, record, printer, &error);

	if (!bytes)
	{
		return venture_web_error_response(error);
	}
	result = venture_printer_exchange(printer, bytes, &error);

	if (!result)
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
							"Printer exchange failed or timed out; delivery may be partial, check "
							"paper before retrying");
		return venture_web_error_response(error);
	}
	/* PDF download does not record an audit write; physical delivery follows
	 * that same read boundary, and does not enter staged confirmations. */
	if (!api)
	{
		g_autofree gchar	*path =
			g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, type_name, venture_entity_get_id(record));
		/* Post-redirect-get keeps the ordinary styled record page and prevents
		 * a browser refresh from sending the same physical receipt again. */
		return venture_web_redirect_to(path);
	}
	g_clear_pointer(&body, json_node_unref);
	body = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(body, json_object_new());
	json_object_set_string_member(json_node_get_object(body), "state", "sent");
	return venture_web_json_response(body, 200);
invalid:
	g_clear_error(&error);
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
						"Expected a JSON object with only an optional printer name");
	return venture_web_error_response(error);
}

/* Use the shared form classes so both looks expose the same default printer selector. */
static void
venture_web_append_print_control(
	VentureWebServer	*self,
	GString				*html,
	VentureEntity		*record
){
	VentureConfig	*config = venture_context_get_config(self->context);
	const GPtrArray	*printers = venture_config_get_printers(config);
	guint			i;

	if ((!VENTURE_IS_PAYMENT(record) && !VENTURE_IS_INVOICE(record)) || !printers->len)
	{
		return;
	}
	g_string_append_printf(
		html,
		"<section class=\"card\"><div class=\"card-head\"><h2>Receipt printer</h2></div>"
		"<form class=\"card-body\" method=\"post\" action=\"/print/%s/%" G_GINT64_FORMAT "\">"
		"<label class=\"field\"><span class=\"field-label\">Printer</span><select "
		"name=\"printer\">",
		VENTURE_IS_PAYMENT(record) ? "payment" : "invoice", venture_entity_get_id(record));

	for (i = 0; i < printers->len; i++)
	{
		const VenturePrinter	*printer = g_ptr_array_index(printers, i);

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, printer->name);
		g_string_append_printf(html, "\"%s>",
							   !g_strcmp0(printer->name, venture_config_get_default_printer(config))
								   ? " selected"
								   : "");
		venture_html_escape_append(html, printer->name);
		g_string_append(html, "</option>");
	}
	g_string_append(html, "</select></label><button class=\"btn\" type=\"submit\">Print "
						  "receipt</button></form></section>");
}
