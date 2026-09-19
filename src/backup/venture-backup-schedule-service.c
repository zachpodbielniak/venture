/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>

/*
 * Scheduled, retained, verified backups. The accounting snapshot itself is
 * VentureBackupService's (docs/backup.org, version 4); this service only
 * decides when one is written, where it goes, how many are kept, and whether
 * it restores into an empty database and ties. Off-site transport is not
 * here: the file in =backup_run.path= is the hand-off point for rsync, S3 or
 * whatever the operator attaches.
 */

#define KIND_BACKUP "backup"
#define KIND_DRILL "drill"
#define SCOPE_ORGANIZATION "organization"
#define SCOPE_INSTALLATION "installation"
#define STATUS_RUNNING "running"
#define STATUS_SUCCEEDED "succeeded"
#define STATUS_FAILED "failed"
#define STATUS_PRUNED "pruned"
#define VERIFICATION_TIE "tie"
#define VERIFICATION_MISMATCH "mismatch"
#define DEFAULT_RETENTION 7

struct _VentureBackupScheduleService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureConfig *config;
	VentureEntity *writing;
	gboolean actions;
};
G_DEFINE_FINAL_TYPE(VentureBackupScheduleService, venture_backup_schedule_service, G_TYPE_OBJECT)

enum { PROP_0, PROP_DATABASE, PROP_CONFIG, N_PROPS };

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureBackupScheduleService: %s", message);
	return FALSE;
}

static gboolean
enabled(GError **error)
{
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "backup_schedule") == G_TYPE_INVALID)
		return refuse(error, "The backup module is disabled");
	return TRUE;
}

static gchar *
text(VentureEntity *e, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gboolean
validate_schedule(VentureDatabase *database, VentureEntity *record, VentureEntity *previous,
	gpointer data, GError **error)
{
	g_autofree gchar *schedule = NULL, *scope = NULL;
	gint64 retention = 0;
	(void)database;
	(void)previous;
	(void)data;
	g_object_get(record, "schedule", &schedule, "scope", &scope, "retention", &retention, NULL);
	if (g_strcmp0(scope, SCOPE_ORGANIZATION) != 0 && g_strcmp0(scope, SCOPE_INSTALLATION) != 0)
		return refuse(error, "scope must be organization or installation");
	if (retention < 0)
		return refuse(error, "retention is a count of backups to keep; zero uses backup.retention");
	if (!venture_string_is_empty(schedule) && !venture_report_pack_schedule_validate(schedule, error))
		return FALSE;
	return TRUE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VentureBackupScheduleService *self = VENTURE_BACKUP_SCHEDULE_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE: g_value_set_object(value, self->database); break;
	case PROP_CONFIG: g_value_set_object(value, self->config); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureBackupScheduleService *self = VENTURE_BACKUP_SCHEDULE_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE:
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
		break;
	case PROP_CONFIG:
		g_set_object(&self->config, g_value_get_object(value));
		break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
finalize(GObject *object)
{
	VentureBackupScheduleService *self = VENTURE_BACKUP_SCHEDULE_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_object(&self->config);
	G_OBJECT_CLASS(venture_backup_schedule_service_parent_class)->finalize(object);
}

static void
venture_backup_schedule_service_class_init(VentureBackupScheduleServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, PROP_DATABASE,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_CONFIG,
		g_param_spec_object("config", "Configuration", "Supplies backup.directory and backup.retention", VENTURE_TYPE_CONFIG,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
venture_backup_schedule_service_init(VentureBackupScheduleService *self)
{
	(void)self;
}

VentureBackupScheduleService *
venture_backup_schedule_service_get(VentureDatabase *database)
{
	VentureBackupScheduleService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-backup-schedule-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_BACKUP_SCHEDULE_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-backup-schedule-service", self, g_object_unref);
		venture_database_add_save_validator(database, VENTURE_TYPE_BACKUP_SCHEDULE, validate_schedule, NULL, NULL);
	}
	return self;
}

void
venture_backup_schedule_service_set_config(VentureBackupScheduleService *self, VentureConfig *config)
{
	g_return_if_fail(VENTURE_IS_BACKUP_SCHEDULE_SERVICE(self));
	g_object_set(self, "config", config, NULL);
}

gboolean
venture_backup_schedule_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	VentureBackupScheduleService *self;
	(void)removal;
	if (record == NULL || database == NULL || !VENTURE_IS_BACKUP_RUN(record))
		return TRUE;
	self = venture_backup_schedule_service_get(database);
	if (self->writing == record)
		return TRUE;
	return refuse(error, "backup runs are evidence owned by VentureBackupScheduleService");
}

static gboolean
save_owned(VentureBackupScheduleService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

/* --- Where and how many --------------------------------------------------- */

static gchar *
resolve_destination(VentureBackupScheduleService *self, VentureEntity *schedule)
{
	g_autofree gchar *destination = text(schedule, "destination");
	g_autofree gchar *configured = NULL;
	g_autofree gchar *cwd = NULL;
	if (!venture_string_is_empty(destination))
		return g_steal_pointer(&destination);
	if (self->config != NULL)
	{
		g_object_get(self->config, "backup-directory", &configured, NULL);
		return venture_config_resolve_path(self->config, venture_string_is_empty(configured) ? "backups" : configured);
	}
	cwd = g_get_current_dir();
	return g_build_filename(cwd, "backups", NULL);
}

static gint64
resolve_retention(VentureBackupScheduleService *self, VentureEntity *schedule)
{
	gint64 retention = number(schedule, "retention");
	if (retention > 0)
		return retention;
	if (self->config != NULL)
		g_object_get(self->config, "backup-retention", &retention, NULL);
	return retention > 0 ? retention : DEFAULT_RETENTION;
}

/* Streams the file so an installation copy is never held in memory twice. */
static gchar *
file_digest(const gchar *path, gint64 *size, GError **error)
{
	g_autoptr(GFile) file = g_file_new_for_path(path);
	g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, error);
	g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
	guchar buffer[65536];
	gssize count;
	*size = 0;
	if (stream == NULL)
		return NULL;
	while ((count = g_input_stream_read(G_INPUT_STREAM(stream), buffer, sizeof buffer, NULL, error)) > 0)
	{
		g_checksum_update(checksum, buffer, count);
		*size += count;
	}
	if (count < 0)
		return NULL;
	return g_strdup(g_checksum_get_string(checksum));
}

static gboolean
write_organization_snapshot(VentureBackupScheduleService *self, gint64 org, const gchar *path, GError **error)
{
	g_autofree gchar *payload = venture_backup_service_snapshot(venture_backup_service_get(self->database), org, error);
	if (payload == NULL)
		return FALSE;
	return g_file_set_contents(path, payload, -1, error);
}

/* SQLite copies itself consistently with VACUUM INTO, in this process and
 * without a thread. PostgreSQL has no in-process equivalent; pg_dump is the
 * tool and the place an operator attaches it is documented. */
static gboolean
write_installation_copy(VentureBackupScheduleService *self, const gchar *path, GError **error)
{
	g_autoptr(GString) sql = NULL;
	const gchar *p;
	if (venture_database_get_backend(self->database) != VENTURE_DATABASE_BACKEND_SQLITE)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
			"Installation backups of a PostgreSQL database attach at pg_dump; schedule it against the same destination");
		return FALSE;
	}
	if (venture_database_has_transaction(self->database))
		return refuse(error, "an installation copy needs no open transaction");
	sql = g_string_new("VACUUM INTO '");
	for (p = path; *p != '\0'; p++)
	{
		if (*p == '\'')
			g_string_append_c(sql, '\'');
		g_string_append_c(sql, *p);
	}
	g_string_append_c(sql, '\'');
	return venture_database_execute(self->database, sql->str, NULL, error);
}

