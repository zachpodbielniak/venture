/* SPDX-License-Identifier: AGPL-3.0-or-later */

#define CRM_ATTACHMENT_MAX_BYTES (8 * 1024 * 1024)

static gboolean
track_auxiliary(Run *run, const gchar *object, const gchar *source_id,
	VentureEntity *record, GError **error)
{
	g_autoptr(VentureCrmImportRow) tracking = venture_crm_import_row_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(tracking), run->org);
	g_object_set(tracking, "import-id", venture_entity_get_id(run->batch), "source", run->source,
		"source-object", object, "source-id", source_id, "status", "imported", "created", TRUE,
		"record-type", venture_entity_get_entity_name(record), "record-id", venture_entity_get_id(record), NULL);
	return save_owned(run->self, VENTURE_ENTITY(tracking), run->actor, error);
}

/* Decode strictly: GLib's permissive decoder otherwise silently drops
 * invalid characters, turning a corrupt export into a different file. */
static GBytes *
attachment_bytes(const gchar *encoded, GError **error)
{
	g_autofree guchar *decoded = NULL;
	g_autofree gchar *canonical = NULL;
	gsize length = 0;
	if (encoded == NULL || strlen(encoded) > ((CRM_ATTACHMENT_MAX_BYTES + 2) / 3) * 4)
	{
		refuse(error, "attachment content is missing or exceeds 8 MiB");
		return NULL;
	}
	decoded = g_base64_decode(encoded, &length);
	canonical = g_base64_encode(decoded, length);
	if (length > CRM_ATTACHMENT_MAX_BYTES || !g_str_equal(encoded, canonical))
	{
		refuse(error, "attachment content must be canonical base64, at most 8 MiB");
		return NULL;
	}
	return g_bytes_new_take(g_steal_pointer(&decoded), length);
}

static gboolean
import_attachments(Run *run, Table *table, GError **error)
{
	guint r;
	for (r = 1; r < table->rows->len; r++)
	{
		gchar **row = g_ptr_array_index(table->rows, r);
		const gchar *id = cell(table, row, "id");
		const gchar *encoded = cell(table, row, "content_base64");
		const gchar *parent_object = cell(table, row, "parent_object");
		const gchar *parent_id = cell(table, row, "parent_id");
		const gchar *name = cell(table, row, "name");
		const gchar *mime = cell(table, row, "mime_type");
		JsonObject *payloads = obj_obj(run->manifest, "attachment_data");
		VentureEntity *tracking = blank(id) ? NULL : run_row(run, table->object, id);
		g_autoptr(VentureDocument) document = NULL;
		g_autoptr(VentureRecordLink) link = NULL;
		g_autoptr(GBytes) bytes = NULL;
		g_autofree gchar *parent_type = NULL;
		g_autofree gchar *checksum = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *url = NULL;
		gint64 earlier, parent;
		if (tracking == NULL || !row_pending(tracking)) continue;
		earlier = imported_record(run, "attachment", id, NULL, error);
		if (earlier < 0) return FALSE;
		if (earlier > 0)
		{
			if (!finish_row(run, tracking, "matched", NULL, "document", earlier, FALSE, error))
				return FALSE;
			continue;
		}
		if (blank(encoded) && payloads != NULL)
		{
			const gchar *payload = obj_str(payloads, id);
			if (payload != NULL) encoded = payload;
		}
		bytes = attachment_bytes(encoded, error);
		if (bytes == NULL) return FALSE;
		if (blank(parent_object) || blank(parent_id) || blank(name))
			return refuse(error, "attachment %s needs name, parent_object and parent_id", id);
		parent = imported_record(run, parent_object, parent_id, &parent_type, error);
		if (parent < 0) return FALSE;
		if (parent == 0 || blank(parent_type))
			return refuse(error, "attachment %s: parent %s %s is unresolved", id, parent_object, parent_id);
		checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
		text = venture_document_extract_text(mime, name, bytes);
		document = venture_document_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(document), run->org);
		g_object_set(document, "title", name, "kind", "crm-import", "mime-type", mime,
			"content-base64", encoded, "size-bytes", (gint64)g_bytes_get_size(bytes),
			"hash", checksum, "extracted-text", text, NULL);
		if (!apply_custom_fields(run, table, row, VENTURE_ENTITY(document), error) ||
			!venture_database_save(run->self->database, VENTURE_ENTITY(document), run->actor, error))
			return FALSE;
		url = g_strdup_printf("/api/v1/crm_imports/attachments/%" G_GINT64_FORMAT,
			venture_entity_get_id(VENTURE_ENTITY(document)));
		g_object_set(document, "url", url, NULL);
		if (!venture_database_save(run->self->database, VENTURE_ENTITY(document), run->actor, error))
			return FALSE;
		link = venture_record_link_create(run->self->database, "document", venture_entity_get_id(VENTURE_ENTITY(document)),
			VENTURE_LINK_KIND_RELATED, parent_type, parent, "Imported attachment", error);
		if (link == NULL || !venture_database_save(run->self->database, VENTURE_ENTITY(link), run->actor, error) ||
			!track_auxiliary(run, "attachment_link", id, VENTURE_ENTITY(link), error) ||
			!finish_row(run, tracking, "imported", NULL, "document", venture_entity_get_id(VENTURE_ENTITY(document)), TRUE, error))
			return FALSE;
	}
	return TRUE;
}
