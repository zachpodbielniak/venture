/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
/*
 * Duplicate companies and contacts that already exist. Capture-time
 * matching in the leads module stops a new inquiry from becoming a second
 * row; this service finds the second rows that are already there and folds
 * one into the other on an operator's say-so. The scan proposes and never
 * merges. The merge re-points references by walking the field tables for
 * reference fields that name the kind, so a record type registered
 * tomorrow is covered the day it registers.
 *
 * Re-pointing is one derived UPDATE per referencing column rather than a
 * save per row: payments, credits and allocations refuse every save once
 * persisted, and a merge that could not move a customer's payments would
 * leave the one-row-per-customer numbers this module exists for wrong for
 * exactly the customers who paid. The merge's own audit entry lists every
 * column moved and how many rows, and the survivor's timeline carries the
 * note, so the trail is at the merge rather than per row.
 */
struct _VentureDedupeService
{
	GObject parent_instance;
	VentureDatabase *database;
	GPtrArray *pending;
	guint writing;
	gboolean actions;
};
G_DEFINE_FINAL_TYPE(VentureDedupeService, venture_dedupe_service, G_TYPE_OBJECT)
enum { PROP_0, PROP_DATABASE, N_PROPS };
enum { MERGING, MERGED, N_SIGNALS };
static guint signals[N_SIGNALS];
#define STATUS_OPEN "open"
#define STATUS_MERGED "merged"
#define STATUS_DISMISSED "dismissed"
#define KIND_COMPANY "company"
#define KIND_CONTACT "contact"
/* Documented thresholds and scores; docs/dedupe.org repeats them. */
#define NAME_ALONE_THRESHOLD 75
#define DOMAIN_NAME_THRESHOLD 50
#define SCORE_EXACT 100
#define SCORE_DOMAIN_NAME 80

static gboolean
refuse(GError **error, gint code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureDedupeService: %s", message);
	return FALSE;
}
static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}
static gchar *
text(VentureEntity *e, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}
static gboolean
enabled(GError **error)
{
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "duplicate_candidate") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_VALIDATION, "the dedupe module is disabled");
	return TRUE;
}
static GType
kind_type(const gchar *kind, GError **error)
{
	if (g_strcmp0(kind, KIND_COMPANY) == 0) return VENTURE_TYPE_COMPANY;
	if (g_strcmp0(kind, KIND_CONTACT) == 0) return VENTURE_TYPE_CONTACT;
	refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "kind must be company or contact");
	return G_TYPE_INVALID;
}
static gboolean
first_error(GSignalInvocationHint *hint, GValue *accumulator, const GValue *value, gpointer data)
{
	(void)hint; (void)data;
	if (g_value_get_boxed(value) == NULL) return TRUE;
	g_value_copy(value, accumulator);
	return FALSE;
}
static void
transaction_finished(VentureDatabase *db, gboolean committed, VentureDedupeService *self)
{
	g_autoptr(GPtrArray) pending = self->pending;
	guint i;
	(void)db;
	self->pending = g_ptr_array_new_with_free_func(g_object_unref);
	if (!committed) return;
	for (i = 0; i < pending->len; i++)
		g_signal_emit(self, signals[MERGED], 0, g_ptr_array_index(pending, i));
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureDedupeService *self = VENTURE_DEDUPE_SERVICE(object);
	if (id == PROP_DATABASE) self->database = g_value_get_object(value);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureDedupeService *self = VENTURE_DEDUPE_SERVICE(object);
	if (id == PROP_DATABASE) g_value_set_object(value, self->database);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
finalize(GObject *object)
{
	VentureDedupeService *self = VENTURE_DEDUPE_SERVICE(object);
	g_clear_pointer(&self->pending, g_ptr_array_unref);
	G_OBJECT_CLASS(venture_dedupe_service_parent_class)->finalize(object);
}
static void
venture_dedupe_service_class_init(VentureDedupeServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->get_property = get_property;
	object->set_property = set_property;
	object->finalize = finalize;
	/**
	 * VentureDedupeService::merging:
	 * @self: the service
	 * @survivor: the record kept, as stored before the merge
	 * @loser: the record about to be folded in
	 *
	 * Emitted inside the transaction before any write. Return an owned
	 * #GError to veto; the first error stops emission and the merge.
	 */
	signals[MERGING] = g_signal_new("merging", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		0, first_error, NULL, NULL, G_TYPE_ERROR, 2, VENTURE_TYPE_ENTITY, VENTURE_TYPE_ENTITY);
	/**
	 * VentureDedupeService::merged:
	 * @self: the service
	 * @survivor: the surviving record
	 *
	 * Emitted after the outermost transaction commits. A rollback drops it.
	 * Keep external effects here.
	 */
	signals[MERGED] = g_signal_new("merged", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		0, NULL, NULL, NULL, G_TYPE_NONE, 1, VENTURE_TYPE_ENTITY);
	g_object_class_install_property(object, PROP_DATABASE, g_param_spec_object("database", "Database",
		"Owning repository", VENTURE_TYPE_DATABASE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void
venture_dedupe_service_init(VentureDedupeService *self)
{
	self->pending = g_ptr_array_new_with_free_func(g_object_unref);
}

/* --- Validators: the service is the only writer ----------------------------- */

static gboolean
candidate_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	VentureDedupeService *self = data;
	(void)db; (void)entity; (void)previous;
	if (self->writing > 0) return TRUE;
	return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
		"duplicate candidates are proposed by a scan and closed by a merge or dismissal through VentureDedupeService");
}
static gboolean
forward_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	VentureDedupeService *self = data;
	gint64 now = number(entity, "merged-into-id");
	gint64 before = previous ? number(previous, "merged-into-id") : 0;
	(void)db;
	if (self->writing > 0 || now == before) return TRUE;
	return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
		"merged_into_id is set by a VentureDedupeService merge, not by an edit");
}