static gboolean
stamp(VentureBackupScheduleService *self, VentureEntity *run, const gchar *status, const gchar *message,
	GDateTime *finished, const VentureActor *actor, GError **error)
{
	g_object_set(run, "status", status, "finished-at", finished, NULL);
	if (message != NULL)
		g_object_set(run, "message", message, NULL);
	return save_owned(self, run, actor, error);
}

/* Successful backups beyond retention go oldest-first. A run whose file
 * cannot be removed keeps its status; nothing is ever deleted after a
 * failed backup because prune only runs after a success. */
static gboolean
prune(VentureBackupScheduleService *self, VentureEntity *schedule, gint64 retention, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BACKUP_RUN);
	g_autoptr(GPtrArray) rows = NULL;
	guint i, excess;
	venture_query_set_organization(query, venture_entity_get_organization_id(schedule));
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "schedule-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(schedule), error) ||
		!venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, KIND_BACKUP, error) ||
		!venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, STATUS_SUCCEEDED, error) ||
		!venture_query_add_order(query, "started-at", VENTURE_SORT_ASCENDING, error) ||
		!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return FALSE;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return FALSE;
	excess = rows->len > retention ? rows->len - (guint)retention : 0;
	for (i = 0; i < excess; i++)
	{
		VentureEntity *run = g_ptr_array_index(rows, i);
		g_autofree gchar *path = text(run, "path");
		g_autofree gchar *message = NULL;
		if (!venture_string_is_empty(path) && g_unlink(path) != 0 && errno != ENOENT)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Could not prune %s: %s", path, g_strerror(errno));
			return FALSE;
		}
		message = g_strdup_printf("pruned: retention keeps %" G_GINT64_FORMAT, retention);
		g_object_set(run, "status", STATUS_PRUNED, "message", message, NULL);
		if (!save_owned(self, run, actor, error))
			return FALSE;
	}
	return TRUE;
}

/* --- Comparing two sets of books ---------------------------------------- */

typedef struct
{
	JsonBuilder *builder;
	JsonArray *mismatches;
	gboolean tie;
} Comparison;

static void
mismatch(Comparison *c, const gchar *what)
{
	c->tie = FALSE;
	json_array_add_string_element(c->mismatches, what);
}

static gint64
account_by_code(VentureDatabase *database, gint64 org, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(database, query, NULL);
	return row != NULL ? venture_entity_get_id(row) : 0;
}

