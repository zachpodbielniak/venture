/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The Lightsite owner read: which business a Lightsite sign-up made for an
 * identity-provider subject, and the owner's names and address. Lightsite's
 * daemon asks it after Keycloak signs a person in, so it can show them their
 * own business without keeping a copy of the account.
 *
 * - the authority is the sign-up's own: venture_tenant_service_check_trusted_service();
 * - the answer comes from the sign-up receipt, matched on issuer and subject
 *   exactly, never from an email or a provider claim;
 * - the names are the ones the receipt kept, not the user's display name,
 *   which is joined for a new owner and left alone for a linked member;
 * - the business name and the email are read now, so a rename shows.
 */

JsonNode *
venture_tenant_service_lightsite_owner(VentureTenantService *self,
	const gchar *issuer, const gchar *subject, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) receipt = NULL, organization = NULL, user = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *business_name = NULL, *first = NULL, *last = NULL, *email = NULL;
	JsonObject *object;
	JsonNode *node;
	gint64 organization_id = 0, user_id = 0;
	g_return_val_if_fail(VENTURE_IS_TENANT_SERVICE(self), NULL);
	/* Own prefix: this is a read, and "Lightsite sign-up" would send an
	 * operator looking at the wrong route. */
	if (!signup_issuer_valid(issuer)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Lightsite owner: issuer must be an https URL without credentials, query or fragment");
		return NULL;
	}
	if (!signup_text_valid(subject, 255)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Lightsite owner: subject must be 1-255 characters without control characters");
		return NULL;
	}
	if (!venture_tenant_service_check_trusted_service(self, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	query = venture_query_new(VENTURE_TYPE_TENANT_SIGNUP);
	venture_query_add_filter_string(query, "issuer", VENTURE_FILTER_OP_EQ, issuer, NULL);
	venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ, subject, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 1);
	receipt = venture_database_find_one(self->database, query, &local);
	if (local) {
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (receipt) g_object_get(receipt, "business-id", &organization_id, "user-id", &user_id,
		"first-name", &first, "last-name", &last, NULL);
	/* No receipt, or a business or owner removed since, is the same 404. */
	if (organization_id > 0) organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);
	if (user_id > 0) user = venture_database_get(self->database, VENTURE_TYPE_USER, user_id, NULL);
	if (!organization || venture_entity_is_deleted(organization) || !user || venture_entity_is_deleted(user)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No Lightsite sign-up for this identity");
		return NULL;
	}
	g_object_get(organization, "name", &business_name, NULL);
	g_object_get(user, "email", &email, NULL);
	object = json_object_new();
	json_object_set_int_member(object, "organization_id", organization_id);
	json_object_set_string_member(object, "business_name", business_name ? business_name : "");
	json_object_set_string_member(object, "first_name", first ? first : "");
	json_object_set_string_member(object, "last_name", last ? last : "");
	json_object_set_string_member(object, "email", email ? email : "");
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, object);
	return node;
}
