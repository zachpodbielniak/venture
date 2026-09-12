/*
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture.h"
#include "ledger/venture-ledger-private.h"

#include <string.h>

struct _VenturePostingService
{
	GObject parent_instance;
	/* The database owns the canonical service, so the reverse edge is weak. */
	GWeakRef database;
	VenturePostingRuleRegistry *rules;
	GPtrArray *pending;
	GHashTable *active_journals;
	GHashTable *active_sources;
	VentureEntity *write_permit;
	VentureEntity *source_permit;
};

enum { PROP_0, PROP_DATABASE, N_PROPERTIES };
enum { POSTING, POSTED, DATE_POSTABLE, N_SIGNALS };
static guint signals[N_SIGNALS];
G_DEFINE_FINAL_TYPE(VenturePostingService, venture_posting_service, G_TYPE_OBJECT)

static gboolean
ledger_enabled(GError **error)
{
	if (0 != venture_entity_registry_lookup(venture_entity_registry_get_default(), "journal"))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		"The ledger module is disabled");
	return FALSE;
}

static VentureEntity *
copy_record(VentureEntity *entity)
{
	VentureEntity *copy = g_object_new(G_OBJECT_TYPE(entity), NULL);

	venture_entity_copy_properties_from(copy, entity, FALSE);
	return copy;
}

static GPtrArray *
copy_lines(GPtrArray *rows)
{
	GPtrArray *copy = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;

	for (i = 0; i < rows->len; i++)
		g_ptr_array_add(copy, copy_record(g_ptr_array_index(rows, i)));
	return copy;
}

static gboolean
first_error(GSignalInvocationHint *hint, GValue *accumulator, const GValue *value, gpointer data)
{
	(void)hint;
	(void)data;
	if (NULL == g_value_get_boxed(value))
		return TRUE;
	g_value_copy(value, accumulator);
	return FALSE;
}

static void
transaction_finished(VentureDatabase *db, gboolean committed, VenturePostingService *self)
{
	g_autoptr(GPtrArray) pending = NULL;
	guint i;

	(void)db;
	/* Detach before emission: a posted observer may itself post a new journal. */
	pending = self->pending;
	self->pending = g_ptr_array_new_with_free_func(g_object_unref);
	if (committed)
		for (i = 0; i < pending->len; i++)
			g_signal_emit(self, signals[POSTED], 0, g_ptr_array_index(pending, i));
}

static void
venture_posting_service_set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VenturePostingService *self = VENTURE_POSTING_SERVICE(object);

	if (PROP_DATABASE == id)
		g_weak_ref_set(&self->database, g_value_get_object(value));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void
venture_posting_service_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VenturePostingService *self = VENTURE_POSTING_SERVICE(object);

	if (PROP_DATABASE == id)
		g_value_take_object(value, g_weak_ref_get(&self->database));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void
venture_posting_service_constructed(GObject *object)
{
	VenturePostingService *self = VENTURE_POSTING_SERVICE(object);
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);

	G_OBJECT_CLASS(venture_posting_service_parent_class)->constructed(object);
	if (NULL != db)
		g_signal_connect_object(db, "transaction-finished",
			G_CALLBACK(transaction_finished), self, 0);
}
static void
venture_posting_service_finalize(GObject *object)
{
	VenturePostingService *self = VENTURE_POSTING_SERVICE(object);

	g_weak_ref_clear(&self->database);
	g_clear_object(&self->rules);
	g_ptr_array_unref(self->pending);
	g_hash_table_unref(self->active_journals);
	g_hash_table_unref(self->active_sources);
	G_OBJECT_CLASS(venture_posting_service_parent_class)->finalize(object);
}
static void
venture_posting_service_class_init(VenturePostingServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->set_property = venture_posting_service_set_property;
	object_class->get_property = venture_posting_service_get_property;
	object_class->constructed = venture_posting_service_constructed;
	object_class->finalize = venture_posting_service_finalize;
	g_object_class_install_property(object_class, PROP_DATABASE,
		g_param_spec_object("database", "Database", "Owning ledger database",
			VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	/**
	 * VenturePostingService::posting:
	 * @self: the service
	 * @journal: detached header snapshot
	 * @lines: (element-type VentureJournalLine): detached valued line snapshots
	 *
	 * RUN_LAST, after validation and date-postable, before any journal writes.
	 * Snapshot edits have no effect. The first non-NULL error stops emission.
	 * Returns: (transfer full) (nullable): a veto error, or NULL to allow
	 */
	signals[POSTING] = g_signal_new("posting", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, first_error, NULL, NULL, G_TYPE_ERROR, 2,
		VENTURE_TYPE_JOURNAL, G_TYPE_PTR_ARRAY);
	/**
	 * VenturePostingService::posted:
	 * @self: the service
	 * @journal: committed journal snapshot
	 *
	 * Emitted in posting order after the outermost successful commit. Rollback
	 * discards queued emissions. Accounting writes and their audit precede it.
	 */
	signals[POSTED] = g_signal_new("posted", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, VENTURE_TYPE_JOURNAL);
	/**
	 * VenturePostingService::date-postable:
	 * @self: the service
	 * @organization_id: legal entity
	 * @when: accounting date
	 *
	 * Period/close implementations connect here. Emitted inside the posting
	 * transaction before posting. No period policy is implied by an empty hook.
	 * Returns: (transfer full) (nullable): a veto error, or NULL
	 */
	signals[DATE_POSTABLE] = g_signal_new("date-postable", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, first_error, NULL, NULL, G_TYPE_ERROR, 2,
		G_TYPE_INT64, G_TYPE_DATE_TIME);
}
static void
venture_posting_service_init(VenturePostingService *self)
{
	g_weak_ref_init(&self->database, NULL);
	self->pending = g_ptr_array_new_with_free_func(g_object_unref);
	self->rules = venture_posting_rule_registry_new();
	self->active_journals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->active_sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	venture_ledger_register_rules(self->rules);
}

VenturePostingService *
venture_database_get_posting_service(VentureDatabase *db)
{
	VenturePostingService *service;

	g_return_val_if_fail(VENTURE_IS_DATABASE(db), NULL);
	service = g_object_get_data(G_OBJECT(db), "venture-posting-service");
	if (NULL == service)
	{
		service = g_object_new(VENTURE_TYPE_POSTING_SERVICE, "database", db, NULL);
		g_object_set_data_full(G_OBJECT(db), "venture-posting-service", service, g_object_unref);
	}
	return service;
}
VenturePostingService *
venture_context_get_posting_service(VentureContext *context)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	if (!venture_context_module_enabled(context, "ledger"))
		return NULL;
	return venture_database_get_posting_service(venture_context_get_database(context));
}
VenturePostingRuleRegistry *
venture_posting_service_get_rules(VenturePostingService *self)
{
	g_return_val_if_fail(VENTURE_IS_POSTING_SERVICE(self), NULL);
	return self->rules;
}

static gboolean
permission_error(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		"Posted journals, their lines and ledger projections are immutable; use a reversing journal");
	return FALSE;
}

