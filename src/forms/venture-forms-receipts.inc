/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define FORMS_RECEIPT_WRITE "venture-forms-receipt-write"
#define FORMS_RECEIPT_NONCE "venture-forms-receipt-nonce"
#define FORMS_RECEIPT_DIGEST "venture-forms-receipt-digest"

/* Length-independent canonical JSON includes stable field/value ordering.
 * Keyed hashes prevent a leaked digest from becoming an email dictionary. */
gchar *venture_forms_request_digest(VentureEntity *form, GHashTable *answers, GPtrArray *parts)
{
	g_autoptr(JsonObject) object = json_object_new();
	g_autoptr(JsonObject) values = json_object_new();
	GHashTableIter iter;
	gpointer name, entry;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonArray) files = json_array_new();
	g_autoptr(GString) canonical = g_string_new(NULL);
	g_autofree gchar *key = venture_forms_get_string(form, "ticket-key");
	guint i;
	g_hash_table_iter_init(&iter, answers);
	while (g_hash_table_iter_next(&iter, &name, &entry))
	{
		GPtrArray *strings = entry;
		JsonArray *array = json_array_new();
		for (i = 0; i < strings->len; i++) json_array_add_string_element(array, g_ptr_array_index(strings, i));
		json_object_set_array_member(values, name, array);
	}
	json_object_set_object_member(object, "answers", g_steal_pointer(&values));
	if (parts != NULL) for (i = 0; i < parts->len; i++)
	{
		VentureFormsUploadPart *part = g_ptr_array_index(parts, i);
		g_autoptr(JsonObject) file = json_object_new();
		g_autofree gchar *hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, part->bytes);
		json_object_set_string_member(file, "field", part->key);
		json_object_set_string_member(file, "name", part->filename);
		json_object_set_string_member(file, "bytes", hash);
		json_array_add_object_element(files, g_steal_pointer(&file));
	}
	json_object_set_array_member(object, "files", g_steal_pointer(&files));
	json_node_set_object(node, object);
	venture_json_append_canonical(canonical, node);
	return g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), canonical->str, canonical->len);
}

static VentureEntity *forms_receipt_find(VentureDatabase *database, VentureEntity *form,
	const gchar *nonce, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_RECEIPT);
	g_autofree gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, nonce, -1);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "nonce-hash", VENTURE_FILTER_OP_EQ, hash, NULL);
	return venture_database_find_one(database, query, error);
}

/* -1 refuses; 0 is new/legacy; 1 returns the previously accepted response.
 * The caller still enforces the public origin and module boundary. */
gint venture_forms_receipt_replay(VentureDatabase *database, VentureEntity *form,
	GHashTable *answers, const gchar *digest, gboolean wire, VentureEntity **response, GError **error)
{
	const gchar *nonce = forms_first(answers, VENTURE_FORMS_PAYMENT_NONCE);
	g_autoptr(VentureEntity) receipt = NULL, found = NULL;
	g_autofree gchar *expected = NULL;
	g_autoptr(GError) local = NULL;
	if (nonce == NULL) return 0;
	if (((GPtrArray *)g_hash_table_lookup(answers, VENTURE_FORMS_PAYMENT_NONCE))->len != 1 ||
	    !venture_forms_payment_nonce_valid(form, nonce))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
		return -1;
	}
	receipt = forms_receipt_find(database, form, nonce, &local);
	if (local != NULL) { g_propagate_error(error, g_steal_pointer(&local)); return -1; }
	if (receipt == NULL) return 0;
	if (venture_forms_get_int(receipt, "response-id") <= 0) goto missing;
	expected = venture_forms_get_string(receipt, wire ? "request-hash" : "answers-hash");
	if (g_strcmp0(expected, digest) != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			"This submission identity was already used with different answers. Start a new form.");
		return -1;
	}
	found = venture_database_get(database, VENTURE_TYPE_FORM_SUBMISSION,
		venture_forms_get_int(receipt, "response-id"), &local);
	if (found == NULL)
	{
		if (local != NULL && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{ g_propagate_error(error, g_steal_pointer(&local)); return -1; }
		goto missing;
	}
	if (venture_entity_is_deleted(found) || venture_entity_get_organization_id(found) != venture_entity_get_organization_id(form)) goto missing;
	if (response != NULL) *response = g_steal_pointer(&found);
	return 1;
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
	return -1;
}

static gboolean forms_receipt_reserve(VentureDatabase *database, VentureEntity *form,
	VentureEntity *response, VentureEntity **reserved, GError **error)
{
	const gchar *nonce = g_object_get_data(G_OBJECT(response), FORMS_RECEIPT_NONCE);
	const gchar *digest = g_object_get_data(G_OBJECT(response), FORMS_RECEIPT_DIGEST);
	g_autofree gchar *hash = NULL;
	g_autoptr(VentureEntity) receipt = NULL;
	if (nonce == NULL) return TRUE;
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, nonce, -1);
	receipt = g_object_new(VENTURE_TYPE_FORM_RECEIPT, "organization-id", venture_entity_get_organization_id(form),
		"name", "Accepted form operation", "form-id", venture_entity_get_id(form), "nonce-hash", hash,
		"answers-hash", digest, "request-hash", g_object_get_data(G_OBJECT(form), VENTURE_FORMS_REQUEST_HASH) != NULL ?
			g_object_get_data(G_OBJECT(form), VENTURE_FORMS_REQUEST_HASH) : digest,
		"response-id", (gint64)0, NULL);
	g_object_set_data(G_OBJECT(receipt), FORMS_RECEIPT_WRITE, GINT_TO_POINTER(1));
	if (!venture_database_save(database, receipt, NULL, error)) return FALSE;
	*reserved = g_steal_pointer(&receipt);
	return TRUE;
}

static gboolean forms_receipt_forget(VentureDatabase *database, VentureEntity *response, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_RECEIPT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(response));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "response-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(response), NULL);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *receipt = g_ptr_array_index(rows, i);
		g_object_set_data(G_OBJECT(receipt), FORMS_RECEIPT_WRITE, GINT_TO_POINTER(1));
		g_object_set(receipt, "response-id", (gint64)0, "answers-hash", NULL, "request-hash", NULL, NULL);
		if (!venture_database_save(database, receipt, NULL, error)) return FALSE;
	}
	return TRUE;
}