VentureDedupeService *
venture_dedupe_service_get(VentureDatabase *database)
{
	VentureDedupeService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-dedupe-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_DEDUPE_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-dedupe-service", self, g_object_unref);
		venture_database_add_save_validator(database, VENTURE_TYPE_DUPLICATE_CANDIDATE, candidate_validate, self, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_COMPANY, forward_validate, self, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_CONTACT, forward_validate, self, NULL);
		g_signal_connect_object(database, "transaction-finished", G_CALLBACK(transaction_finished), self, 0);
		venture_dedupe_actions_register(database);
	}
	return self;
}

/* --- Name similarity ------------------------------------------------------ */

static gboolean
is_stop_word(const gchar *word)
{
	static const gchar *const stop[] = { "inc", "ltd", "llc", "co", "corp", "gmbh", "plc", "the", "and", "of", NULL };
	guint i;
	for (i = 0; stop[i]; i++)
		if (g_strcmp0(word, stop[i]) == 0) return TRUE;
	return FALSE;
}
/* The distinct words of a name, lower-cased, punctuation removed, legal and
 * filler words dropped. Keyed for O(1) membership. */
static GHashTable *
name_tokens(const gchar *name)
{
	GHashTable *tokens = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree gchar *lower = g_utf8_strdown(name ? name : "", -1);
	g_autoptr(GString) word = g_string_new(NULL);
	const gchar *p;
	if (!g_utf8_validate(lower, -1, NULL)) return tokens;
	for (p = lower; ; p = g_utf8_next_char(p))
	{
		gunichar c = *p ? g_utf8_get_char(p) : 0;
		if (c != 0 && g_unichar_isalnum(c))
		{
			g_string_append_unichar(word, c);
			continue;
		}
		if (word->len > 0 && !is_stop_word(word->str))
			g_hash_table_add(tokens, g_strdup(word->str));
		g_string_truncate(word, 0);
		if (c == 0) break;
	}
	return tokens;
}
static gint
token_similarity(GHashTable *a, GHashTable *b)
{
	GHashTableIter iter;
	gpointer key;
	guint shared = 0, longest;
	longest = MAX(g_hash_table_size(a), g_hash_table_size(b));
	if (longest == 0) return 0;
	g_hash_table_iter_init(&iter, a);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		if (g_hash_table_contains(b, key)) shared++;
	return (gint)(shared * 100 / longest);
}
gint
venture_dedupe_name_similarity(const gchar *a, const gchar *b)
{
	g_autoptr(GHashTable) ta = name_tokens(a);
	g_autoptr(GHashTable) tb = name_tokens(b);
	return token_similarity(ta, tb);
}

/* --- Field text ------------------------------------------------------------- */