static VentureEntity *
required_record(VentureDatabase *db, GType type, gint64 id, GError **error)
{
	VentureEntity *entity = venture_database_get(db, type, id, error);

	if (NULL == entity && (NULL == error || NULL == *error))
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"Required %s #%" G_GINT64_FORMAT " does not exist", g_type_name(type), id);
	return entity;
}

static gboolean
parent_is_draft(VentureDatabase *db, VentureEntity *line, GError **error)
{
	g_autoptr(VentureEntity) parent = NULL;
	gint64 id;
	VentureJournalState state;
	VenturePostingService *service = g_object_get_data(G_OBJECT(db), "venture-posting-service");

	g_object_get(line, "journal-id", &id, NULL);
	parent = required_record(db, VENTURE_TYPE_JOURNAL, id, error);
	if (NULL == parent)
		return FALSE;
	g_object_get(parent, "state", &state, NULL);
	if (VENTURE_JOURNAL_DRAFT != state || venture_entity_is_deleted(parent) ||
		(NULL != service && g_hash_table_contains(service->active_journals, venture_entity_get_uuid(parent))))
		return permission_error(error);
	return TRUE;
}

gboolean
venture_ledger_check_write(VentureDatabase *db, VentureEntity *entity,
	VentureEntity *previous, gboolean removing, GError **error)
{
	g_autoptr(VentureEntity) stored = NULL;
	VenturePostingService *service;

	service = g_object_get_data(G_OBJECT(db), "venture-posting-service");
	if (NULL != service && service->write_permit == entity && !removing)
	{
		/* Consume BEFORE generic validators or signals can re-enter. */
		service->write_permit = NULL;
		return TRUE;
	}
	if (VENTURE_IS_LEDGER_ENTRY(entity))
		return permission_error(error);
	if (!VENTURE_IS_JOURNAL(entity) && !VENTURE_IS_JOURNAL_LINE(entity))
		return TRUE;
	if (NULL == previous && venture_entity_is_persisted(entity))
	{
		stored = required_record(db, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
		if (NULL == stored)
			return FALSE;
		previous = stored;
	}
	if (VENTURE_IS_JOURNAL(entity))
	{
		VentureJournalState state;
		gint64 reverses;
		g_autoptr(GDateTime) posted_at = NULL;

		if (NULL != service &&
			(g_hash_table_contains(service->active_journals, venture_entity_get_uuid(entity)) ||
			 (NULL != previous && g_hash_table_contains(service->active_journals, venture_entity_get_uuid(previous)))))
			return permission_error(error);
		g_object_get(entity, "state", &state, "reverses-id", &reverses,
			"posted-at", &posted_at, NULL);
		if (VENTURE_JOURNAL_DRAFT != state || reverses != 0 || NULL != posted_at)
			return permission_error(error);
		if (NULL != previous)
		{
			g_object_get(previous, "state", &state, NULL);
			if (VENTURE_JOURNAL_DRAFT != state)
				return permission_error(error);
		}
		return TRUE;
	}
	if (NULL != previous && !parent_is_draft(db, previous, error))
		return FALSE;
	return parent_is_draft(db, entity, error);
}

static gboolean
save_authorized(VenturePostingService *self, VentureDatabase *db,
	VentureEntity *entity, const VentureActor *actor, GError **error)
{
	gboolean ok;
	g_autoptr(VentureEntity) expected = copy_record(entity);
	g_autoptr(JsonNode) diff = NULL;

	self->write_permit = entity;
	ok = venture_database_save(db, entity, actor, error);
	self->write_permit = NULL;
	if (ok)
	{
		/* Generic validators may supply defaults for ordinary records. They
		 * cannot change values that the posting service already balanced. */
		diff = venture_entity_diff(expected, entity);
		if (json_object_get_size(json_node_get_object(diff)) != 0 ||
			venture_entity_is_deleted(entity))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"A save hook changed the validated journal; nothing was posted");
			return FALSE;
		}
	}
	return ok;
}

