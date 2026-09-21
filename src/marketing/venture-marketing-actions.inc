/* SPDX-License-Identifier: AGPL-3.0-or-later */
static const gchar *parameter(GHashTable *params, const gchar *name)
{
	JsonNode *node = g_hash_table_lookup(params, name);
	return node && !JSON_NODE_HOLDS_NULL(node) ? json_node_get_string(node) : NULL;
}
static gboolean action_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	return allowed(venture_action_get_data(action), venture_entity_get_organization_id(entity), error);
}
static VentureEntity *invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureMarketingService *self = venture_action_get_data(action);
	g_autofree gchar *name = NULL;
	g_autoptr(VentureContext) context = g_weak_ref_get(&self->context);
	g_autoptr(GDateTime) now = NULL;
	gint64 org = venture_entity_get_organization_id(entity), id = venture_entity_get_id(entity);
	g_object_get(action, "name", &name, NULL);
	if (!strcmp(name, "consent_marketing") || !strcmp(name, "feedback")) {
		now = venture_time_from_string(parameter(params, "occurred_at"), error);
		if (!now) return NULL;
		if (!strcmp(name, "consent_marketing"))
			return VENTURE_ENTITY(venture_marketing_service_consent(self, org, entity,
				parameter(params, "source"), parameter(params, "evidence"), parameter(params, "evidence_key"), now, actor, error));
		if (!venture_marketing_service_feedback(self, org, id, parameter(params, "kind"),
			parameter(params, "source"), parameter(params, "event_key"), now, actor, error)) return NULL;
		return get_row(self, VENTURE_TYPE_MARKETING_RECIPIENT, org, id, error);
	}
	now = venture_time_now();
	if (!strcmp(name, "withdraw")) {
		g_autofree gchar *email = string(entity, "email");
		if (!venture_database_begin(self->database, error)) return NULL;
		if (!withdraw(self, org, email, 0, "Operator-recorded marketing withdrawal", now, actor, error)) {
			venture_database_rollback(self->database); return NULL;
		}
		if (!venture_database_commit(self->database, error)) return NULL;
		return get_row(self, VENTURE_TYPE_MARKETING_CONSENT, org, id, error);
	}
	if (!strcmp(name, "preview")) {
		g_autofree gchar *base = NULL;
		if (context) g_object_get(venture_context_get_config(context), "server-base-url", &base, NULL);
		return VENTURE_ENTITY(venture_marketing_service_preview(self, org, id, base, actor, error));
	}
	if (!strcmp(name, "run")) {
		JsonNode *value = g_hash_table_lookup(params, "limit");
		gint64 limit = value && !JSON_NODE_HOLDS_NULL(value) ? json_node_get_int(value) : 100;
		g_autoptr(VentureEntity) send = NULL;
		VentureMailOutbox *outbox = context ? venture_context_get_mail_outbox(context) : NULL;
		if (!outbox) { refuse(error, "Configure organization mail before executing a campaign"); return NULL; }
		if (venture_marketing_service_run(self, org, id, limit, now, actor, error) < 0) return NULL;
		send = get_row(self, VENTURE_TYPE_MARKETING_SEND, org, id, error);
		if (!send) return NULL;
		if (number(send, "last-mail-id") && venture_mail_outbox_deliver_one(outbox, org, number(send, "last-mail-id"), 0, 0, NULL, error) < 0) return NULL;
		return g_steal_pointer(&send);
	}
	if (!venture_marketing_service_transition(self, org, id, name, actor, error)) return NULL;
	return get_row(self, VENTURE_TYPE_MARKETING_SEND, org, id, error);
}
static void add_parameter(GPtrArray *parameters, const gchar *name, const gchar *label, VentureFieldKind kind, gboolean required)
{
	VentureFieldSpec *field = venture_field_spec_new(name, label, kind);
	field->required = required;
	if (kind == VENTURE_FIELD_KIND_TEXT) field->max_length = 8192;
	else if (kind == VENTURE_FIELD_KIND_STRING) field->max_length = 128;
	if (!strcmp(name, "limit")) { field->has_min = TRUE; field->min_value = 1; field->has_max = TRUE; field->max_value = 100; }
	if (!strcmp(name, "kind")) {
		field->choices = g_strsplit("hard_bounce,temporary_bounce,complaint", ",", -1);
		field->choice_labels = g_strsplit("Hard bounce,Temporary bounce,Complaint", ",", -1);
	}
	g_ptr_array_add(parameters, field);
}
static void register_actions(VentureMarketingService *self)
{
	static const struct { const gchar *type; const gchar *name; const gchar *label; const gchar *description; } definitions[] = {
		{ "contact", "consent_marketing", "Record marketing permission", "Capture explicit permission for this record's current address" },
		{ "company", "consent_marketing", "Record marketing permission", "Capture explicit permission for this company's primary address" },
		{ "lead", "consent_marketing", "Record marketing permission", "Capture explicit permission for this lead's current address" },
		{ "marketing_consent", "withdraw", "Withdraw marketing permission", "Suppress this address and cancel queued outreach; transactional mail remains independent" },
		{ "marketing_send", "preview", "Preview and freeze audience", "Retain deduplicated recipients, exclusions and exact rendered content before approval" },
		{ "marketing_send", "approve", "Approve frozen send", "Authorize the reviewed immutable recipient snapshot" },
		{ "marketing_send", "pause", "Pause delivery", "Keep queued identities and retries until resumed" },
		{ "marketing_send", "resume", "Resume delivery", "Continue the same approved audience and delivery identities" },
		{ "marketing_send", "cancel", "Cancel pending delivery", "Stop future work; sending and uncertain SMTP outcomes remain retained" },
		{ "marketing_send", "run", "Execute bounded delivery", "Queue at most one due recipient and attempt its retained outbox identity" },
		{ "marketing_recipient", "feedback", "Record delivery feedback", "Retain operator-verified bounce or complaint evidence; temporary failures never imply hard bounce" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(definitions); i++) {
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;
		if (i < 3 || i == 10) {
			add_parameter(parameters, "source", "Permission or feedback source", VENTURE_FIELD_KIND_TEXT, TRUE);
			add_parameter(parameters, "occurred_at", "Actual evidence time", VENTURE_FIELD_KIND_DATETIME, TRUE);
			if (i < 3) {
				add_parameter(parameters, "evidence", "Permission statement or evidence reference", VENTURE_FIELD_KIND_TEXT, TRUE);
				add_parameter(parameters, "evidence_key", "Replay identity", VENTURE_FIELD_KIND_STRING, TRUE);
			} else {
				add_parameter(parameters, "kind", "Feedback kind", VENTURE_FIELD_KIND_ENUM, TRUE);
				add_parameter(parameters, "event_key", "Feedback replay identity", VENTURE_FIELD_KIND_STRING, TRUE);
			}
		}
		if (i == 9) add_parameter(parameters, "limit", "Maximum recipients examined", VENTURE_FIELD_KIND_INTEGER, FALSE);
		action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", definitions[i].type, "name", definitions[i].name,
			"label", definitions[i].label, "description", definitions[i].description, "parameters", parameters,
			"stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(self->database), action,
			action_allowed, invoke, self, NULL, &error)) g_error("Marketing action registration: %s", error->message);
	}
}