static gboolean
collect_currencies(VentureDatabase *database, gint64 org, GHashTable *currencies, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	journals = venture_database_find(database, query, error);
	if (journals == NULL)
		return FALSE;
	for (i = 0; i < journals->len; i++)
	{
		gchar *currency = text(g_ptr_array_index(journals, i), "currency");
		if (venture_string_is_empty(currency))
			g_free(currency);
		else
			g_hash_table_add(currencies, currency);
	}
	return TRUE;
}

static gboolean
accumulate(VentureMoney **sum, const VentureMoney *amount, GError **error)
{
	VentureMoney *total = venture_money_add(*sum, amount, error);
	if (total == NULL)
		return FALSE;
	venture_money_free(*sum);
	*sum = total;
	return TRUE;
}

/* The same numbers docs/ledger.org's trial balance shows, asked of each
 * side's own posting service: every account's balance per currency, and the
 * debit and credit totals those balances add up to. */
static gboolean
compare_ledger(Comparison *c, VentureDatabase *live, gint64 live_org, VentureDatabase *other, gint64 other_org, GError **error)
{
	g_autoptr(GHashTable) currencies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GList) names = NULL;
	g_autoptr(GDateTime) cutoff = g_date_time_new_utc(2999, 1, 1, 0, 0, 0);
	g_autoptr(JsonBuilder) totals = json_builder_new();
	gboolean balances_tie = TRUE, totals_tie = TRUE;
	GList *node;
	guint i;
	if (!collect_currencies(live, live_org, currencies, error) || !collect_currencies(other, other_org, currencies, error))
		return FALSE;
	venture_query_set_organization(query, live_org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "code", VENTURE_SORT_ASCENDING, NULL);
	accounts = venture_database_find(live, query, error);
	if (accounts == NULL)
		return FALSE;
	names = g_list_sort(g_hash_table_get_keys(currencies), (GCompareFunc)g_strcmp0);
	json_builder_set_member_name(c->builder, "trial_balance");
	json_builder_begin_object(c->builder);
	json_builder_set_member_name(c->builder, "accounts");
	json_builder_begin_array(c->builder);
	json_builder_begin_object(totals);
	for (node = names; node != NULL; node = node->next)
	{
		const gchar *currency = node->data;
		g_autoptr(VentureMoney) live_debits = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) live_credits = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) other_debits = venture_money_new_zero(currency);
		g_autoptr(VentureMoney) other_credits = venture_money_new_zero(currency);
		g_autofree gchar *ld = NULL, *lc = NULL, *od = NULL, *oc = NULL;
		for (i = 0; i < accounts->len; i++)
		{
			VentureEntity *account = g_ptr_array_index(accounts, i);
			g_autofree gchar *code = text(account, "code");
			g_autoptr(VentureMoney) before = NULL, after = NULL, before_abs = NULL, after_abs = NULL;
			g_autofree gchar *before_text = NULL, *after_text = NULL;
			gint64 mapped = account_by_code(other, other_org, code);
			before = venture_posting_service_account_balance(venture_database_get_posting_service(live),
				venture_entity_get_id(account), live_org, currency, cutoff, error);
			if (before == NULL)
				return FALSE;
			if (mapped != 0)
			{
				after = venture_posting_service_account_balance(venture_database_get_posting_service(other),
					mapped, other_org, currency, cutoff, error);
				if (after == NULL)
					return FALSE;
			}
			else
				after = venture_money_new_zero(currency);
			if (venture_money_is_zero(before) && venture_money_is_zero(after))
				continue;
			before_abs = venture_money_abs(before);
			after_abs = venture_money_abs(after);
			if (!accumulate(venture_money_is_negative(before) ? &live_credits : &live_debits, before_abs, error) ||
				!accumulate(venture_money_is_negative(after) ? &other_credits : &other_debits, after_abs, error))
				return FALSE;
			before_text = venture_money_to_string(before);
			after_text = venture_money_to_string(after);
			json_builder_begin_object(c->builder);
			json_builder_set_member_name(c->builder, "code");
			json_builder_add_string_value(c->builder, code);
			json_builder_set_member_name(c->builder, "currency");
			json_builder_add_string_value(c->builder, currency);
			json_builder_set_member_name(c->builder, "live");
			json_builder_add_string_value(c->builder, before_text);
			json_builder_set_member_name(c->builder, "restored");
			json_builder_add_string_value(c->builder, after_text);
			json_builder_set_member_name(c->builder, "tie");
			json_builder_add_boolean_value(c->builder, venture_money_equal(before, after));
			json_builder_end_object(c->builder);
			if (!venture_money_equal(before, after))
			{
				g_autofree gchar *what = g_strdup_printf("account %s %s: live %s, restored %s", code, currency, before_text, after_text);
				balances_tie = FALSE;
				mismatch(c, what);
			}
		}
		ld = venture_money_to_string(live_debits);
		lc = venture_money_to_string(live_credits);
		od = venture_money_to_string(other_debits);
		oc = venture_money_to_string(other_credits);
		json_builder_set_member_name(totals, currency);
		json_builder_begin_object(totals);
		json_builder_set_member_name(totals, "live_debits");
		json_builder_add_string_value(totals, ld);
		json_builder_set_member_name(totals, "live_credits");
		json_builder_add_string_value(totals, lc);
		json_builder_set_member_name(totals, "restored_debits");
		json_builder_add_string_value(totals, od);
		json_builder_set_member_name(totals, "restored_credits");
		json_builder_add_string_value(totals, oc);
		json_builder_end_object(totals);
		if (!venture_money_equal(live_debits, other_debits) || !venture_money_equal(live_credits, other_credits))
		{
			g_autofree gchar *what = g_strdup_printf("ledger totals %s: live %s/%s, restored %s/%s", currency, ld, lc, od, oc);
			totals_tie = FALSE;
			mismatch(c, what);
		}
	}
	json_builder_end_array(c->builder);
	json_builder_set_member_name(c->builder, "tie");
	json_builder_add_boolean_value(c->builder, balances_tie);
	json_builder_end_object(c->builder);
	json_builder_end_object(totals);
	json_builder_set_member_name(c->builder, "ledger_totals");
	json_builder_begin_object(c->builder);
	json_builder_set_member_name(c->builder, "currencies");
	{
		g_autoptr(JsonNode) built = json_builder_get_root(totals);
		json_builder_add_value(c->builder, json_node_copy(built));
	}
	json_builder_set_member_name(c->builder, "tie");
	json_builder_add_boolean_value(c->builder, totals_tie);
	json_builder_end_object(c->builder);
	return TRUE;
}

