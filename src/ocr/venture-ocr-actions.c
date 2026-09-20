/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const gchar *string_param(GHashTable *params, const gchar *name)
{ JsonNode *node = g_hash_table_lookup(params, name); return node != NULL && !JSON_NODE_HOLDS_NULL(node) ? json_node_get_string(node) : NULL; }
static gint64 int_param(GHashTable *params, const gchar *name, gint64 fallback)
{ JsonNode *node = g_hash_table_lookup(params, name); return node != NULL && !JSON_NODE_HOLDS_NULL(node) ? json_node_get_int(node) : fallback; }
static gboolean bool_param(GHashTable *params, const gchar *name)
{ JsonNode *node = g_hash_table_lookup(params, name); return node != NULL && !JSON_NODE_HOLDS_NULL(node) && json_node_get_boolean(node); }
static gboolean allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action; (void)entity; (void)actor;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "ocr_job") == G_TYPE_INVALID) {
		venture_set_error_validation(error, "module", "The OCR module is disabled"); return FALSE;
	}
	return TRUE;
}
static VentureEntity *invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	VentureOcrService *service = venture_ocr_service_get(database);
	g_autofree gchar *name = NULL;
	g_object_get(action, "name", &name, NULL);
	if (g_str_equal(name, "ocr_extract")) {
		g_autoptr(VentureEntity) document = NULL;
		if (VENTURE_IS_CAPTURE_ITEM(entity)) {
			gint64 id;
			g_object_get(entity, "document-id", &id, NULL);
			document = venture_database_get(database, VENTURE_TYPE_DOCUMENT, id, error);
			if (document == NULL) return NULL;
			if (venture_entity_get_organization_id(document) != venture_entity_get_organization_id(entity)) {
				venture_set_error_validation(error, "document", "Document belongs to another organization"); return NULL;
			}
		} else document = g_object_ref(entity);
		return venture_ocr_service_queue(service, document, string_param(params, "language"), bool_param(params, "force"), actor, error);
	}
	if (g_str_equal(name, "ocr_review")) {
		g_autoptr(VentureEntity) job = venture_database_get(database, VENTURE_TYPE_OCR_JOB, int_param(params, "job_id", 0), error);
		gint64 document_id;
		if (job == NULL) return NULL;
		g_object_get(job, "document-id", &document_id, NULL);
		if (document_id != venture_entity_get_id(entity) || venture_entity_get_organization_id(job) != venture_entity_get_organization_id(entity)) {
			venture_set_error_validation(error, "job_id", "OCR job does not belong to this document"); return NULL;
		}
		/* The document is the action subject, so staged approval also
		 * freezes the version of the text being replaced. */
		return venture_ocr_service_control(service, job, "review", string_param(params, "text"), actor, error);
	}
	if (g_str_equal(name, "ocr_extract_all")) {
		if (!venture_action_require_organization(action, entity, database, error)) return NULL;
		return venture_ocr_service_batch(service, venture_entity_get_organization_id(entity), int_param(params, "after_id", 0),
			(guint)int_param(params, "limit", VENTURE_OCR_MAX_BATCH), string_param(params, "language"), bool_param(params, "force"), actor, error);
	}
	if (VENTURE_IS_OCR_BATCH(entity)) return venture_ocr_service_batch_step(service, entity, g_str_equal(name, "cancel"), actor, error);
	if (g_str_equal(name, "step")) return venture_ocr_service_step(service, entity, NULL, actor, error);
	return venture_ocr_service_control(service, entity, name, string_param(params, "text"), actor, error);
}
static void register_action(VentureDatabase *database, const gchar *type, const gchar *name, const gchar *label,
	gboolean type_level, GPtrArray *parameters)
{
	g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", type, "name", name,
		"label", label, "description", label, "parameters", parameters, "stageable", TRUE,
		"type-level", type_level, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(GError) error = NULL;
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action, allowed, invoke, database, NULL, &error))
		g_error("OCR action registration: %s", error->message);
}
void venture_ocr_actions_register(VentureDatabase *database)
{
	g_autoptr(GPtrArray) params = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(GPtrArray) empty = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	VentureFieldSpec *field;
	venture_ocr_service_get(database);
	field = venture_field_spec_new("language", "Language", VENTURE_FIELD_KIND_STRING); field->max_length = 128;
	field->pattern = g_strdup("^[A-Za-z0-9_]+(\\+[A-Za-z0-9_]+)*$"); g_ptr_array_add(params, field);
	g_ptr_array_add(params, venture_field_spec_new("force", "Force unchanged source", VENTURE_FIELD_KIND_BOOLEAN));
	register_action(database, "document", "ocr_extract", "Extract text locally", FALSE, params);
	register_action(database, "capture_item", "ocr_extract", "Extract text locally", FALSE, params);
	field = venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER); field->required = TRUE; field->has_min = TRUE; field->min_value = 1; g_ptr_array_add(params, field);
	field = venture_field_spec_new("after_id", "After capture identity", VENTURE_FIELD_KIND_INTEGER); field->has_min = TRUE; field->min_value = 0; g_ptr_array_add(params, field);
	field = venture_field_spec_new("limit", "Documents", VENTURE_FIELD_KIND_INTEGER); field->has_min = field->has_max = TRUE; field->min_value = 1; field->max_value = VENTURE_OCR_MAX_BATCH; g_ptr_array_add(params, field);
	register_action(database, "capture_item", "ocr_extract_all", "Extract inbox text locally", TRUE, params);
	register_action(database, "ocr_job", "step", "Extract next page", FALSE, empty);
	register_action(database, "ocr_job", "retry", "Retry extraction", FALSE, empty);
	register_action(database, "ocr_job", "cancel", "Cancel extraction", FALSE, empty);
	register_action(database, "ocr_batch", "step", "Continue extraction batch", FALSE, empty);
	register_action(database, "ocr_batch", "cancel", "Cancel extraction batch", FALSE, empty);
	g_ptr_array_set_size(params, 0);
	field = venture_field_spec_new("text", "Reviewed text", VENTURE_FIELD_KIND_TEXT); field->max_length = VENTURE_OCR_MAX_TEXT; g_ptr_array_add(params, field);
	field = venture_field_spec_new("job_id", "OCR job", VENTURE_FIELD_KIND_REFERENCE); field->reference_type = g_strdup("ocr_job"); field->required = TRUE; g_ptr_array_add(params, field);
	register_action(database, "document", "ocr_review", "Apply reviewed OCR text", FALSE, params);
}