gboolean
venture_posting_service_is_date_postable(VenturePostingService *self,
	gint64 org, GDateTime *when, GError **error)
{
	GError *veto_error = NULL;

	g_return_val_if_fail(VENTURE_IS_POSTING_SERVICE(self), FALSE);
	if (!ledger_enabled(error))
		return FALSE;
	if (org <= 0 || NULL == when)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A journal requires a legal entity and accounting date");
		return FALSE;
	}
	g_signal_emit(self, signals[DATE_POSTABLE], 0, org, when, &veto_error);
	if (NULL == veto_error)
		return TRUE;
	g_propagate_error(error, veto_error);
	return FALSE;
}

static GPtrArray *
journal_lines(VentureDatabase *db, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ, id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(db, query, error);
}

static gboolean
validate_header(VenturePostingService *self, VentureDatabase *db,
	VentureJournal *header, gboolean reversing, GError **error)
{
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autofree gchar *source_type = NULL;
	g_autofree gchar *currency = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org = venture_entity_get_organization_id(VENTURE_ENTITY(header));
	gint64 source_id;
	gint64 reverses_id;
	GType source_gtype;
	VentureJournalState state;

	g_object_get(header, "source-type", &source_type, "source-id", &source_id,
		"occurred-at", &when, "currency", &currency, "state", &state,
		"reverses-id", &reverses_id, NULL);
	if (VENTURE_JOURNAL_DRAFT != state || venture_entity_is_deleted(VENTURE_ENTITY(header)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Only a live draft can be posted");
		return FALSE;
	}
	if (!reversing && reverses_id != 0)
		return permission_error(error);
	if (!venture_posting_service_is_date_postable(self, org, when, error))
		return FALSE;
	organization = required_record(db, VENTURE_TYPE_ORGANIZATION, org, error);
	if (NULL == organization)
		return FALSE;
	source_gtype = (NULL != source_type) ? venture_entity_registry_lookup_any(
		venture_entity_registry_get_default(), source_type) : 0;
	if (venture_entity_is_deleted(organization) || !source_gtype || source_id <= 0 ||
		NULL == currency || strlen(currency) != 3)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A journal requires an existing source document, legal entity and three-letter currency");
		return FALSE;
	}
	source = required_record(db, source_gtype, source_id, error);
	if (NULL == source)
		return FALSE;
	if (!reversing && (venture_entity_is_deleted(source) ||
		(VENTURE_IS_ORGANIZATION(source) ? source_id != org :
		 venture_entity_get_organization_id(source) != org)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"The source must belong to the journal's legal entity");
		return FALSE;
	}
	return TRUE;
}