/* A disabled module's table may not exist; an installation copy carries
 * exactly the live schema, so the live side decides what is countable. */
static gint
table_exists(VentureDatabase *database, GType type, GError **error)
{
	g_autoptr(VentureEntity) prototype = g_object_new(type, NULL);
	g_autoptr(GHashTable) columns = venture_schema_get_existing_columns(
		venture_database_get_connection(database), venture_entity_get_table_name(prototype), error);
	return columns == NULL ? -1 : g_hash_table_size(columns) != 0;
}

static gint64
count_type(VentureDatabase *database, GType type, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	return venture_database_count(database, query, error);
}

/* Row counts per record type, soft-deleted rows included, since the
 * snapshot keeps them. Types with nothing on either side stay out of the
 * report so a reader sees what exists, not the whole registry. */
static gboolean
compare_documents(Comparison *c, VentureDatabase *live, gint64 live_org, VentureDatabase *other, gint64 other_org,
	GPtrArray *type_names, GError **error)
{
	gboolean counts_tie = TRUE;
	guint i;
	json_builder_set_member_name(c->builder, "document_counts");
	json_builder_begin_object(c->builder);
	for (i = 0; i < type_names->len; i++)
	{
		const gchar *name = g_ptr_array_index(type_names, i);
		GType type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), name);
		gint64 before, after;
		gint exists;
		if (type == G_TYPE_INVALID)
			continue;
		exists = table_exists(live, type, error);
		if (exists < 0)
			return FALSE;
		if (exists == 0)
			continue;
		before = count_type(live, type, live_org, error);
		if (before < 0)
			return FALSE;
		after = count_type(other, type, other_org, error);
		if (after < 0)
			return FALSE;
		if (before == 0 && after == 0)
			continue;
		json_builder_set_member_name(c->builder, name);
		json_builder_begin_object(c->builder);
		json_builder_set_member_name(c->builder, "live");
		json_builder_add_int_value(c->builder, before);
		json_builder_set_member_name(c->builder, "restored");
		json_builder_add_int_value(c->builder, after);
		json_builder_set_member_name(c->builder, "tie");
		json_builder_add_boolean_value(c->builder, before == after);
		json_builder_end_object(c->builder);
		if (before != after)
		{
			g_autofree gchar *what = g_strdup_printf("%s count: live %" G_GINT64_FORMAT ", restored %" G_GINT64_FORMAT, name, before, after);
			counts_tie = FALSE;
			mismatch(c, what);
		}
	}
	json_builder_end_object(c->builder);
	json_builder_set_member_name(c->builder, "document_counts_tie");
	json_builder_add_boolean_value(c->builder, counts_tie);
	return TRUE;
}

static gboolean
compare_books(Comparison *c, VentureDatabase *live, gint64 live_org, VentureDatabase *other, gint64 other_org,
	GPtrArray *type_names, GError **error)
{
	json_builder_set_member_name(c->builder, "organization_id");
	json_builder_add_int_value(c->builder, live_org);
	return compare_ledger(c, live, live_org, other, other_org, error) &&
		compare_documents(c, live, live_org, other, other_org, type_names, error);
}

/* The record types an archive carries, in archive order, without repeats. */
static GPtrArray *
archive_types(const gchar *payload, GError **error)
{
	g_autoptr(JsonNode) root = venture_json_parse(payload, error);
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	JsonArray *records;
	guint i;
	if (root == NULL)
		return NULL;
	if (!JSON_NODE_HOLDS_OBJECT(root) || !json_object_has_member(json_node_get_object(root), "records"))
	{
		refuse(error, "the file is not a version-4 accounting snapshot");
		return NULL;
	}
	records = json_object_get_array_member(json_node_get_object(root), "records");
	for (i = 0; i < json_array_get_length(records); i++)
	{
		const gchar *type = json_object_get_string_member(json_array_get_object_element(records, i), "type");
		if (type != NULL && !g_hash_table_contains(seen, type))
		{
			gchar *copy = g_strdup(type);
			g_hash_table_add(seen, copy);
			g_ptr_array_add(names, copy);
		}
	}
	return g_steal_pointer(&names);
}

