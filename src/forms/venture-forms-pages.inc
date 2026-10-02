/* SPDX-License-Identifier: AGPL-3.0-or-later */

static gboolean
forms_validate_draft(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	(void)database; (void)previous; (void)data;
	if (g_object_get_data(G_OBJECT(entity), VENTURE_FORMS_DRAFT_WRITE) == NULL)
	{
		venture_set_error_validation(error, "Draft", "only the public form service may write intermediate answers");
		return FALSE;
	}
	return TRUE;
}

void
venture_forms_step_free(VentureFormsStep *step)
{
	if (step == NULL) return;
	g_clear_object(&step->version);
	g_clear_pointer(&step->values, json_object_unref);
	g_clear_pointer(&step->errors, json_object_unref);
	g_free(step->token); g_free(step->ticket); g_free(step);
}

static gchar *
forms_draft_token(VentureEntity *form, VentureEntity *draft)
{
	g_autofree gchar *key = venture_forms_get_string(form, "ticket-key");
	g_autoptr(GDateTime) expires = NULL;
	g_autofree gchar *payload = NULL, *mac = NULL;
	g_object_get(draft, "expires-at", &expires, NULL);
	if (expires == NULL || venture_string_is_empty(key)) return NULL;
	payload = g_strdup_printf("%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
		venture_entity_get_uuid(draft), venture_forms_get_int(draft, "generation"),
		venture_entity_get_id(form), venture_forms_get_int(draft, "version-id"), g_date_time_to_unix(expires));
	mac = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), payload, -1);
	return g_strdup_printf("%s.%s", venture_entity_get_uuid(draft), mac);
}

static VentureEntity *
forms_draft_find(VentureDatabase *database, VentureEntity *form, const gchar *token, GDateTime *now)
{
	g_auto(GStrv) parts = g_strsplit(token, ".", 3);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_DRAFT_RECORD);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *expected = NULL;
	g_autoptr(GDateTime) expires = NULL;
	VentureEntity *draft;
	if (g_strv_length(parts) != 2 || !g_uuid_string_is_valid(parts[0])) return NULL;
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "uuid", VENTURE_FILTER_OP_EQ, parts[0], NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(database, query, NULL);
	if (rows == NULL || rows->len == 0) return NULL;
	draft = g_ptr_array_index(rows, 0);
	g_object_get(draft, "expires-at", &expires, NULL);
	if (expires == NULL || g_date_time_compare(now, expires) >= 0 || venture_forms_get_bool(draft, "finishing")) return NULL;
	expected = forms_draft_token(form, draft);
	if (expected == NULL || !venture_constant_time_equal(expected, token)) return NULL;
	return g_object_ref(draft);
}

static gboolean
forms_draft_save(VentureDatabase *database, VentureEntity *draft, VentureFormsStep *step,
	gboolean finishing, GError **error)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *text = NULL;
	json_node_set_object(node, step->values);
	text = json_to_string(node, FALSE);
	if (strlen(text) > VENTURE_FORMS_MAX_BODY)
	{
		venture_set_error_validation(error, "Draft", "too much data for one response");
		return FALSE;
	}
	g_object_set(draft, "answers", text, "page", (gint64)step->page, "finishing", finishing,
		"generation", venture_forms_get_int(draft, "generation") + 1, NULL);
	g_object_set_data(G_OBJECT(draft), VENTURE_FORMS_DRAFT_WRITE, GINT_TO_POINTER(1));
	return venture_database_save(database, draft, NULL, error);
}