static gboolean
value_is_empty(const GValue *value)
{
	if (G_VALUE_HOLDS_STRING(value)) return venture_string_is_empty(g_value_get_string(value));
	if (G_VALUE_HOLDS_INT64(value)) return g_value_get_int64(value) == 0;
	if (G_VALUE_HOLDS_INT(value)) return g_value_get_int(value) == 0;
	if (G_VALUE_HOLDS_UINT(value)) return g_value_get_uint(value) == 0;
	if (G_VALUE_HOLDS_DOUBLE(value)) return g_value_get_double(value) == 0.0;
	if (G_VALUE_HOLDS_BOOLEAN(value) || G_VALUE_HOLDS_ENUM(value)) return FALSE;
	if (G_VALUE_HOLDS_BOXED(value)) return g_value_get_boxed(value) == NULL;
	return TRUE;
}
static gboolean
values_equal(const GValue *a, const GValue *b)
{
	if (G_VALUE_HOLDS_STRING(a)) return g_strcmp0(g_value_get_string(a), g_value_get_string(b)) == 0;
	if (G_VALUE_HOLDS_INT64(a)) return g_value_get_int64(a) == g_value_get_int64(b);
	if (G_VALUE_HOLDS_INT(a)) return g_value_get_int(a) == g_value_get_int(b);
	if (G_VALUE_HOLDS_UINT(a)) return g_value_get_uint(a) == g_value_get_uint(b);
	if (G_VALUE_HOLDS_DOUBLE(a)) return g_value_get_double(a) == g_value_get_double(b);
	if (G_VALUE_HOLDS_BOOLEAN(a)) return g_value_get_boolean(a) == g_value_get_boolean(b);
	if (G_VALUE_HOLDS_ENUM(a)) return g_value_get_enum(a) == g_value_get_enum(b);
	if (G_VALUE_HOLDS(a, VENTURE_TYPE_MONEY)) return venture_money_equal(g_value_get_boxed(a), g_value_get_boxed(b));
	if (G_VALUE_HOLDS(a, G_TYPE_DATE_TIME)) return venture_time_equal(g_value_get_boxed(a), g_value_get_boxed(b));
	return TRUE;
}
static gchar *
value_text(const GValue *value)
{
	if (value_is_empty(value)) return NULL;
	if (G_VALUE_HOLDS_STRING(value)) return g_strdup(g_value_get_string(value));
	if (G_VALUE_HOLDS_INT64(value)) return g_strdup_printf("%" G_GINT64_FORMAT, g_value_get_int64(value));
	if (G_VALUE_HOLDS_INT(value)) return g_strdup_printf("%d", g_value_get_int(value));
	if (G_VALUE_HOLDS_UINT(value)) return g_strdup_printf("%u", g_value_get_uint(value));
	if (G_VALUE_HOLDS_DOUBLE(value))
	{
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
		return g_strdup(g_ascii_dtostr(buffer, sizeof(buffer), g_value_get_double(value)));
	}
	if (G_VALUE_HOLDS_BOOLEAN(value)) return g_strdup(g_value_get_boolean(value) ? "yes" : "no");
	if (G_VALUE_HOLDS_ENUM(value)) return g_strdup(venture_enum_to_nick(G_VALUE_TYPE(value), g_value_get_enum(value)));
	if (G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY)) return venture_money_to_string(g_value_get_boxed(value));
	if (G_VALUE_HOLDS(value, G_TYPE_DATE_TIME)) return venture_time_to_string(g_value_get_boxed(value));
	return NULL;
}
gchar *
venture_dedupe_field_text(VentureEntity *record, const gchar *field)
{
	GValue value = G_VALUE_INIT;
	gchar *result;
	g_return_val_if_fail(VENTURE_IS_ENTITY(record), NULL);
	g_return_val_if_fail(field != NULL, NULL);
	if (!venture_entity_get_field(record, field, &value)) return NULL;
	result = value_text(&value);
	g_value_unset(&value);
	return result;
}
/* The identity spine and the machinery this module owns are never unioned. */
static gboolean
field_is_machinery(VentureFieldSpec *spec)
{
	static const gchar *const skip[] = { "id", "uuid", "organization-id", "created-at", "updated-at",
		"deleted-at", "version", "merged-into-id", NULL };
	const gchar *name = venture_field_spec_get_name(spec);
	VentureColumnFlags flags = venture_field_spec_get_flags(spec);
	guint i;
	if (flags & (VENTURE_COLUMN_FLAG_TRANSIENT | VENTURE_COLUMN_FLAG_SENSITIVE)) return TRUE;
	for (i = 0; skip[i]; i++)
		if (g_strcmp0(name, skip[i]) == 0) return TRUE;
	return FALSE;
}

/* --- Scan ------------------------------------------------------------------- */