static gboolean
value_lines(VentureDatabase *db, VentureJournal *journal, GPtrArray *rows,
	VentureExchangePolicy *policy, gboolean reversing, GError **error)
{
	g_autoptr(VentureMoney) debits = NULL;
	g_autoptr(VentureMoney) credits = NULL;
	g_autofree gchar *currency = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org = venture_entity_get_organization_id(VENTURE_ENTITY(journal));
	guint i;

	if (rows->len < 2)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A journal needs at least two lines");
		return FALSE;
	}
	g_object_get(journal, "currency", &currency, "occurred-at", &when, NULL);
	debits = venture_money_new_zero(currency);
	credits = venture_money_new_zero(currency);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) acct = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) valued = NULL;
		g_autoptr(VentureMoney) total = NULL;
		VentureLedgerSide side;
		gint64 account_id;
		gint64 line_org;
		gboolean active;
		VentureMoney **sum;

		if (!VENTURE_IS_JOURNAL_LINE(row))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected journal lines");
			return FALSE;
		}
		line_org = venture_entity_get_organization_id(row);
		g_object_get(row, "amount", &amount, "side", &side, "account-id", &account_id, NULL);
		if (NULL == amount || amount->amount < 0 || (line_org != 0 && line_org != org) ||
			(side != VENTURE_LEDGER_SIDE_DEBIT && side != VENTURE_LEDGER_SIDE_CREDIT))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Lines require nonnegative money, a debit/credit side and the journal's legal entity");
			return FALSE;
		}
		acct = required_record(db, VENTURE_TYPE_ACCOUNT, account_id, error);
		if (NULL == acct)
			return FALSE;
		g_object_get(acct, "active", &active, NULL);
		if ((!active && !reversing) || venture_entity_is_deleted(acct) ||
			venture_entity_get_organization_id(acct) != org)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"The account must be active and belong to the journal's legal entity");
			return FALSE;
		}
		if (reversing)
			g_object_get(row, "book-amount", &valued, NULL);
		else if (0 == g_strcmp0(currency, amount->currency))
			valued = venture_money_copy(amount);
		else if (NULL != policy)
			valued = venture_exchange_policy_convert(policy, amount, currency, when, error);
		else
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Mixed currencies require an explicit exchange policy");
			return FALSE;
		}
		if (NULL == valued)
		{
			if (NULL == error || NULL == *error)
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The exchange policy returned no valuation");
			return FALSE;
		}
		if (valued->amount < 0 || 0 != g_strcmp0(valued->currency, currency))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"The exchange policy must return nonnegative money in the book currency");
			return FALSE;
		}
		sum = (side == VENTURE_LEDGER_SIDE_DEBIT) ? &debits : &credits;
		total = venture_money_add(*sum, valued, error);
		if (NULL == total)
			return FALSE;
		g_clear_pointer(sum, venture_money_free);
		*sum = g_steal_pointer(&total);
		g_object_set(row, "book-amount", valued, "organization-id", org, NULL);
	}
	if (!venture_money_equal(debits, credits))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_BALANCE,
			"The journal's valued debits and credits do not balance");
		return FALSE;
	}
	if (NULL != policy)
	{
		const gchar *name = venture_exchange_policy_get_name(policy);

		if (NULL == name || '\0' == *name)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Exchange policy must identify its rate set");
			return FALSE;
		}
		g_object_set(journal, "exchange-policy", name, NULL);
	}
	else if (!reversing)
		g_object_set(journal, "exchange-policy", NULL, NULL);
	return TRUE;
}

static gboolean
save_projection(VenturePostingService *self, VentureDatabase *db, VentureJournal *journal,
	VentureEntity *row, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureLedgerEntry) entry = venture_ledger_entry_new();
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *source_type = NULL;
	g_autofree gchar *memo = NULL;
	gint64 account_id;
	gint64 source_id;
	VentureLedgerSide side;

	g_object_get(journal, "occurred-at", &when, "source-type", &source_type, "source-id", &source_id, NULL);
	g_object_get(row, "book-amount", &amount, "side", &side, "account-id", &account_id, "memo", &memo, NULL);
	g_object_set(entry, "transaction-id", venture_entity_get_uuid(VENTURE_ENTITY(journal)),
		"journal-line-id", venture_entity_get_id(row), "account-id", account_id,
		"amount", amount, "side", side, "occurred-at", when, "memo", memo,
		"source-type", source_type, "source-id", source_id,
		"organization-id", venture_entity_get_organization_id(VENTURE_ENTITY(journal)), NULL);
	return save_authorized(self, db, VENTURE_ENTITY(entry), actor, error);
}

static VentureJournal *
post_internal(VenturePostingService *self, VentureJournal *input, GPtrArray *input_lines,
	VentureExchangePolicy *policy, const VentureActor *actor, gboolean reversing, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureJournal) journal = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *active_uuid = NULL;
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(input));
	guint i;

	if (!ledger_enabled(error) || NULL == db)
		return NULL;
	if (!venture_database_begin(db, error))
		return NULL;
	if (id != 0)
	{
		VentureJournalState state;

		stored = required_record(db, VENTURE_TYPE_JOURNAL, id, error);
		if (NULL == stored)
			goto fail;
		g_object_get(stored, "state", &state, NULL);
		if (state != VENTURE_JOURNAL_DRAFT ||
			venture_entity_get_version(stored) != venture_entity_get_version(VENTURE_ENTITY(input)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"The journal is already posted or the draft changed");
			goto fail;
		}
	}
	if ((id != 0 && NULL != input_lines) || (id == 0 && NULL == input_lines))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"New journals take unsaved lines; saved drafts post all their saved lines");
		goto fail;
	}
	journal = VENTURE_JOURNAL(copy_record(VENTURE_ENTITY(input)));
	if (NULL != input_lines)
	{
		for (i = 0; i < input_lines->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(input_lines, i);
			gint64 parent;

			if (!VENTURE_IS_JOURNAL_LINE(row) || venture_entity_is_persisted(row))
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "New journals take new journal lines");
				goto fail;
			}
			g_object_get(row, "journal-id", &parent, NULL);
			if (parent != 0)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A new line cannot belong to another journal");
				goto fail;
			}
		}
		rows = copy_lines(input_lines);
	}
	else
		rows = journal_lines(db, id, error);
	if (NULL == rows || !validate_header(self, db, journal, reversing, error) ||
		!value_lines(db, journal, rows, policy, reversing, error))
		goto fail;
	if (g_hash_table_contains(self->active_journals, venture_entity_get_uuid(VENTURE_ENTITY(journal))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "This journal is already being posted");
		goto fail;
	}
	active_uuid = g_strdup(venture_entity_get_uuid(VENTURE_ENTITY(journal)));
	g_hash_table_add(self->active_journals, g_strdup(active_uuid));
	{
		GError *veto_error = NULL;
		g_autoptr(VentureEntity) snapshot = copy_record(VENTURE_ENTITY(journal));
		g_autoptr(GPtrArray) snapshots = copy_lines(rows);

		g_signal_emit(self, signals[POSTING], 0, snapshot, snapshots, &veto_error);
		if (NULL != veto_error)
		{
			g_propagate_error(error, veto_error);
			goto fail;
		}
	}
	if (!save_authorized(self, db, VENTURE_ENTITY(journal), actor, error))
		goto fail;
	id = venture_entity_get_id(VENTURE_ENTITY(journal));
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);

		g_object_set(row, "journal-id", id, NULL);
		if (!save_authorized(self, db, row, actor, error) ||
			!save_projection(self, db, journal, row, actor, error))
			goto fail;
	}
	now = venture_time_now();
	g_object_set(journal, "state", VENTURE_JOURNAL_POSTED, "posted-at", now, NULL);
	if (!save_authorized(self, db, VENTURE_ENTITY(journal), actor, error))
		goto fail;
	g_ptr_array_add(self->pending, copy_record(VENTURE_ENTITY(journal)));
	g_hash_table_remove(self->active_journals, active_uuid);
	if (!venture_database_commit(db, error))
		return NULL;
	return g_steal_pointer(&journal);