static gboolean
verify_organization_snapshot(VentureBackupScheduleService *self, Comparison *c, gint64 org, const gchar *payload, GError **error)
{
	g_autoptr(VentureDatabase) temporary = venture_database_new("sqlite://:memory:", error);
	g_autoptr(VentureOrganization) destination = venture_organization_new();
	g_autoptr(GPtrArray) types = NULL;
	if (temporary == NULL || !venture_database_migrate(temporary, venture_entity_registry_get_default(), error))
		return FALSE;
	types = archive_types(payload, error);
	if (types == NULL)
		return FALSE;
	g_object_set(destination, "name", "Backup verification", "legal-name", "Backup verification", NULL);
	if (!venture_database_save(temporary, VENTURE_ENTITY(destination), NULL, error) ||
		!venture_backup_service_restore(venture_backup_service_get(temporary),
			venture_entity_get_id(VENTURE_ENTITY(destination)), payload, NULL, error))
		return FALSE;
	json_builder_set_member_name(c->builder, "organizations");
	json_builder_begin_array(c->builder);
	json_builder_begin_object(c->builder);
	if (!compare_books(c, self->database, org, temporary, venture_entity_get_id(VENTURE_ENTITY(destination)), types, error))
		return FALSE;
	json_builder_end_object(c->builder);
	json_builder_end_array(c->builder);
	return TRUE;
}

/* Opening SQLite switches the file to WAL and rewrites its header, so the
 * retained artefact is never opened: a scratch copy is. */
static gboolean
verify_installation_copy(VentureBackupScheduleService *self, Comparison *c, const gchar *path, GError **error)
{
	g_autofree gchar *scratch = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(VentureDatabase) copy = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	g_autoptr(GPtrArray) organizations = NULL;
	g_autoptr(GPtrArray) types = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) names = NULL;
	gsize length = 0;
	gboolean ok = TRUE;
	guint i;
	gint fd = g_file_open_tmp("venture-verify-XXXXXX.sqlite", &scratch, error);
	if (fd < 0)
		return FALSE;
	g_close(fd, NULL);
	if (!g_file_get_contents(path, &contents, &length, error) || !g_file_set_contents(scratch, contents, length, error))
	{
		g_unlink(scratch);
		return FALSE;
	}
	g_clear_pointer(&contents, g_free);
	uri = g_strconcat("sqlite://", scratch, NULL);
	copy = venture_database_new(uri, error);
	if (copy == NULL)
		ok = FALSE;
	/* The copy predates the run's own bookkeeping: the run row stamped
	 * succeeded, the schedule's last-run-at, and the audit entries those
	 * saves write. Counting them would report the backup as its own
	 * mismatch, so the live side is compared on everything else. */
	names = venture_entity_registry_list_all_names(venture_entity_registry_get_default());
	for (i = 0; names != NULL && names[i] != NULL; i++)
		if (g_strcmp0(names[i], "audit_entry") != 0 && g_strcmp0(names[i], "backup_run") != 0 &&
			g_strcmp0(names[i], "backup_schedule") != 0)
			g_ptr_array_add(types, g_strdup(names[i]));
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	organizations = ok ? venture_database_find(self->database, query, error) : NULL;
	if (organizations == NULL)
		ok = FALSE;
	if (ok)
	{
		json_builder_set_member_name(c->builder, "organizations");
		json_builder_begin_array(c->builder);
		for (i = 0; ok && i < organizations->len; i++)
		{
			gint64 org = venture_entity_get_id(g_ptr_array_index(organizations, i));
			json_builder_begin_object(c->builder);
			ok = compare_books(c, self->database, org, copy, org, types, error);
			json_builder_end_object(c->builder);
		}
		json_builder_end_array(c->builder);
	}
	g_clear_object(&copy);
	g_unlink(scratch);
	{
		g_autofree gchar *wal = g_strconcat(scratch, "-wal", NULL);
		g_autofree gchar *shm = g_strconcat(scratch, "-shm", NULL);
		g_unlink(wal);
		g_unlink(shm);
	}
	return ok;
}

/* Restore and tie out one run's file. The verdict and the report land on
 * @target, which is the run itself for a verification and the new drill
 * record for a drill. */
