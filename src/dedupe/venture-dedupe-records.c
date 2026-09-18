/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/* A candidate names its two records by kind and id rather than by two
 * reference fields, because one row serves companies and contacts alike; the
 * labels are a snapshot for the list so the page needs no second query per
 * row. Everything here is written by VentureDedupeService, never by hand. */
static const VentureFieldDecl candidate_fields[] = {
	VENTURE_FIELD("kind", "Kind", "company or contact", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("record-a", "Record A", "The lower id of the pair", VENTURE_FIELD_KIND_INTEGER,
		VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("record-b", "Record B", "The higher id of the pair", VENTURE_FIELD_KIND_INTEGER,
		VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("label-a", "Record A name", "Display name at scan time", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("label-b", "Record B name", "Display name at scan time", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("score", "Score", "0-100; 100 is an exact identifier match", VENTURE_FIELD_KIND_INTEGER,
		VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("reasons", "Reasons", "JSON array of email, phone, website, domain+name, name",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("status", "Status", "open, merged or dismissed", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("pair-key", "Pair key", "kind:a:b; one row per pair per organization",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("survivor-id", "Survivor", "The record kept by the merge", VENTURE_FIELD_KIND_INTEGER,
		VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("scanned-at", "Scanned", "Last scan that confirmed the pair", VENTURE_FIELD_KIND_DATETIME,
		VENTURE_COLUMN_FLAG_NONE)
};

static gchar *
candidate_display_name(VentureEntity *self)
{
	g_autofree gchar *a = NULL, *b = NULL, *kind = NULL;
	g_object_get(self, "kind", &kind, "label-a", &a, "label-b", &b, NULL);
	return g_strdup_printf("%s: %s ~ %s", kind ? kind : "duplicate",
		venture_string_is_empty(a) ? "?" : a, venture_string_is_empty(b) ? "?" : b);
}

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureDuplicateCandidate, venture_duplicate_candidate, candidate_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = candidate_display_name;)