fail:
	if (NULL != active_uuid)
		g_hash_table_remove(self->active_journals, active_uuid);
	venture_database_rollback(db);
	return NULL;
}

VentureJournal *
venture_posting_service_post(VenturePostingService *self, VentureJournal *journal,
	GPtrArray *rows, VentureExchangePolicy *policy, const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_POSTING_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_JOURNAL(journal), NULL);
	g_return_val_if_fail(NULL == policy || VENTURE_IS_EXCHANGE_POLICY(policy), NULL);
	return post_internal(self, journal, rows, policy, actor, FALSE, error);
}

GPtrArray *
venture_posting_service_find_source(VenturePostingService *self, const gchar *source_type,
	gint64 source_id, gint64 org, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);

	if (!ledger_enabled(error) || NULL == db)
		return NULL;
	if (NULL == source_type || source_id <= 0 || org <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Source lookup requires a type, id and legal entity");
		return NULL;
	}
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "source-type", VENTURE_FILTER_OP_EQ, source_type, NULL);
	venture_query_add_filter_int(query, "source-id", VENTURE_FILTER_OP_EQ, source_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(db, query, error);
}

VentureJournal *
venture_posting_service_reverse(VenturePostingService *self, gint64 journal_id,
	GDateTime *when, const gchar *memo, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(VentureJournal) draft = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) original_rows = NULL;
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) original_date = NULL;
	VentureJournalState state;
	guint i;

	if (!ledger_enabled(error) || NULL == db || !venture_database_begin(db, error))
		return NULL;
	original = required_record(db, VENTURE_TYPE_JOURNAL, journal_id, error);
	if (NULL == original)
		goto fail;
	g_object_get(original, "state", &state, "occurred-at", &original_date, NULL);
	if (state != VENTURE_JOURNAL_POSTED)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Only a posted journal can be reversed once");
		goto fail;
	}
	if (NULL == when || g_date_time_compare(when, original_date) < 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A reversal cannot precede its original");
		goto fail;
	}
	draft = VENTURE_JOURNAL(venture_entity_duplicate(original));
	g_object_set(draft, "state", VENTURE_JOURNAL_DRAFT, "reverses-id", journal_id,
		"posted-at", NULL, "occurred-at", when, "memo", memo, "rule-name", "reversal",
		"posting-key", NULL, NULL);
	original_rows = journal_lines(db, journal_id, error);
	if (NULL == original_rows)
		goto fail;
	for (i = 0; i < original_rows->len; i++)
	{
		VentureEntity *row = venture_entity_duplicate(g_ptr_array_index(original_rows, i));
		VentureLedgerSide side;

		g_object_get(row, "side", &side, NULL);
		g_object_set(row, "journal-id", (gint64)0, "side",
			side == VENTURE_LEDGER_SIDE_DEBIT ? VENTURE_LEDGER_SIDE_CREDIT : VENTURE_LEDGER_SIDE_DEBIT, NULL);
		g_ptr_array_add(rows, row);
	}
	/* Stored valuations are reversed exactly; today's rates cannot rewrite yesterday. */
	posted = post_internal(self, draft, rows, NULL, actor, TRUE, error);
	if (NULL == posted)
		goto fail;
	g_object_set(original, "state", VENTURE_JOURNAL_REVERSED, NULL);
	if (!save_authorized(self, db, original, actor, error))
		goto fail;
	if (!venture_database_commit(db, error))
		return NULL;
	return g_steal_pointer(&posted);
fail:
	venture_database_rollback(db);
	return NULL;
}