typedef struct
{
	gint64 id;
	gchar *label;
	gchar *email;
	gchar *phone;
	gchar *website;
	const gchar *domain;
	GHashTable *tokens;
} Profile;
static void
profile_free(gpointer data)
{
	Profile *p = data;
	g_free(p->label); g_free(p->email); g_free(p->phone); g_free(p->website);
	g_clear_pointer(&p->tokens, g_hash_table_unref);
	g_free(p);
}
static Profile *
profile_new(VentureEntity *record)
{
	Profile *p = g_new0(Profile, 1);
	g_autofree gchar *name = text(record, "name");
	g_autofree gchar *email = text(record, "email");
	g_autofree gchar *phone = text(record, "phone");
	g_autofree gchar *website = text(record, "website");
	const gchar *at;
	p->id = venture_entity_get_id(record);
	p->label = venture_entity_get_display_name(record);
	p->email = venture_lead_normalize_email(email);
	p->phone = venture_lead_normalize_phone(phone);
	p->website = venture_lead_normalize_website(website);
	at = strchr(p->email, '@');
	p->domain = (at != NULL && at[1] != '\0') ? at + 1 : NULL;
	p->tokens = name_tokens(name);
	return p;
}
static gboolean
same(const gchar *a, const gchar *b)
{
	return !venture_string_is_empty(a) && g_strcmp0(a, b) == 0;
}
/* Returns the score, filling @reasons; 0 means the pair does not match. */
static gint
compare_profiles(const Profile *a, const Profile *b, GPtrArray *reasons)
{
	gint score = 0;
	gint similarity = token_similarity(a->tokens, b->tokens);
	if (same(a->email, b->email)) { g_ptr_array_add(reasons, (gpointer)"email"); score = SCORE_EXACT; }
	if (same(a->phone, b->phone)) { g_ptr_array_add(reasons, (gpointer)"phone"); score = SCORE_EXACT; }
	if (same(a->website, b->website)) { g_ptr_array_add(reasons, (gpointer)"website"); score = SCORE_EXACT; }
	if (same(a->domain, b->domain) && similarity >= DOMAIN_NAME_THRESHOLD)
	{
		g_ptr_array_add(reasons, (gpointer)"domain+name");
		score = MAX(score, SCORE_DOMAIN_NAME);
	}
	if (similarity >= NAME_ALONE_THRESHOLD)
	{
		g_ptr_array_add(reasons, (gpointer)"name");
		score = MAX(score, similarity * 70 / 100);
	}
	return score;
}
static gchar *
reasons_json(GPtrArray *reasons)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) node = NULL;
	guint i;
	json_builder_begin_array(builder);
	for (i = 0; i < reasons->len; i++)
		json_builder_add_string_value(builder, g_ptr_array_index(reasons, i));
	json_builder_end_array(builder);
	node = json_builder_get_root(builder);
	return venture_json_to_string(node, FALSE);
}
static GPtrArray *
find_rows(VentureDedupeService *self, GType type, gint64 organization_id, gboolean include_deleted, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, organization_id);
	venture_query_set_include_deleted(query, include_deleted);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(self->database, query, error);
}
gint
venture_dedupe_service_scan(VentureDedupeService *self, gint64 organization_id,
	const gchar *kind, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) records = NULL, profiles = NULL, existing = NULL;
	g_autoptr(GHashTable) by_key = NULL, seen = NULL;
	g_autoptr(GDateTime) now = NULL;
	GType type;
	guint i, j;
	gint open = 0;

	g_return_val_if_fail(VENTURE_IS_DEDUPE_SERVICE(self), -1);
	if (!enabled(error)) return -1;
	type = kind_type(kind, error);
	if (type == G_TYPE_INVALID) return -1;
	if (organization_id <= 0)
	{
		refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "an organization is required");
		return -1;
	}
	if (!venture_database_begin(self->database, error)) return -1;
	self->writing++;
	records = find_rows(self, type, organization_id, FALSE, error);
	if (records == NULL) goto fail;
	profiles = g_ptr_array_new_with_free_func(profile_free);
	for (i = 0; i < records->len; i++)
		g_ptr_array_add(profiles, profile_new(g_ptr_array_index(records, i)));
	existing = find_rows(self, VENTURE_TYPE_DUPLICATE_CANDIDATE, organization_id, TRUE, error);
	if (existing == NULL) goto fail;
	by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < existing->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(existing, i);
		g_autofree gchar *row_kind = text(row, "kind");
		if (g_strcmp0(row_kind, kind) == 0)
			g_hash_table_insert(by_key, text(row, "pair-key"), row);
	}
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	now = venture_time_now();
	for (i = 0; i < profiles->len; i++)
	{
		for (j = i + 1; j < profiles->len; j++)
		{
			const Profile *a = g_ptr_array_index(profiles, i);
			const Profile *b = g_ptr_array_index(profiles, j);
			g_autoptr(GPtrArray) reasons = g_ptr_array_new();
			g_autofree gchar *key = NULL;
			g_autofree gchar *reasons_text = NULL;
			g_autofree gchar *status = NULL;
			g_autoptr(VentureEntity) created = NULL;
			VentureEntity *row;
			gint score = compare_profiles(a, b, reasons);
			if (score == 0) continue;
			key = g_strdup_printf("%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, kind, a->id, b->id);
			row = g_hash_table_lookup(by_key, key);
			if (row != NULL)
			{
				status = text(row, "status");
				/* A closed pair stays closed: a dismissal is a decision. */
				if (g_strcmp0(status, STATUS_MERGED) == 0) continue;
				if (g_strcmp0(status, STATUS_DISMISSED) == 0 && !venture_entity_is_deleted(row))
				{
					g_hash_table_add(seen, g_strdup(key));
					continue;
				}
				if (venture_entity_is_deleted(row) && !venture_database_restore(self->database, row, actor, error))
					goto fail;
			}
			else
			{
				created = g_object_new(VENTURE_TYPE_DUPLICATE_CANDIDATE, "organization-id", organization_id,
					"kind", kind, "record-a", a->id, "record-b", b->id, "pair-key", key, NULL);
				row = created;
			}
			reasons_text = reasons_json(reasons);
			g_object_set(row, "label-a", a->label, "label-b", b->label, "score", (gint64)score,
				"reasons", reasons_text, "status", STATUS_OPEN, "scanned-at", now, NULL);
			if (!venture_database_save(self->database, row, actor, error)) goto fail;
			g_hash_table_add(seen, g_strdup(key));
			open++;
		}
	}
	/* Open candidates the scan did not confirm no longer match: drop them. */
	for (i = 0; i < existing->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(existing, i);
		g_autofree gchar *row_kind = text(row, "kind");
		g_autofree gchar *status = text(row, "status");
		g_autofree gchar *key = text(row, "pair-key");
		if (g_strcmp0(row_kind, kind) != 0 || venture_entity_is_deleted(row)) continue;
		if (g_strcmp0(status, STATUS_OPEN) != 0 || g_hash_table_contains(seen, key)) continue;
		if (!venture_database_delete(self->database, row, actor, error)) goto fail;
	}
	self->writing--;
	if (!venture_database_commit(self->database, error)) return -1;
	return open;
fail:
	self->writing--;
	venture_database_rollback(self->database);
	return -1;
}

/* --- Merge ------------------------------------------------------------------ */

/* Currencies of the issued invoices and bills that name a record: the
 * financial history that a merge must not mix with another currency. */