static gboolean
verify_into(VentureBackupScheduleService *self, VentureEntity *run, VentureEntity *target, const VentureActor *actor, GError **error)
{
	g_autofree gchar *status = text(run, "status");
	g_autofree gchar *scope = text(run, "scope");
	g_autofree gchar *path = text(run, "path");
	g_autofree gchar *recorded = text(run, "sha256");
	g_autofree gchar *digest = NULL;
	g_autofree gchar *report = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonArray) mismatches = json_array_new();
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	Comparison c;
	gint64 size = 0;
	gboolean ok;
	if (g_strcmp0(status, STATUS_SUCCEEDED) != 0)
		return refuse(error, "only a succeeded backup can be verified");
	if (venture_string_is_empty(path) || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Backup file %s is missing", path ? path : "");
		return FALSE;
	}
	digest = file_digest(path, &size, error);
	if (digest == NULL)
		return FALSE;
	if (g_strcmp0(digest, recorded) != 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Backup file %s has changed since it was written (sha256 %s, recorded %s)", path, digest, recorded);
		return FALSE;
	}
	c.builder = builder;
	c.mismatches = mismatches;
	c.tie = TRUE;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "run_id");
	json_builder_add_int_value(builder, venture_entity_get_id(run));
	json_builder_set_member_name(builder, "scope");
	json_builder_add_string_value(builder, scope);
	json_builder_set_member_name(builder, "verified_at");
	{
		g_autofree gchar *stamp_text = venture_time_to_string(now);
		json_builder_add_string_value(builder, stamp_text);
	}
	if (g_strcmp0(scope, SCOPE_INSTALLATION) == 0)
		ok = verify_installation_copy(self, &c, path, error);
	else
	{
		g_autofree gchar *payload = NULL;
		ok = g_file_get_contents(path, &payload, NULL, error) &&
			verify_organization_snapshot(self, &c, venture_entity_get_organization_id(run), payload, error);
	}
	if (!ok)
		return FALSE;
	/* Only the first organization's blocks are hoisted to the top level so a
	 * single-organization report reads flat; the array carries every one. */
	{
		g_autoptr(JsonNode) built = NULL;
		JsonObject *object;
		JsonArray *organizations;
		json_builder_set_member_name(builder, "mismatches");
		json_builder_add_value(builder, json_node_init_array(json_node_alloc(), mismatches));
		json_builder_set_member_name(builder, "result");
		json_builder_add_string_value(builder, c.tie ? VERIFICATION_TIE : VERIFICATION_MISMATCH);
		json_builder_end_object(builder);
		built = json_builder_get_root(builder);
		object = json_node_get_object(built);
		organizations = json_object_get_array_member(object, "organizations");
		if (json_array_get_length(organizations) > 0)
		{
			JsonObject *first = json_array_get_object_element(organizations, 0);
			json_object_set_member(object, "trial_balance", json_node_copy(json_object_get_member(first, "trial_balance")));
			json_object_set_member(object, "ledger_totals", json_node_copy(json_object_get_member(first, "ledger_totals")));
			json_object_set_member(object, "document_counts", json_node_copy(json_object_get_member(first, "document_counts")));
		}
		report = venture_json_to_string(built, FALSE);
	}
	g_object_set(target, "verified-at", now, "verification", c.tie ? VERIFICATION_TIE : VERIFICATION_MISMATCH,
		"report", report, NULL);
	return save_owned(self, target, actor, error);
}

/* --- Public entry points ------------------------------------------------- */

VentureEntity *
venture_backup_schedule_service_run(VentureBackupScheduleService *self, VentureBackupSchedule *schedule,
	GDateTime *as_of, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureBackupRun) run = NULL;
	g_autoptr(VentureEntity) fresh = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *scope = NULL, *name = NULL, *directory = NULL, *stamp_text = NULL, *filename = NULL, *path = NULL;
	g_autofree gchar *digest = NULL;
	gint64 org, size = 0;
	gboolean ok;
	g_return_val_if_fail(VENTURE_IS_BACKUP_SCHEDULE_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_BACKUP_SCHEDULE(schedule), NULL);
	if (!enabled(error))
		return NULL;
	if (venture_entity_get_id(VENTURE_ENTITY(schedule)) == 0)
	{
		refuse(error, "save the schedule before running it");
		return NULL;
	}
	now = as_of != NULL ? g_date_time_to_utc(as_of) : venture_time_now();
	org = venture_entity_get_organization_id(VENTURE_ENTITY(schedule));
	scope = text(VENTURE_ENTITY(schedule), "scope");
	name = text(VENTURE_ENTITY(schedule), "name");
	directory = resolve_destination(self, VENTURE_ENTITY(schedule));
	run = venture_backup_run_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(run), org);
	g_object_set(run, "schedule-id", venture_entity_get_id(VENTURE_ENTITY(schedule)), "kind", KIND_BACKUP,
		"name", name, "scope", scope, "started-at", now, "status", STATUS_RUNNING, NULL);
	if (!save_owned(self, VENTURE_ENTITY(run), actor, error))
		return NULL;
	stamp_text = g_date_time_format(now, "%Y%m%dT%H%M%SZ");
	filename = g_strdup_printf("%s-%" G_GINT64_FORMAT "-%s-%" G_GINT64_FORMAT ".%s", scope, org, stamp_text,
		venture_entity_get_id(VENTURE_ENTITY(run)), g_strcmp0(scope, SCOPE_INSTALLATION) == 0 ? "sqlite" : "json");
	path = g_build_filename(directory, filename, NULL);
	if (g_mkdir_with_parents(directory, 0700) != 0)
	{
		g_set_error(&failure, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Could not create %s: %s", directory, g_strerror(errno));
		ok = FALSE;
	}
	else if (g_strcmp0(scope, SCOPE_INSTALLATION) == 0)
		ok = write_installation_copy(self, path, &failure);
	else
		ok = write_organization_snapshot(self, org, path, &failure);
	if (ok)
	{
		digest = file_digest(path, &size, &failure);
		ok = digest != NULL;
	}
	if (!ok)
	{
		g_autoptr(GDateTime) finished = venture_time_now();
		if (!stamp(self, VENTURE_ENTITY(run), STATUS_FAILED, failure->message, finished, actor, error))
			return NULL;
		g_propagate_error(error, g_steal_pointer(&failure));
		return NULL;
	}
	{
		g_autoptr(GDateTime) finished = venture_time_now();
		g_object_set(run, "path", path, "size", size, "sha256", digest, NULL);
		if (!stamp(self, VENTURE_ENTITY(run), STATUS_SUCCEEDED, NULL, finished, actor, error))
			return NULL;
	}
	fresh = venture_database_get(self->database, VENTURE_TYPE_BACKUP_SCHEDULE, venture_entity_get_id(VENTURE_ENTITY(schedule)), error);
	if (fresh == NULL)
		return NULL;
	g_object_set(fresh, "last-run-at", now, NULL);
	if (!venture_database_save(self->database, fresh, actor, error))
		return NULL;
	{
		gboolean verify = FALSE;
		g_object_get(fresh, "verify", &verify, NULL);
		if (verify && !verify_into(self, VENTURE_ENTITY(run), VENTURE_ENTITY(run), actor, &failure))
		{
			/* The backup stands; the verification's failure is recorded on it. */
			g_object_set(run, "message", failure->message, NULL);
			g_clear_error(&failure);
			if (!save_owned(self, VENTURE_ENTITY(run), actor, error))
				return NULL;
		}
	}
	if (!prune(self, fresh, resolve_retention(self, fresh), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&run));
}