VentureMoney *
venture_posting_service_account_balance(VenturePostingService *self, gint64 account_id,
	gint64 org, const gchar *currency, GDateTime *as_of, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	guint i;

	if (!ledger_enabled(error) || NULL == db)
		return NULL;
	if (account_id <= 0 || org <= 0 || NULL == currency || strlen(currency) != 3 || NULL == as_of)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Balance requires account, legal entity, currency and as-of date");
		return NULL;
	}
	if (!venture_database_begin(db, error))
		return NULL;
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, org);
	journals = venture_database_find(db, query, error);
	if (NULL == journals)
		goto fail;
	balance = venture_money_new_zero(currency);
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(GDateTime) when = NULL;
		g_autofree gchar *book_currency = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		VentureJournalState state;
		guint j;

		g_object_get(journal, "state", &state, "occurred-at", &when, "currency", &book_currency, NULL);
		if ((state != VENTURE_JOURNAL_POSTED && state != VENTURE_JOURNAL_REVERSED) ||
			NULL == when || g_date_time_compare(when, as_of) > 0 || g_strcmp0(currency, book_currency) != 0)
			continue;
		rows = journal_lines(db, venture_entity_get_id(journal), error);
		if (NULL == rows)
			goto fail;
		for (j = 0; j < rows->len; j++)
		{
			VentureEntity *row = g_ptr_array_index(rows, j);
			gint64 acct;
			VentureLedgerSide side;
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(VentureMoney) total = NULL;

			g_object_get(row, "account-id", &acct, "side", &side, "book-amount", &amount, NULL);
			if (acct != account_id)
				continue;
			total = side == VENTURE_LEDGER_SIDE_DEBIT ? venture_money_add(balance, amount, error) :
				venture_money_subtract(balance, amount, error);
			if (NULL == total)
				goto fail;
			g_clear_pointer(&balance, venture_money_free);
			balance = g_steal_pointer(&total);
		}
	}
	if (!venture_database_commit(db, error))
		return NULL;
	return g_steal_pointer(&balance);
fail:
	venture_database_rollback(db);
	return NULL;
}

static VentureJournal *
document_header(VentureEntity *source, const gchar *rule_name, GPtrArray *rows, GError **error)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *memo = venture_entity_get_display_name(source);
	const gchar *date_field;

	if (rows->len == 0 || !VENTURE_IS_JOURNAL_LINE(g_ptr_array_index(rows, 0)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The posting rule returned no journal lines");
		return NULL;
	}
	date_field = VENTURE_IS_INVOICE(source) ? "paid-at" : "occurred-at";
	if (NULL != g_object_class_find_property(G_OBJECT_GET_CLASS(source), date_field))
		g_object_get(source, date_field, &when, NULL);
	if (NULL == when)
		g_object_get(source, "created-at", &when, NULL);
	g_object_get(g_ptr_array_index(rows, 0), "amount", &amount, NULL);
	g_object_set(journal, "source-type", venture_entity_get_entity_name(source),
		"source-id", venture_entity_get_id(source), "source-version", venture_entity_get_version(source),
		"organization-id", VENTURE_IS_ORGANIZATION(source) ? venture_entity_get_id(source) :
			venture_entity_get_organization_id(source),
		"occurred-at", when, "currency", NULL != amount ? amount->currency : NULL,
		"memo", memo, "rule-name", rule_name, NULL);
	return g_steal_pointer(&journal);
}

static GPtrArray *
build_document_lines(VenturePostingService *self, VentureDatabase *db,
	const gchar *name, VentureEntity *source, GError **error)
{
	g_autoptr(VenturePostingRule) rule = NULL;
	VenturePostingRule *registered = venture_posting_rule_registry_lookup(self->rules, name);

	if (NULL == registered)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No posting rule named %s", name);
		return NULL;
	}
	/* A rule may replace its registry entry while running. */
	rule = g_object_ref(registered);
	return venture_posting_rule_build_lines(rule, db, source, error);
}

VentureJournal *
venture_posting_service_post_document(VenturePostingService *self, const gchar *name,
	VentureEntity *source, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureJournal) journal = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) existing = NULL;
	guint i;

	if (!ledger_enabled(error) || NULL == db || !venture_database_begin(db, error))
		return NULL;
	current = required_record(db, G_OBJECT_TYPE(source), venture_entity_get_id(source), error);
	if (NULL == current)
		goto fail;
	if (venture_entity_get_version(current) != venture_entity_get_version(source))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "The source document changed");
		goto fail;
	}
	rows = build_document_lines(self, db, name, current, error);
	if (NULL == rows)
		goto fail;
	journal = document_header(current, name, rows, error);
	if (NULL == journal)
		goto fail;
	existing = venture_posting_service_find_source(self, venture_entity_get_entity_name(current),
		venture_entity_get_id(current), venture_entity_get_organization_id(VENTURE_ENTITY(journal)), error);
	if (NULL == existing)
		goto fail;
	for (i = 0; i < existing->len; i++)
	{
		g_autofree gchar *rule_name = NULL;
		gint64 version;
		VentureJournalState state;

		g_object_get(g_ptr_array_index(existing, i), "rule-name", &rule_name,
			"source-version", &version, "state", &state, NULL);
		if (state != VENTURE_JOURNAL_DRAFT && g_strcmp0(rule_name, name) == 0 &&
			version == venture_entity_get_version(current))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS, "This document version was already posted by this rule");
			goto fail;
		}
	}
	posted = venture_posting_service_post(self, journal, rows, NULL, actor, error);
	if (NULL == posted)
		goto fail;
	if (!venture_database_commit(db, error))
		return NULL;
	return g_steal_pointer(&posted);