static GHashTable *
posted_currencies(VentureDedupeService *self, const gchar *kind, gint64 id, GError **error)
{
	g_autoptr(GHashTable) currencies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	guint i;
	if (!venture_query_add_filter_int(query, g_strcmp0(kind, KIND_COMPANY) == 0 ? "company-id" : "contact-id",
			VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	venture_query_set_limit(query, 0);
	invoices = venture_database_find(self->database, query, error);
	if (invoices == NULL) return NULL;
	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(invoices, i);
		g_autoptr(VentureMoney) balance = NULL;
		VentureInvoiceStatus status;
		g_object_get(invoice, "status", &status, NULL);
		if (status != VENTURE_INVOICE_STATUS_SENT && status != VENTURE_INVOICE_STATUS_PAID &&
		    status != VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
			continue;
		balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(self->database),
			venture_entity_get_id(invoice), NULL, error);
		if (balance == NULL) return NULL;
		g_hash_table_add(currencies, g_strdup(venture_money_get_currency(balance)));
	}
	if (g_strcmp0(kind, KIND_COMPANY) == 0)
	{
		g_autoptr(GPtrArray) bills = NULL;
		g_autoptr(VentureQuery) bill_query = venture_query_new(VENTURE_TYPE_VENDOR_BILL);
		if (!venture_query_add_filter_int(bill_query, "company-id", VENTURE_FILTER_OP_EQ, id, error))
			return NULL;
		venture_query_set_limit(bill_query, 0);
		bills = venture_database_find(self->database, bill_query, error);
		if (bills == NULL) return NULL;
		for (i = 0; i < bills->len; i++)
		{
			VentureEntity *bill = g_ptr_array_index(bills, i);
			g_autofree gchar *status = text(bill, "status");
			g_autofree gchar *currency = text(bill, "currency");
			if (g_strcmp0(status, "draft") == 0 || g_strcmp0(status, "void") == 0 || venture_string_is_empty(currency))
				continue;
			g_hash_table_add(currencies, g_steal_pointer(&currency));
		}
	}
	return g_steal_pointer(&currencies);
}
static gboolean
currencies_compatible(VentureDedupeService *self, const gchar *kind, gint64 survivor_id, gint64 loser_id, GError **error)
{
	g_autoptr(GHashTable) survivor = posted_currencies(self, kind, survivor_id, error);
	g_autoptr(GHashTable) loser = NULL;
	GHashTableIter iter;
	gpointer currency;
	if (survivor == NULL) return FALSE;
	loser = posted_currencies(self, kind, loser_id, error);
	if (loser == NULL) return FALSE;
	if (g_hash_table_size(survivor) == 0) return TRUE;
	g_hash_table_iter_init(&iter, loser);
	while (g_hash_table_iter_next(&iter, &currency, NULL))
	{
		if (!g_hash_table_contains(survivor, currency))
		{
			g_autofree gchar *message = g_strdup_printf(
				"#%" G_GINT64_FORMAT " has issued documents in %s but #%" G_GINT64_FORMAT
				" has none in that currency; merge the other way or keep both",
				loser_id, (const gchar *)currency, survivor_id);
			return refuse(error, VENTURE_ERROR_CONFLICT, message);
		}
	}
	return TRUE;
}
static GList *
bound(gint64 first, gint64 second, gboolean two)
{
	GList *params = g_list_append(NULL, orm_value_new_integer(first));
	if (two) params = g_list_append(params, orm_value_new_integer(second));
	return params;
}
/* A registered type whose module has never been on has no table yet; the
 * walk over the registry must skip it rather than fail the merge on it. */
static gboolean
table_exists(VentureDedupeService *self, const gchar *table, gboolean *exists, GError **error)
{
	g_autoptr(OrmResult) result = NULL;
	GList *params = g_list_append(NULL, orm_value_new_string(table));
	const gchar *sql = venture_database_get_backend(self->database) == VENTURE_DATABASE_BACKEND_POSTGRES
		? "SELECT CAST(COUNT(*) AS BIGINT) FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = ?"
		: "SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name = ?";
	result = venture_database_query_raw(self->database, sql, params, error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);
	if (result == NULL) return FALSE;
	*exists = orm_result_next(result) && orm_row_get_integer(orm_result_get_row(result), 0) > 0;
	return TRUE;
}
/* Every reference field the registry knows that names @kind, in whatever
 * module and whether or not that module is on: the rows are still there. */
static gboolean
repoint_references(VentureDedupeService *self, const gchar *kind, gint64 survivor_id, gint64 loser_id,
	JsonObject *repointed, GError **error)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_auto(GStrv) names = venture_entity_registry_list_all_names(registry);
	guint i, j;
	gboolean present;
	for (i = 0; names[i] != NULL; i++)
	{
		GType type = venture_entity_registry_lookup_any(registry, names[i]);
		g_autoptr(VentureEntity) prototype = NULL;
		g_autoptr(GPtrArray) specs = NULL;
		g_autofree gchar *table = NULL;
		if (type == G_TYPE_INVALID || type == VENTURE_TYPE_DUPLICATE_CANDIDATE || type == VENTURE_TYPE_AUDIT_ENTRY)
			continue;
		prototype = g_object_new(type, NULL);
		specs = venture_entity_get_field_specs(prototype);
		if (!table_exists(self, venture_entity_get_table_name(prototype), &present, error)) return FALSE;
		if (!present) continue;
		table = venture_schema_quote_identifier(venture_entity_get_table_name(prototype));
		for (j = 0; j < specs->len; j++)
		{
			VentureFieldSpec *spec = g_ptr_array_index(specs, j);
			g_autofree gchar *column = NULL, *quoted = NULL, *sql = NULL, *key = NULL;
			g_autoptr(OrmResult) result = NULL;
			GList *params;
			gint64 count = 0;
			gboolean ok;
			if (venture_field_spec_get_kind(spec) != VENTURE_FIELD_KIND_REFERENCE ||
			    g_strcmp0(venture_field_spec_get_reference_type(spec), kind) != 0)
				continue;
			column = venture_entity_property_to_column(venture_field_spec_get_name(spec));
			quoted = venture_schema_quote_identifier(column);
			sql = g_strdup_printf("SELECT CAST(COUNT(*) AS BIGINT) FROM %s WHERE %s = ?", table, quoted);
			params = bound(loser_id, 0, FALSE);
			result = venture_database_query_raw(self->database, sql, params, error);
			g_list_free_full(params, (GDestroyNotify)orm_value_free);
			if (result == NULL) return FALSE;
			if (orm_result_next(result))
				count = orm_row_get_integer(orm_result_get_row(result), 0);
			if (count == 0) continue;
			g_clear_pointer(&sql, g_free);
			sql = g_strdup_printf("UPDATE %s SET %s = ? WHERE %s = ?", table, quoted, quoted);
			params = bound(survivor_id, loser_id, TRUE);
			ok = venture_database_execute(self->database, sql, params, error);
			g_list_free_full(params, (GDestroyNotify)orm_value_free);
			if (!ok) return FALSE;
			key = g_strdup_printf("%s.%s", names[i], column);
			json_object_set_int_member(repointed, key, count);
		}
	}
	return TRUE;
}
/* Fill what the survivor lacks; keep what it has, noting the loser's. */
static void
union_fields(VentureEntity *survivor, VentureEntity *loser, JsonObject *took, JsonObject *kept)
{
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(survivor);
	guint i;
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		const gchar *name = venture_field_spec_get_name(spec);
		GValue mine = G_VALUE_INIT, theirs = G_VALUE_INIT;
		g_autofree gchar *shown = NULL;
		if (field_is_machinery(spec)) continue;
		if (!venture_entity_get_field(survivor, name, &mine)) continue;
		if (!venture_entity_get_field(loser, name, &theirs)) { g_value_unset(&mine); continue; }
		shown = value_text(&theirs);
		if (shown != NULL)
		{
			if (value_is_empty(&mine))
			{
				g_object_set_property(G_OBJECT(survivor), name, &theirs);
				json_object_set_string_member(took, name, shown);
			}
			else if (!values_equal(&mine, &theirs))
				json_object_set_string_member(kept, name, shown);
		}
		g_value_unset(&mine);
		g_value_unset(&theirs);
	}
}
static void
append_members(GString *body, const gchar *heading, JsonObject *object)
{
	g_autoptr(GList) members = json_object_get_members(object);
	GList *l;
	if (members == NULL) return;
	g_string_append(body, heading);
	for (l = members; l != NULL; l = l->next)
	{
		JsonNode *node = json_object_get_member(object, l->data);
		g_string_append_printf(body, "\n  %s: ", (const gchar *)l->data);
		if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
			g_string_append(body, json_node_get_string(node));
		else
			g_string_append_printf(body, "%" G_GINT64_FORMAT, json_node_get_int(node));
	}
	g_string_append(body, "\n");
}
VentureEntity *
venture_dedupe_service_merge_records(VentureDedupeService *self, const gchar *kind,
	gint64 survivor_id, gint64 loser_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) survivor = NULL, loser = NULL, note = NULL;
	g_autoptr(VentureAuditEntry) entry = NULL;
	g_autoptr(JsonNode) diff = NULL;
	g_autoptr(GString) body = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *loser_label = NULL, *subject = NULL;
	JsonObject *merge, *repointed, *took, *kept;
	GError *veto = NULL;
	GType type;

	g_return_val_if_fail(VENTURE_IS_DEDUPE_SERVICE(self), NULL);
	if (!enabled(error)) return NULL;
	type = kind_type(kind, error);
	if (type == G_TYPE_INVALID) return NULL;
	if (survivor_id == loser_id)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "a record cannot be merged onto itself");
		return NULL;
	}
	survivor = venture_database_get(self->database, type, survivor_id, error);
	if (survivor == NULL || venture_entity_is_deleted(survivor))
	{
		if (error != NULL && *error == NULL)
			refuse(error, VENTURE_ERROR_NOT_FOUND, "the survivor does not exist or was deleted");
		return NULL;
	}
	loser = venture_database_get(self->database, type, loser_id, error);
	if (loser == NULL || venture_entity_is_deleted(loser))
	{
		if (error != NULL && *error == NULL)
			refuse(error, VENTURE_ERROR_NOT_FOUND, "the loser does not exist or was already merged");
		return NULL;
	}
	if (venture_entity_get_organization_id(survivor) != venture_entity_get_organization_id(loser))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "the two records belong to different organizations");
		return NULL;
	}
	if (!currencies_compatible(self, kind, survivor_id, loser_id, error)) return NULL;

	if (!venture_database_begin(self->database, error)) return NULL;
	self->writing++;
	g_signal_emit(self, signals[MERGING], 0, survivor, loser, &veto);
	if (veto != NULL) { g_propagate_error(error, veto); goto fail; }

	diff = json_node_new(JSON_NODE_OBJECT);
	merge = json_object_new();
	json_node_take_object(diff, merge);
	repointed = json_object_new(); took = json_object_new(); kept = json_object_new();
	json_object_set_int_member(merge, "loser_id", loser_id);
	json_object_set_string_member(merge, "kind", kind);
	loser_label = venture_entity_get_display_name(loser);
	json_object_set_string_member(merge, "loser_label", loser_label);
	json_object_set_object_member(merge, "repointed", repointed);
	json_object_set_object_member(merge, "took", took);
	json_object_set_object_member(merge, "kept_from_loser", kept);

	if (!repoint_references(self, kind, survivor_id, loser_id, repointed, error)) goto fail;
	union_fields(survivor, loser, took, kept);
	if (!venture_database_save(self->database, survivor, actor, error)) goto fail;

	/* The note is the human-readable half of the audit entry, on the
	 * survivor's own timeline where the next person will look. */
	now = venture_time_now();
	body = g_string_new(NULL);
	g_string_append_printf(body, "Merged duplicate %s #%" G_GINT64_FORMAT " \"%s\" into this record.\n",
		kind, loser_id, loser_label);
	append_members(body, "Taken from the merged record:", took);
	append_members(body, "Kept this record's values; the merged record had:", kept);
	append_members(body, "References re-pointed (type.column: rows):", repointed);
	subject = g_strdup_printf("Merged duplicate: %s", loser_label);
	note = g_object_new(VENTURE_TYPE_INTERACTION, "organization-id", venture_entity_get_organization_id(survivor),
		"kind", VENTURE_INTERACTION_KIND_NOTE, "subject", subject, "body", body->str, "occurred-at", now,
		g_strcmp0(kind, KIND_COMPANY) == 0 ? "company-id" : "contact-id", survivor_id, NULL);
	if (!venture_database_save(self->database, note, actor, error)) goto fail;

	g_object_set(loser, "merged-into-id", survivor_id, NULL);
	if (!venture_database_save(self->database, loser, actor, error)) goto fail;
	if (!venture_database_delete(self->database, loser, actor, error)) goto fail;

	entry = venture_audit_entry_new_for_change(VENTURE_AUDIT_ACTION_UPDATE,
		actor != NULL ? actor->kind : VENTURE_ACTOR_KIND_SYSTEM, actor != NULL ? actor->name : NULL,
		survivor, diff);
	g_object_set(entry, "source", "dedupe", NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(entry), actor, error)) goto fail;

	self->writing--;
	g_ptr_array_add(self->pending, g_object_ref(survivor));
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&survivor);
fail:
	self->writing--;
	venture_database_rollback(self->database);
	return NULL;
}
static gboolean
candidate_is_open(VentureEntity *candidate, GError **error)
{
	g_autofree gchar *status = text(candidate, "status");
	if (!VENTURE_IS_DUPLICATE_CANDIDATE(candidate) || venture_entity_is_deleted(candidate) ||
	    g_strcmp0(status, STATUS_OPEN) != 0)
		return refuse(error, VENTURE_ERROR_CONFLICT, "this candidate is no longer open; rescan");
	return TRUE;
}
VentureEntity *
venture_dedupe_service_merge(VentureDedupeService *self, VentureEntity *candidate,
	gint64 survivor_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) survivor = NULL;
	g_autoptr(GPtrArray) others = NULL;
	g_autofree gchar *kind = NULL;
	gint64 a, b, loser_id;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DEDUPE_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(candidate), NULL);
	if (!enabled(error) || !candidate_is_open(candidate, error)) return NULL;
	a = number(candidate, "record-a");
	b = number(candidate, "record-b");
	kind = text(candidate, "kind");
	if (survivor_id != a && survivor_id != b)
	{
		refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "the survivor must be one of the candidate's two records");
		return NULL;
	}
	loser_id = (survivor_id == a) ? b : a;
	if (!venture_database_begin(self->database, error)) return NULL;
	self->writing++;
	survivor = venture_dedupe_service_merge_records(self, kind, survivor_id, loser_id, actor, error);
	if (survivor == NULL) goto fail;
	g_object_set(candidate, "status", STATUS_MERGED, "survivor-id", survivor_id, NULL);
	if (!venture_database_save(self->database, candidate, actor, error)) goto fail;
	/* Other proposals that named the loser are moot: it is gone. */
	others = find_rows(self, VENTURE_TYPE_DUPLICATE_CANDIDATE, venture_entity_get_organization_id(candidate), FALSE, error);
	if (others == NULL) goto fail;
	for (i = 0; i < others->len; i++)
	{
		VentureEntity *other = g_ptr_array_index(others, i);
		g_autofree gchar *status = text(other, "status");
		g_autofree gchar *other_kind = text(other, "kind");
		if (venture_entity_get_id(other) == venture_entity_get_id(candidate)) continue;
		if (g_strcmp0(other_kind, kind) != 0 || g_strcmp0(status, STATUS_OPEN) != 0) continue;
		if (number(other, "record-a") != loser_id && number(other, "record-b") != loser_id) continue;
		if (!venture_database_delete(self->database, other, actor, error)) goto fail;
	}
	self->writing--;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&survivor);