gint
venture_backup_schedule_service_run_due(VentureBackupScheduleService *self, gint64 organization_id,
	GDateTime *as_of, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint ran = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_BACKUP_SCHEDULE_SERVICE(self), -1);
	if (!enabled(error))
		return -1;
	now = as_of != NULL ? g_date_time_to_utc(as_of) : venture_time_now();
	query = venture_query_new(VENTURE_TYPE_BACKUP_SCHEDULE);
	if (organization_id > 0)
		venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return -1;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *schedule = g_ptr_array_index(rows, i);
		g_autofree gchar *expression = text(schedule, "schedule");
		g_autoptr(GDateTime) last = NULL;
		g_autoptr(VentureEntity) run = NULL;
		g_autoptr(GError) failure = NULL;
		gint due;
		g_object_get(schedule, "last-run-at", &last, NULL);
		due = venture_report_pack_schedule_due(expression, last, now, error);
		if (due < 0)
			return -1;
		if (due == 0)
			continue;
		run = venture_backup_schedule_service_run(self, VENTURE_BACKUP_SCHEDULE(schedule), now, actor, &failure);
		/* A failed backup is on its run record; the sweep goes on. */
		if (run == NULL)
			g_info("Scheduled backup %" G_GINT64_FORMAT " failed: %s", venture_entity_get_id(schedule), failure->message);
		ran++;
	}
	return ran;
}

gboolean
venture_backup_schedule_service_verify(VentureBackupScheduleService *self, VentureBackupRun *run,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_BACKUP_SCHEDULE_SERVICE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_BACKUP_RUN(run), FALSE);
	if (!enabled(error))
		return FALSE;
	return verify_into(self, VENTURE_ENTITY(run), VENTURE_ENTITY(run), actor, error);
}

VentureEntity *
venture_backup_schedule_service_restore_drill(VentureBackupScheduleService *self, VentureBackupRun *run,
	const gchar *name, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureBackupRun) drill = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *scope = NULL, *path = NULL, *sha = NULL;
	g_return_val_if_fail(VENTURE_IS_BACKUP_SCHEDULE_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_BACKUP_RUN(run), NULL);
	if (!enabled(error))
		return NULL;
	scope = text(VENTURE_ENTITY(run), "scope");
	path = text(VENTURE_ENTITY(run), "path");
	sha = text(VENTURE_ENTITY(run), "sha256");
	drill = venture_backup_run_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(drill), venture_entity_get_organization_id(VENTURE_ENTITY(run)));
	g_object_set(drill, "schedule-id", number(VENTURE_ENTITY(run), "schedule-id"), "source-run-id", venture_entity_get_id(VENTURE_ENTITY(run)),
		"kind", KIND_DRILL, "name", venture_string_is_empty(name) ? "Restore drill" : name, "scope", scope,
		"path", path, "sha256", sha, "size", number(VENTURE_ENTITY(run), "size"), "started-at", now, "status", STATUS_RUNNING, NULL);
	if (!save_owned(self, VENTURE_ENTITY(drill), actor, error))
		return NULL;
	if (!verify_into(self, VENTURE_ENTITY(run), VENTURE_ENTITY(drill), actor, &failure))
	{
		g_autoptr(GDateTime) finished = venture_time_now();
		if (!stamp(self, VENTURE_ENTITY(drill), STATUS_FAILED, failure->message, finished, actor, error))
			return NULL;
		g_propagate_error(error, g_steal_pointer(&failure));
		return NULL;
	}
	{
		g_autoptr(GDateTime) finished = venture_time_now();
		if (!stamp(self, VENTURE_ENTITY(drill), STATUS_SUCCEEDED, NULL, finished, actor, error))
			return NULL;
	}
	return VENTURE_ENTITY(g_steal_pointer(&drill));
}