fail:
	venture_database_rollback(db);
	return NULL;
}

gboolean
venture_ledger_wrap_source(VentureDatabase *db, VentureEntity *entity)
{
	VenturePostingService *service = g_object_get_data(G_OBJECT(db), "venture-posting-service");

	if (NULL != service && service->source_permit == entity)
	{
		service->source_permit = NULL;
		return FALSE;
	}
	return (VENTURE_IS_SALE(entity) || VENTURE_IS_EXPENSE(entity)) && ledger_enabled(NULL);
}

static gboolean
same_posting(VentureJournal *a, GPtrArray *a_rows, VentureEntity *b, GPtrArray *b_rows)
{
	g_autoptr(GDateTime) a_date = NULL;
	g_autoptr(GDateTime) b_date = NULL;
	guint i;

	if (a_rows->len != b_rows->len ||
		venture_entity_get_organization_id(VENTURE_ENTITY(a)) != venture_entity_get_organization_id(b))
		return FALSE;
	g_object_get(a, "occurred-at", &a_date, NULL);
	g_object_get(b, "occurred-at", &b_date, NULL);
	if (NULL == a_date || NULL == b_date || !g_date_time_equal(a_date, b_date))
		return FALSE;
	for (i = 0; i < a_rows->len; i++)
	{
		g_autoptr(VentureMoney) a_money = NULL;
		g_autoptr(VentureMoney) b_money = NULL;
		gint64 a_account;
		gint64 b_account;
		VentureLedgerSide a_side;
		VentureLedgerSide b_side;

		g_object_get(g_ptr_array_index(a_rows, i), "amount", &a_money, "account-id", &a_account, "side", &a_side, NULL);
		g_object_get(g_ptr_array_index(b_rows, i), "amount", &b_money, "account-id", &b_account, "side", &b_side, NULL);
		if (a_account != b_account || a_side != b_side || !venture_money_equal(a_money, b_money))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_ledger_save_source(VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	VenturePostingService *self = venture_database_get_posting_service(db);
	g_autoptr(VentureEntity) copy = copy_record(entity);
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GPtrArray) existing = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureJournal) draft = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	const gchar *name = venture_entity_get_entity_name(entity);
	g_autofree gchar *source_uuid = g_strdup(venture_entity_get_uuid(entity));
	guint i;

	if (g_hash_table_contains(self->active_sources, source_uuid))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "This source is already being posted");
		return FALSE;
	}
	if (!venture_database_begin(db, error))
		return FALSE;
	g_hash_table_add(self->active_sources, g_strdup(source_uuid));
	if (venture_entity_is_persisted(entity))
		previous = venture_database_get(db, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
	if (NULL != error && NULL != *error)
		goto fail;
	self->source_permit = copy;
	if (!venture_database_save(db, copy, actor, error))
	{
		self->source_permit = NULL;
		goto fail;
	}
	g_object_get(copy, VENTURE_IS_SALE(copy) ? "gross" : "amount", &amount, NULL);
	/* Incomplete operational records carry no amount yet. Once an amount
	 * exists, every save is held to the posting rules, regardless of surface. */
	if (NULL == amount)
	{
		g_autoptr(VentureMoney) old_amount = NULL;

		if (NULL != previous)
			g_object_get(previous, VENTURE_IS_SALE(copy) ? "gross" : "amount", &old_amount, NULL);
		if (NULL != old_amount)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A posted source cannot lose its amount");
			goto fail;
		}
		goto commit;
	}
	rows = build_document_lines(self, db, name, copy, error);
	if (NULL == rows)
		goto fail;
	draft = document_header(copy, name, rows, error);
	if (NULL == draft)
		goto fail;
	existing = venture_posting_service_find_source(self, name, venture_entity_get_id(copy),
		NULL != previous ? venture_entity_get_organization_id(previous) :
			venture_entity_get_organization_id(copy), error);
	if (NULL == existing)
		goto fail;
	for (i = 0; i < existing->len; i++)
	{
		VentureEntity *old = g_ptr_array_index(existing, i);
		g_autofree gchar *old_rule = NULL;
		VentureJournalState state;
		g_autoptr(GPtrArray) old_rows = NULL;
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(GDateTime) old_when = NULL;
		g_autoptr(VentureJournal) reversed = NULL;

		g_object_get(old, "state", &state, "rule-name", &old_rule, NULL);
		if (state != VENTURE_JOURNAL_POSTED || g_strcmp0(old_rule, name) != 0)
			continue;
		old_rows = journal_lines(db, venture_entity_get_id(old), error);
		if (NULL == old_rows)
			goto fail;
		if (same_posting(draft, rows, old, old_rows))
			goto commit;
		g_object_get(draft, "occurred-at", &when, NULL);
		g_object_get(old, "occurred-at", &old_when, NULL);
		/* Correcting an earlier accounting date cannot place the reversal
		 * before the original. Both date checks still run through signals. */
		reversed = venture_posting_service_reverse(self, venture_entity_get_id(old),
			g_date_time_compare(when, old_when) < 0 ? old_when : when,
			"Source document correction", actor, error);
		if (NULL == reversed)
			goto fail;
	}
	posted = venture_posting_service_post(self, draft, rows, NULL, actor, error);
	if (NULL == posted)
		goto fail;