fail:
	self->writing--;
	venture_database_rollback(self->database);
	return NULL;
}
gboolean
venture_dedupe_service_dismiss(VentureDedupeService *self, VentureEntity *candidate,
	const VentureActor *actor, GError **error)
{
	gboolean ok;
	g_return_val_if_fail(VENTURE_IS_DEDUPE_SERVICE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(candidate), FALSE);
	if (!enabled(error) || !candidate_is_open(candidate, error)) return FALSE;
	self->writing++;
	g_object_set(candidate, "status", STATUS_DISMISSED, NULL);
	ok = venture_database_save(self->database, candidate, actor, error);
	self->writing--;
	return ok;
}
gint64
venture_dedupe_merged_into(VentureDatabase *database, VentureEntity *record)
{
	g_autoptr(VentureEntity) current = NULL;
	gint64 target;
	guint hops;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), 0);
	if (record == NULL || !venture_entity_is_deleted(record)) return 0;
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "merged-into-id") == NULL) return 0;
	target = number(record, "merged-into-id");
	/* A merge collapses chains as it goes, so one hop is the normal case;
	 * the bound only guards a hand-edited row from looping. */
	for (hops = 0; target > 0 && hops < 8; hops++)
	{
		g_clear_object(&current);
		current = venture_database_get(database, G_OBJECT_TYPE(record), target, NULL);
		if (current == NULL) return 0;
		if (!venture_entity_is_deleted(current)) return target;
		target = number(current, "merged-into-id");
	}
	return 0;
}