VentureEntity *
venture_backup_schedule_service_latest_run(VentureBackupScheduleService *self, gint64 organization_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BACKUP_RUN);
	VentureEntity *run;
	g_return_val_if_fail(VENTURE_IS_BACKUP_SCHEDULE_SERVICE(self), NULL);
	if (!enabled(error))
		return NULL;
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, KIND_BACKUP, NULL);
	venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, STATUS_SUCCEEDED, NULL);
	venture_query_add_order(query, "started-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	run = venture_database_find_one(self->database, query, error);
	if (run == NULL && (error == NULL || *error == NULL))
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"Organization %" G_GINT64_FORMAT " has no succeeded backup to drill", organization_id);
	return run;
}

/* --- Actions: the one door the web page, REST, the CLI and the assistant use */

static gint64
param_id(GHashTable *params, const gchar *name)
{
	JsonNode *node = params ? g_hash_table_lookup(params, name) : NULL;
	const gchar *value;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return 0;
	if (json_node_get_value_type(node) != G_TYPE_STRING)
		return json_node_get_int(node);
	value = json_node_get_string(node);
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
action_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	return enabled(error);
}

static VentureEntity *
run_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureBackupScheduleService *self = venture_action_get_data(action);
	const gchar *as_of_text = param_string(params, "as_of");
	g_autoptr(GDateTime) as_of = NULL;
	if (!venture_string_is_empty(as_of_text))
	{
		as_of = venture_time_from_string(as_of_text, error);
		if (as_of == NULL)
			return NULL;
	}
	return venture_backup_schedule_service_run(self, VENTURE_BACKUP_SCHEDULE(entity), as_of, actor, error);
}

static VentureEntity *
verify_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureBackupScheduleService *self = venture_action_get_data(action);
	(void)params;
	if (!venture_backup_schedule_service_verify(self, VENTURE_BACKUP_RUN(entity), actor, error))
		return NULL;
	return g_object_ref(entity);
}

static VentureEntity *
drill_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureBackupScheduleService *self = venture_action_get_data(action);
	g_autoptr(VentureEntity) source = NULL;
	gint64 run_id = param_id(params, "run_id");
	gint64 org = param_id(params, "organization_id");
	(void)entity;
	if (run_id > 0)
		source = venture_database_get(self->database, VENTURE_TYPE_BACKUP_RUN, run_id, error);
	else
	{
		if (org <= 0)
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
			g_autoptr(VentureEntity) organization = NULL;
			venture_query_add_filter_string(query, "is-default", VENTURE_FILTER_OP_EQ, "true", NULL);
			organization = venture_database_find_one(self->database, query, NULL);
			if (organization == NULL)
			{
				g_clear_object(&query);
				query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
				organization = venture_database_find_one(self->database, query, NULL);
			}
			org = organization != NULL ? venture_entity_get_id(organization) : 1;
		}
		source = venture_backup_schedule_service_latest_run(self, org, error);
	}
	if (source == NULL)
		return NULL;
	return venture_backup_schedule_service_restore_drill(self, VENTURE_BACKUP_RUN(source), param_string(params, "name"), actor, error);
}

void
venture_backup_schedule_actions_register(VentureDatabase *database)
{
	VentureBackupScheduleService *self = venture_backup_schedule_service_get(database);
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	g_autoptr(GError) error = NULL;
	if (self->actions)
		return;
	self->actions = TRUE;
	{
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_ptr_array_add(parameters, venture_field_spec_new("as_of", "As of", VENTURE_FIELD_KIND_DATETIME));
		action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "backup_schedule", "name", "run",
			"label", "Run backup now", "description", "Write this schedule's backup, verify it if the schedule says so, and apply retention",
			"parameters", parameters, "stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_ADMIN, NULL);
		if (!venture_action_registry_register(registry, action, action_allowed, run_invoke, self, NULL, &error))
			g_error("Backup action registration: %s", error->message);
	}
	{
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "backup_run", "name", "verify",
			"label", "Verify", "description", "Restore this backup into a temporary empty database and tie it to the live books",
			"parameters", parameters, "stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_ADMIN, NULL);
		if (!venture_action_registry_register(registry, action, action_allowed, verify_invoke, self, NULL, &error))
			g_error("Backup action registration: %s", error->message);
	}
	{
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		g_ptr_array_add(parameters, venture_field_spec_new("run_id", "Backup run", VENTURE_FIELD_KIND_INTEGER));
		g_ptr_array_add(parameters, venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER));
		g_ptr_array_add(parameters, venture_field_spec_new("name", "Drill name", VENTURE_FIELD_KIND_STRING));
		action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "backup_run", "name", "restore_drill",
			"label", "Restore drill", "description", "Restore a backup (the latest by default) into an empty database, tie it out, and keep the report as a named drill",
			"parameters", parameters, "stageable", FALSE, "type-level", TRUE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_ADMIN, NULL);
		if (!venture_action_registry_register(registry, action, action_allowed, drill_invoke, self, NULL, &error))
			g_error("Backup action registration: %s", error->message);
	}
}