commit:
	g_hash_table_remove(self->active_sources, source_uuid);
	if (!venture_database_commit(db, error))
		return FALSE;
	venture_entity_copy_properties_from(entity, copy, FALSE);
	return TRUE;
fail:
	g_hash_table_remove(self->active_sources, source_uuid);
	venture_database_rollback(db);
	return FALSE;
}

gboolean
venture_posting_service_post_entries(VenturePostingService *self, GPtrArray *entries,
	VentureExchangePolicy *policy, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureJournal) draft = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *transaction = NULL;
	g_autofree gchar *source_type = NULL;
	g_autofree gchar *posting_key = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 source_id;
	gint64 org;
	guint i;
	gboolean dated;
	VentureEntity *first;

	if (!ledger_enabled(error) || NULL == db)
		return FALSE;
	if (NULL == entries || entries->len == 0 || !VENTURE_IS_LEDGER_ENTRY(g_ptr_array_index(entries, 0)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A batch needs ledger-entry-shaped inputs");
		return FALSE;
	}
	first = g_ptr_array_index(entries, 0);

	org = venture_entity_get_organization_id(first);
	g_object_get(first, "transaction-id", &transaction, "source-type", &source_type,
		"source-id", &source_id, "occurred-at", &when, NULL);
	dated = NULL != when;
	if (NULL == transaction || '\0' == *transaction)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A batch needs a stable transaction-id");
		return FALSE;
	}
	if (!venture_database_begin(db, error))
		return FALSE;
	posting_key = g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, transaction);
	query = venture_query_new(VENTURE_TYPE_JOURNAL);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_string(query, "posting-key", VENTURE_FILTER_OP_EQ, posting_key, NULL);
	existing = venture_database_find_one(db, query, error);
	if (NULL != existing)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS, "This batch was already posted");
		goto fail;
	}
	if (NULL != error && NULL != *error)
		goto fail;
	if (NULL == when)
		when = venture_time_now();
	/* Old manual batches had no source. File these against the actual legal
	 * entity rather than inventing a document type or an unresolvable id. */
	if (NULL == source_type || '\0' == *source_type)
	{
		g_free(source_type);
		source_type = g_strdup("organization");
		source_id = org;
	}
	for (i = 0; i < entries->len; i++)
	{
		VentureEntity *entry = g_ptr_array_index(entries, i);
		g_autoptr(VentureJournalLine) row = venture_journal_line_new();
		g_autoptr(VentureMoney) amount = NULL;
		g_autofree gchar *txn = NULL;
		g_autofree gchar *memo = NULL;
		g_autofree gchar *row_source = NULL;
		g_autoptr(GDateTime) row_date = NULL;
		gint64 row_source_id;
		gint64 acct;
		VentureLedgerSide side;

		if (!VENTURE_IS_LEDGER_ENTRY(entry))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected ledger-entry-shaped inputs");
			goto fail;
		}
		g_object_get(entry, "source-type", &row_source, "source-id", &row_source_id,
			"occurred-at", &row_date, "transaction-id", &txn, "amount", &amount,
			"account-id", &acct, "side", &side, "memo", &memo, NULL);
		if (NULL == row_source || '\0' == *row_source)
		{
			g_free(row_source);
			row_source = g_strdup("organization");
			row_source_id = org;
		}
		if (venture_entity_is_persisted(entry) || org != venture_entity_get_organization_id(entry) ||
			g_strcmp0(txn, transaction) != 0 || g_strcmp0(row_source, source_type) != 0 ||
			row_source_id != source_id || (NULL != row_date) != dated ||
			(NULL != row_date && !g_date_time_equal(row_date, when)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"A batch must contain new lines for one transaction, legal entity, source and date");
			goto fail;
		}
		if (i == 0)
			g_object_set(draft, "currency", NULL != amount ? amount->currency : NULL, NULL);
		g_object_set(row, "amount", amount, "account-id", acct, "side", side,
			"memo", memo, "organization-id", org, NULL);
		g_ptr_array_add(rows, g_steal_pointer(&row));
	}
	g_object_set(draft, "organization-id", org, "occurred-at", when, "source-type", source_type,
		"source-id", source_id, "memo", transaction, "posting-key", posting_key, NULL);
	posted = venture_posting_service_post(self, draft, rows, policy, actor, error);
	if (NULL == posted)
		goto fail;
	return venture_database_commit(db, error);
fail:
	venture_database_rollback(db);
	return FALSE;
}

gboolean
venture_ledger_post_legacy(VentureDatabase *db, GPtrArray *entries,
	const VentureActor *actor, GError **error)
{
	return venture_posting_service_post_entries(venture_database_get_posting_service(db),
		entries, NULL, actor, error);
}