/* --- Actions: REST, CLI and the assistant ---------------------------------- */

static gint64
param_id(GHashTable *params, const gchar *name)
{
	JsonNode *node = params ? g_hash_table_lookup(params, name) : NULL;
	const gchar *value;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)) return 0;
	if (json_node_get_value_type(node) == G_TYPE_INT64) return json_node_get_int(node);
	value = json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
	return value ? g_ascii_strtoll(value, NULL, 10) : 0;
}
static const gchar *
param_string(GHashTable *params, const gchar *name)
{
	JsonNode *node = params ? g_hash_table_lookup(params, name) : NULL;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}
static gboolean
scan_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action; (void)entity; (void)actor;
	return enabled(error);
}
static VentureEntity *
scan_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDedupeService *self = venture_action_get_data(action);
	const gchar *kind = param_string(params, "kind");
	gint64 org = param_id(params, "organization_id");
	gint open;
	if (venture_string_is_empty(kind)) kind = KIND_COMPANY;
	if (org <= 0 && entity != NULL) org = venture_entity_get_organization_id(entity);
	if (org <= 0)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) organization = NULL;
		venture_query_add_filter_string(query, "is-default", VENTURE_FILTER_OP_EQ, "true", NULL);
		organization = venture_database_find_one(self->database, query, NULL);
		org = organization != NULL ? venture_entity_get_id(organization) : 1;
	}
	open = venture_dedupe_service_scan(self, org, kind, actor, error);
	if (open < 0) return NULL;
	/* The answer is a summary shaped like a candidate: how many are open. */
	return g_object_new(VENTURE_TYPE_DUPLICATE_CANDIDATE, "organization-id", org, "kind", kind,
		"status", STATUS_OPEN, "score", (gint64)open, "pair-key", "", NULL);
}
static gboolean
open_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action; (void)actor;
	return enabled(error) && candidate_is_open(entity, error);
}
static VentureEntity *
merge_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDedupeService *self = venture_action_get_data(action);
	g_autoptr(VentureEntity) survivor = NULL;
	gint64 survivor_id = param_id(params, "survivor");
	if (survivor_id <= 0)
	{
		refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "survivor names which of the two records to keep");
		return NULL;
	}
	survivor = venture_dedupe_service_merge(self, entity, survivor_id, actor, error);
	if (survivor == NULL) return NULL;
	return g_object_ref(entity);
}
static VentureEntity *
dismiss_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDedupeService *self = venture_action_get_data(action);
	(void)params;
	if (!venture_dedupe_service_dismiss(self, entity, actor, error)) return NULL;
	return g_object_ref(entity);
}
void
venture_dedupe_actions_register(VentureDatabase *database)
{
	VentureDedupeService *self = venture_dedupe_service_get(database);
	VentureActionRegistry *registry;
	g_autoptr(GPtrArray) scan_parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(GPtrArray) merge_parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(GPtrArray) no_parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) scan = NULL, merge = NULL, dismiss = NULL;
	g_autoptr(GError) error = NULL;
	if (self->actions) return;
	self->actions = TRUE;
	registry = venture_database_get_action_registry(database);
	g_ptr_array_add(scan_parameters, venture_field_spec_new("kind", "Kind (company or contact)", VENTURE_FIELD_KIND_STRING));
	g_ptr_array_add(scan_parameters, venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER));
	scan = g_object_new(VENTURE_TYPE_ACTION, "type-name", "duplicate_candidate", "name", "scan",
		"label", "Scan for duplicates", "description", "Propose pairs of companies or contacts that look like one; merges nothing",
		"parameters", scan_parameters, "stageable", FALSE, "type-level", TRUE, "service-transaction", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, scan, scan_allowed, scan_invoke, self, NULL, &error))
		g_error("Dedupe scan action registration: %s", error->message);
	g_ptr_array_add(merge_parameters, venture_field_spec_new("survivor", "Record to keep", VENTURE_FIELD_KIND_INTEGER));
	merge = g_object_new(VENTURE_TYPE_ACTION, "type-name", "duplicate_candidate", "name", "merge",
		"label", "Merge", "description", "Fold the other record into the survivor: references, fields, timeline note, audit",
		"parameters", merge_parameters, "stageable", TRUE, "service-transaction", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, merge, open_allowed, merge_invoke, self, NULL, &error))
		g_error("Dedupe merge action registration: %s", error->message);
	dismiss = g_object_new(VENTURE_TYPE_ACTION, "type-name", "duplicate_candidate", "name", "dismiss",
		"label", "Not a duplicate", "description", "Close the proposal; later scans leave the pair alone",
		"parameters", no_parameters, "stageable", FALSE, "service-transaction", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, dismiss, open_allowed, dismiss_invoke, self, NULL, &error))
		g_error("Dedupe dismiss action registration: %s", error->message);
}
