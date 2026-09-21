/* SPDX-License-Identifier: AGPL-3.0-or-later */
gboolean
venture_commerce_check_write(VentureDatabase *database, VentureEntity *entity, gboolean removal, GError **error)
{
	gpointer permit;
	if (!VENTURE_IS_COMMERCE_IMPORT_LINK(entity)) return TRUE;
	permit = g_object_get_data(G_OBJECT(database), "commerce-identity-permit");
	g_object_set_data(G_OBJECT(database), "commerce-identity-permit", NULL);
	if (removal || permit != entity || venture_entity_get_id(entity) != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Commerce identity evidence is immutable and written only by import or explicit adoption");
		return FALSE;
	}
	return TRUE;
}
static gchar *
commerce_identity_key(VentureIntegrationConnection *binding, const gchar *kind, const gchar *remote)
{
	g_autofree gchar *provider = NULL, *account = NULL, *environment = NULL, *wire = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
	JsonArray *parts = json_array_new();
	g_object_get(binding, "provider", &provider, "account-id", &account, "environment", &environment, NULL);
	json_array_add_int_element(parts, venture_entity_get_organization_id(VENTURE_ENTITY(binding)));
	json_array_add_string_element(parts, provider);
	json_array_add_string_element(parts, account);
	json_array_add_string_element(parts, environment);
	json_array_add_string_element(parts, kind);
	json_array_add_string_element(parts, remote);
	json_node_take_array(node, parts);
	wire = venture_json_to_string(node, FALSE);
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, wire, -1);
}
static VentureEntity *
commerce_identity_find(VentureCommerceService *self, VentureIntegrationConnection *binding,
	const gchar *kind, const gchar *remote, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMMERCE_IMPORT_LINK);
	g_autofree gchar *key = commerce_identity_key(binding, kind, remote);
	venture_query_set_organization(query, self->organization_id);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_string(query, "identity-key", VENTURE_FILTER_OP_EQ, key, error)) return NULL;
	return venture_database_find_one(self->database, query, error);
}
static gboolean
commerce_legacy_clear(VentureCommerceService *self, GType type, const gchar *kind, const gchar *remote, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, self->organization_id);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, remote, error)) return FALSE;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureQuery) claims = venture_query_new(VENTURE_TYPE_COMMERCE_IMPORT_LINK);
		g_autoptr(VentureEntity) claim = NULL;
		g_autofree gchar *target = g_strdup_printf("%s:%" G_GINT64_FORMAT, kind,
			venture_entity_get_id(g_ptr_array_index(rows, i)));
		venture_query_set_organization(claims, self->organization_id);
		venture_query_set_include_deleted(claims, TRUE);
		if (!venture_query_add_filter_string(claims, "target-key", VENTURE_FILTER_OP_EQ, target, error)) return FALSE;
		claim = venture_database_find_one(self->database, claims, error);
		if (error && *error) return FALSE;
		if (claim == NULL)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"Unbound legacy commerce identity exists; explicitly adopt its original account before importing");
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean
commerce_identity_save(VentureCommerceService *self, VentureIntegrationConnection *binding,
	const gchar *kind, const gchar *remote, gint64 target, const gchar *reason, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) link = VENTURE_ENTITY(venture_commerce_import_link_new());
	g_autofree gchar *key = commerce_identity_key(binding, kind, remote), *target_key = NULL;
	g_autofree gchar *provider = NULL, *account = NULL, *environment = NULL;
	gboolean ok;
	g_object_get(binding, "provider", &provider, "account-id", &account, "environment", &environment, NULL);
	target_key = g_strdup_printf("%s:%" G_GINT64_FORMAT, kind, target);
	venture_entity_set_organization_id(link, self->organization_id);
	g_object_set(link, "name", remote, "identity-key", key, "target-key", target_key,
		"provider", provider, "account-id", account, "environment", environment, "remote-id", remote,
		"connection-id", venture_entity_get_id(VENTURE_ENTITY(binding)), "reason", reason,
		g_str_equal(kind, "order") ? "invoice-id" : "company-id", target, NULL);
	g_object_set_data(G_OBJECT(self->database), "commerce-identity-permit", link);
	ok = venture_database_save(self->database, link, actor, error);
	g_object_set_data(G_OBJECT(self->database), "commerce-identity-permit", NULL);
	return ok;
}
static gboolean
commerce_identity_customer(VentureCommerceService *self, VentureIntegrationConnection *binding,
	JsonObject *spec, const VentureActor *actor, GError **error)
{
	const gchar *remote = venture_json_object_get_string(spec, "customer_external_id", NULL);
	g_autoptr(VentureEntity) link = NULL;
	g_autofree gchar *key = NULL, *external = NULL;
	gint64 id = venture_json_object_get_int(spec, "company_id", 0);
	if (id > 0) return TRUE;
	if (venture_string_is_empty(remote) || strlen(remote) > 1024)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Commerce customer requires a stable provider identity or an explicit company_id; email is not identity");
		return FALSE;
	}
	link = commerce_identity_find(self, binding, "customer", remote, error);
	if (error && *error) return FALSE;
	if (link != NULL)
	{
		g_object_get(link, "company-id", &id, NULL);
		json_object_set_int_member(spec, "company_id", id);
		return TRUE;
	}
	if (!commerce_legacy_clear(self, VENTURE_TYPE_COMPANY, "customer", remote, error)) return FALSE;
	key = commerce_identity_key(binding, "customer", remote);
	external = g_strconcat("commerce:", key, NULL);
	{
		g_autoptr(VentureEntity) company = VENTURE_ENTITY(venture_company_new());
		const gchar *name = venture_json_object_get_string(spec, "customer_name", remote);
		venture_entity_set_organization_id(company, self->organization_id);
		g_object_set(company, "name", name, "kind", VENTURE_COMPANY_KIND_CUSTOMER,
			"email", venture_json_object_get_string(spec, "customer_email", NULL),
			"external-id", external, "source", "commerce", "active", TRUE, NULL);
		if (!venture_database_save(self->database, company, actor, error)) return FALSE;
		id = venture_entity_get_id(company);
	}
	if (!commerce_identity_save(self, binding, "customer", remote, id, "Provider import", actor, error)) return FALSE;
	json_object_set_int_member(spec, "company_id", id);
	return TRUE;
}