VentureFormsStep *
venture_forms_step(VentureDatabase *database, VentureEntity *form, GHashTable *answers,
	const gchar *origin, GDateTime *now, GError **error)
{
	g_autoptr(VentureFormsStep) step = g_new0(VentureFormsStep, 1);
	g_autoptr(VentureEntity) draft = NULL, fresh = NULL;
	g_autoptr(GPtrArray) fields = NULL, page_fields = NULL, base_fields = NULL;
	g_autoptr(JsonObject) page_values = NULL;
	g_autoptr(JsonObject) posted = venture_forms_answers_to_json(answers);
	g_autoptr(JsonObject) checked = json_object_new();
	g_autoptr(GString) summary = g_string_new(NULL);
	const gchar *token = forms_first(answers, VENTURE_FORMS_DRAFT_TOKEN);
	const gchar *move = forms_first(answers, VENTURE_FORMS_MOVE);
	gboolean back = g_strcmp0(move, "back") == 0;
	gboolean finishing = FALSE, row_operation = FALSE;
	g_autoptr(GHashTable) not_shown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GHashTableIter iter;
	gpointer key;
	guint i;

	step->errors = json_object_new();
	step->values = json_object_new();
	if (!venture_database_begin(database, error)) return NULL;
	fresh = venture_database_get(database, VENTURE_TYPE_FORM, venture_entity_get_id(form), error);
	if (fresh == NULL) goto fail;
	if (!forms_is_open(database, fresh, now)) goto missing;
	if (g_hash_table_contains(answers, VENTURE_FORMS_DRAFT_TOKEN))
	{
		g_autofree gchar *text = NULL;
		g_autoptr(JsonParser) parser = json_parser_new();
		if (venture_string_is_empty(token)) goto missing;
		draft = forms_draft_find(database, fresh, token, now);
		if (draft == NULL) goto missing;
		step->version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION,
			venture_forms_get_int(draft, "version-id"), error);
		step->page = (guint)venture_forms_get_int(draft, "page");
		step->ticket = venture_forms_get_string(draft, "ticket");
		text = venture_forms_get_string(draft, "answers");
		if (!json_parser_load_from_data(parser, text, -1, error)) goto fail;
		if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) goto missing;
		g_clear_pointer(&step->values, json_object_unref);
		step->values = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
	}
	else
	{
		step->version = venture_forms_version_for_answers(database, fresh, answers);
		step->ticket = g_strdup(forms_first(answers, VENTURE_FORMS_TICKET));
	}
	if (step->version == NULL) goto missing;
	base_fields = venture_forms_definition_for(database, fresh, step->version, error);
	if (base_fields == NULL) goto fail;
	step->pages = venture_forms_page_count(base_fields);
	if (step->page >= step->pages) goto missing;
	page_values = venture_forms_groups_overlay(base_fields, step->page, step->values, posted);
	fields = venture_forms_expand_groups(base_fields, page_values, FALSE, step->errors);
	page_fields = venture_forms_page_fields(fields, step->page);
	g_hash_table_iter_init(&iter, answers);
	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		if (g_strcmp0(key, VENTURE_FORMS_DRAFT_TOKEN) == 0 || g_strcmp0(key, VENTURE_FORMS_MOVE) == 0 ||
		    g_strcmp0(key, VENTURE_FORMS_TICKET) == 0 || g_strcmp0(key, VENTURE_FORMS_HONEYPOT) == 0) continue;
		if (venture_forms_definition_find(page_fields, key) == NULL)
			forms_refuse(step->errors, "_form", "Only questions on the current page may be answered.");
	}
	venture_forms_groups_clear_page(base_fields, step->page, step->values);
	for (i = 0; i < page_fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(page_fields, i);
		JsonNode *value = json_object_get_member(posted, field->key);
		if (field->group_boundary != VENTURE_FORMS_GROUP_NONE && field->group_boundary != VENTURE_FORMS_GROUP_ROW_START) continue;

		json_object_remove_member(step->values, field->key);
		if (value != NULL && field->kind != VENTURE_FORM_FIELD_PAGE_BREAK)
			json_object_set_member(step->values, field->key, json_node_copy(value));
	}
	row_operation = venture_forms_group_move(page_fields, move, step->values, step->errors);
	venture_forms_rules_filter(fields, step->values, not_shown);
	if (!back && !row_operation) venture_forms_groups_validate(page_fields, not_shown, step->errors);
	for (i = 0; !back && !row_operation && i < page_fields->len; i++)
	{
		VentureFormsField *source = g_ptr_array_index(page_fields, i);
		VentureFormsField effective = *source;
		if (g_hash_table_contains(not_shown, source->key)) continue;
		venture_forms_field_active(source, step->values, &effective.required);
		forms_check_field(&effective, g_hash_table_lookup(answers, source->key), checked, step->errors, summary);
	}
	if (!row_operation && json_object_get_size(step->errors) == 0)
	{
		if (back && step->page > 0)
		{
			guint cursor = 0, previous = 0;
			while (cursor < step->page)
			{
				previous = cursor;
				cursor = venture_forms_next_page(fields, cursor, step->values);
			}
			step->page = previous;
		}
		else if (!back)
		{
			guint next = venture_forms_next_page(fields, step->page, step->values);
			if (next < step->pages) step->page = next;
			else finishing = TRUE;
		}
	}
	if (draft == NULL && json_object_get_size(step->errors) == 0)
	{
		gint64 minutes = venture_forms_get_int(fresh, "draft-minutes");
		g_autoptr(GDateTime) expires = g_date_time_add_minutes(now, minutes > 0 ? minutes : 60);
		draft = VENTURE_ENTITY(venture_form_draft_new());
		venture_entity_set_organization_id(draft, venture_entity_get_organization_id(fresh));
		g_object_set(draft, "name", "Unsubmitted form", "form-id", venture_entity_get_id(fresh),
			"version-id", venture_entity_get_id(step->version), "expires-at", expires, "ticket", step->ticket, NULL);
	}
	if (draft != NULL && !forms_draft_save(database, draft, step, finishing, error)) goto fail;
	if (!venture_database_commit(database, error)) return NULL;

	/* Claim the draft before calling final intake: its follow-up retry owns its
	 * transactions. A stale or concurrent token cannot submit it twice, even
	 * when a process dies after saving the response but before draft cleanup. */
	if (finishing)
	{
		g_autoptr(GHashTable) complete = venture_forms_answers_from_json(step->values, error);
		g_autoptr(JsonObject) refused = NULL;
		VentureFormsOutcome outcome;
		if (complete == NULL) return NULL;
		venture_forms_answers_add(complete, VENTURE_FORMS_TICKET, step->ticket);
		if (forms_first(answers, VENTURE_FORMS_HONEYPOT) != NULL)
			venture_forms_answers_add(complete, VENTURE_FORMS_HONEYPOT, forms_first(answers, VENTURE_FORMS_HONEYPOT));
		if (!venture_forms_screen(fresh, complete, now))
		{
			step->complete = TRUE;
			if (!venture_database_purge(database, draft, NULL, error)) return NULL;
			return g_steal_pointer(&step);
		}
		if (!venture_forms_submit(database, fresh, complete, origin, now, &outcome, NULL, &refused, error))
			return NULL;
		if (outcome == VENTURE_FORMS_ACCEPTED)
		{
			step->complete = TRUE;
			if (!venture_database_purge(database, draft, NULL, error)) return NULL;
			return g_steal_pointer(&step);
		}
		g_clear_pointer(&step->errors, json_object_unref);
		step->errors = refused ? g_steal_pointer(&refused) : json_object_new();
		for (i = 0; i < step->pages; i++)
		{
			g_autoptr(GPtrArray) candidate = venture_forms_page_fields(fields, i);
			guint j;
			gboolean found = FALSE;
			for (j = 0; j < candidate->len; j++)
				if (json_object_has_member(step->errors, ((VentureFormsField *)g_ptr_array_index(candidate, j))->key)) found = TRUE;
			if (found) { step->page = i; break; }
		}
		if (!forms_draft_save(database, draft, step, FALSE, error)) return NULL;
	}
	if (draft != NULL) step->token = forms_draft_token(fresh, draft);
	return g_steal_pointer(&step);

missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
fail:
	venture_database_rollback(database);
	return NULL;
}
