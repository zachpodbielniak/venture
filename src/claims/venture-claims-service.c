/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureClaimsService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
	gboolean busy;
};
G_DEFINE_FINAL_TYPE(VentureClaimsService, venture_claims_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureClaimsService: %s", message);
	return FALSE;
}

static gboolean
module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"expense_claim") != G_TYPE_INVALID;
}

static void
venture_claims_service_class_init(VentureClaimsServiceClass *klass)
{
	(void)klass;
}

static void
venture_claims_service_init(VentureClaimsService *self)
{
	(void)self;
}

VentureClaimsService *
venture_claims_service_get(VentureDatabase *database)
{
	VentureClaimsService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-claims-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_CLAIMS_SERVICE, NULL);
		self->database = database;
		g_object_set_data_full(G_OBJECT(database), "venture-claims-service", self, g_object_unref);
	}
	return self;
}

static gboolean
write_owned(VentureClaimsService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static gboolean
begin_op(VentureClaimsService *self, GError **error)
{
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG, "The claims module is disabled (modules.claims.enabled)");
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "A claims operation is already in progress");
	if (!venture_database_begin(self->database, error))
		return FALSE;
	self->busy = TRUE;
	return TRUE;
}

static gboolean
finish_op(VentureClaimsService *self, gboolean ok, GError **error)
{
	if (ok)
		ok = venture_database_commit(self->database, error);
	else
		venture_database_rollback(self->database);
	self->busy = FALSE;
	return ok;
}

static GPtrArray *
load_lines(VentureClaimsService *self, gint64 claim_id, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EXPENSE_CLAIM_LINE);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "claim-id", VENTURE_FILTER_OP_EQ, claim_id, error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

static gchar *
status_of(VentureEntity *record)
{
	gchar *status = NULL;
	g_object_get(record, "status", &status, NULL);
	return status;
}

static gboolean
guard_date(VentureClaimsService *self, VentureEntity *claim, GError **error)
{
	g_autoptr(GDateTime) date = NULL;
	g_object_get(claim, "claim-date", &date, NULL);
	return venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, venture_entity_get_organization_id(claim), date, error);
}

static gint64
account_id(VentureClaimsService *self, gint64 configured, const gchar *code,
	VentureAccountKind kind, gint64 organization_id, GError **error)
{
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *scoped_code = NULL;
	gboolean active;
	gint actual_kind;

	if (configured == 0)
	{
		const gchar *role = g_str_equal(code, "1000") ? "cash" :
			(g_str_equal(code, "2000") ? "payables" :
			(g_str_equal(code, "6900") ? "expense" : NULL));
		if (role != NULL)
		{
			gint64 mapped = venture_setup_resolve_account(self->database, organization_id,
				role, "organization", 0, NULL, error);
			if (mapped != 0)
				configured = mapped;
			else if (error != NULL && *error != NULL)
				return 0;
		}
	}
	if (configured != 0)
		account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, configured, error);
	else
	{
		query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		venture_query_set_organization(query, organization_id);
		if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
			return 0;
		account = venture_database_find_one(self->database, query, error);
		if (account == NULL && (error == NULL || *error == NULL))
		{
			scoped_code = g_strdup_printf("%" G_GINT64_FORMAT ":%s", organization_id, code);
			g_clear_object(&query);
			query = venture_query_new(VENTURE_TYPE_ACCOUNT);
			venture_query_set_organization(query, organization_id);
			if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped_code, error))
				return 0;
			account = venture_database_find_one(self->database, query, error);
		}
	}
	if (account == NULL && configured == 0 && (error == NULL || *error == NULL))
	{
		account = VENTURE_ENTITY(venture_account_new());
		g_object_set(account, "organization-id", organization_id,
			"code", scoped_code != NULL ? scoped_code : code,
			"name", g_str_equal(code, "6900") ? "General expenses" :
			(g_str_equal(code, "2000") ? "Accounts payable" : "Cash"),
			"kind", kind, "active", TRUE, NULL);
		if (!venture_database_save(self->database, account, NULL, error))
			return 0;
	}
	if (account == NULL)
	{
		if (error == NULL || *error == NULL)
			refuse(error, VENTURE_ERROR_CONFIG, "Configure the cash, payable and expense accounts");
		return 0;
	}
	g_object_get(account, "active", &active, "kind", &actual_kind, NULL);
	if (!active || actual_kind != (gint)kind || venture_entity_get_organization_id(account) != organization_id)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The posting account must be active, of the right class, and in the same organization");
		return 0;
	}
	return venture_entity_get_id(account);
}

static gboolean
add_entry(GPtrArray *entries, gint64 org, const gchar *transaction, gint64 account,
	VentureLedgerSide side, const VentureMoney *amount, GDateTime *date,
	const gchar *source_type, gint64 source_id)
{
	VentureLedgerEntry *entry = venture_ledger_entry_new();
	g_object_set(entry, "transaction-id", transaction, "account-id", account,
		"side", side, "amount", amount, "occurred-at", date,
		"source-type", source_type, "source-id", source_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(entry), org);
	g_ptr_array_add(entries, entry);
	return TRUE;
}

static gboolean
post_payment(VentureClaimsService *self, VentureEntity *claim, GPtrArray *lines,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *settlement = NULL;
	g_autofree gchar *transaction = NULL;
	const gchar *credit_code;
	VentureAccountKind credit_kind;
	gint64 org = venture_entity_get_organization_id(claim);
	gint64 credit;
	guint i;

	g_object_get(claim, "settlement", &settlement, "claim-date", &date, NULL);
	if (g_strcmp0(settlement, "payable") == 0)
	{
		credit_code = "2000";
		credit_kind = VENTURE_ACCOUNT_KIND_LIABILITY;
	}
	else
	{
		credit_code = "1000";
		credit_kind = VENTURE_ACCOUNT_KIND_ASSET;
	}
	credit = account_id(self, 0, credit_code, credit_kind, org, error);
	if (credit == 0)
		return FALSE;
	transaction = g_strdup_printf("claims:pay:%s", venture_entity_get_uuid(claim));
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureMoney) amount = venture_expense_claim_line_get_amount(
			VENTURE_EXPENSE_CLAIM_LINE(line), error);
		gint64 expense = 0;
		gint64 mapped;
		if (amount == NULL)
			return FALSE;
		if (venture_money_is_zero(amount))
			continue;
		g_object_get(line, "account-id", &expense, NULL);
		mapped = expense > 0 ? expense : account_id(self, 0, "6900", VENTURE_ACCOUNT_KIND_EXPENSE, org, error);
		if (mapped == 0)
			return FALSE;
		if (total == NULL)
			total = venture_money_copy(amount);
		else
		{
			g_autoptr(VentureMoney) next = venture_money_add(total, amount, error);
			if (next == NULL)
				return FALSE;
			venture_money_free(total);
			total = g_steal_pointer(&next);
		}
		add_entry(entries, org, transaction, mapped, VENTURE_LEDGER_SIDE_DEBIT, amount, date,
			"expense_claim", venture_entity_get_id(claim));
	}
	if (total == NULL || venture_money_is_zero(total))
		return refuse(error, VENTURE_ERROR_VALIDATION, "A payment needs a positive total");
	add_entry(entries, org, transaction, credit, VENTURE_LEDGER_SIDE_CREDIT, total, date,
		"expense_claim", venture_entity_get_id(claim));
	g_object_set(claim, "total", total, NULL);
	return venture_posting_service_post_entries(venture_database_get_posting_service(self->database),
		entries, NULL, actor, error);
}

static gboolean
freeze_total(VentureClaimsService *self, VentureEntity *claim, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) total = NULL;
	guint i;
	lines = load_lines(self, venture_entity_get_id(claim), venture_entity_get_organization_id(claim), error);
	if (lines == NULL)
		return FALSE;
	if (lines->len == 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Submit a claim with at least one line");
	for (i = 0; i < lines->len; i++)
	{
		g_autoptr(VentureMoney) amount = venture_expense_claim_line_get_amount(
			VENTURE_EXPENSE_CLAIM_LINE(g_ptr_array_index(lines, i)), error);
		if (amount == NULL)
			return FALSE;
		if (total == NULL)
			total = g_steal_pointer(&amount);
		else
		{
			g_autoptr(VentureMoney) next = venture_money_add(total, amount, error);
			if (next == NULL)
				return FALSE;
			venture_money_free(total);
			total = g_steal_pointer(&next);
		}
	}
	g_object_set(claim, "total", total, NULL);
	return TRUE;
}

static gboolean
transition(VentureClaimsService *self, VentureEntity *claim, const gchar *from,
	const gchar *to, gboolean pay, const VentureActor *actor, GError **error)
{
	g_autofree gchar *status = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	if (!begin_op(self, error))
		return FALSE;
	status = status_of(claim);
	if (g_strcmp0(status, from) != 0)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "The claim is not in the expected state");
		return finish_op(self, FALSE, error);
	}
	if (!guard_date(self, claim, error))
		return finish_op(self, FALSE, error);
	if (g_strcmp0(to, "submitted") == 0 && !freeze_total(self, claim, error))
		return finish_op(self, FALSE, error);
	if (pay)
	{
		lines = load_lines(self, venture_entity_get_id(claim),
			venture_entity_get_organization_id(claim), error);
		if (lines == NULL || !post_payment(self, claim, lines, actor, error))
			return finish_op(self, FALSE, error);
	}
	g_object_set(claim, "status", to, NULL);
	if (!write_owned(self, claim, actor, error))
		return finish_op(self, FALSE, error);
	return finish_op(self, TRUE, error);
}

gboolean
venture_claims_service_submit(VentureClaimsService *self, VentureEntity *claim,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CLAIMS_SERVICE(self), FALSE);
	return transition(self, claim, "draft", "submitted", FALSE, actor, error);
}

gboolean
venture_claims_service_approve(VentureClaimsService *self, VentureEntity *claim,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CLAIMS_SERVICE(self), FALSE);
	return transition(self, claim, "submitted", "approved", FALSE, actor, error);
}

gboolean
venture_claims_service_pay(VentureClaimsService *self, VentureEntity *claim,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CLAIMS_SERVICE(self), FALSE);
	return transition(self, claim, "approved", "paid", TRUE, actor, error);
}

static gboolean
hash_taken(VentureClaimsService *self, VentureEntity *line, const gchar *hash, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	gint64 others;
	if (hash == NULL || hash[0] == '\0')
		return FALSE;
	query = venture_query_new(VENTURE_TYPE_EXPENSE_CLAIM_LINE);
	venture_query_set_organization(query, venture_entity_get_organization_id(line));
	if (!venture_query_add_filter_string(query, "receipt-hash", VENTURE_FILTER_OP_EQ, hash, error))
		return TRUE;
	/* Exclude identity, not an assumed match: an edited hash may belong to another receipt. */
	if (venture_entity_is_persisted(line) &&
		!venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_NE,
			venture_entity_get_id(line), error))
		return TRUE;
	others = venture_database_count(self->database, query, error);
	if (others < 0)
		return TRUE;
	if (others > 0)
		return !refuse(error, VENTURE_ERROR_VALIDATION, "duplicate receipt hash refused");
	return FALSE;
}

static gboolean
prepare_line(VentureClaimsService *self, VentureEntity *line, GError **error)
{
	g_autofree gchar *kind = NULL;
	g_autofree gchar *hash = NULL;
	g_autoptr(VentureMoney) computed = NULL;
	gint64 document_id = 0;

	g_object_get(line, "kind", &kind, "receipt-hash", &hash, "document-id", &document_id, NULL);
	if (g_strcmp0(kind, "mileage") == 0)
	{
		computed = venture_expense_claim_line_get_amount(VENTURE_EXPENSE_CLAIM_LINE(line), error);
		if (computed == NULL)
			return FALSE;
		g_object_set(line, "amount", computed, NULL);
	}
	else if (g_strcmp0(kind, "receipt") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A line is a receipt or mileage");
	if (document_id > 0)
	{
		g_autoptr(VentureEntity) document = venture_database_get(self->database, VENTURE_TYPE_DOCUMENT, document_id, error);
		g_autofree gchar *checksum = NULL;
		if (document == NULL)
			return FALSE;
		g_object_get(document, "hash", &checksum, NULL);
		if (checksum != NULL && checksum[0] != '\0')
		{
			g_free(hash);
			hash = g_steal_pointer(&checksum);
			g_object_set(line, "receipt-hash", hash, NULL);
		}
	}
	return !hash_taken(self, line, hash, error);
}

static gboolean
claim_editable(VentureEntity *claim, GError **error)
{
	g_autofree gchar *status = status_of(claim);
	if (g_strcmp0(status, "draft") == 0)
		return TRUE;
	return refuse(error, VENTURE_ERROR_VALIDATION, "Only a draft claim can be edited");
}

gboolean
venture_claims_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureClaimsService *self;
	(void)actor;
	*handled = FALSE;
	if (!VENTURE_IS_EXPENSE_CLAIM(record) && !VENTURE_IS_EXPENSE_CLAIM_LINE(record))
		return TRUE;
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG, "The claims module is disabled (modules.claims.enabled)");
	self = venture_claims_service_get(database);
	if (self->writing == record)
		return TRUE;
	if (VENTURE_IS_EXPENSE_CLAIM(record))
	{
		g_autofree gchar *status = status_of(record);
		if (!venture_entity_is_persisted(record))
		{
			if (status == NULL || status[0] == '\0')
				g_object_set(record, "status", "draft", NULL);
			else if (g_strcmp0(status, "draft") != 0)
				return refuse(error, VENTURE_ERROR_VALIDATION, "New claims start as draft");
			return TRUE;
		}
		{
			g_autoptr(VentureEntity) stored = venture_database_get(database, VENTURE_TYPE_EXPENSE_CLAIM,
				venture_entity_get_id(record), error);
			g_autofree gchar *previous = NULL;
			if (stored == NULL)
				return FALSE;
			g_object_get(stored, "status", &previous, NULL);
			if (g_strcmp0(status, previous) != 0)
				return refuse(error, VENTURE_ERROR_VALIDATION,
					"Submit, approve or pay through VentureClaimsService");
			if (g_strcmp0(previous, "draft") != 0)
				return refuse(error, VENTURE_ERROR_VALIDATION, "Only a draft claim can be edited");
		}
		return TRUE;
	}
	{
		g_autoptr(VentureEntity) claim = NULL;
		gint64 claim_id = 0;
		/* Moving a row must not remove evidence from a submitted claim. */
		if (venture_entity_is_persisted(record))
		{
			g_autoptr(VentureEntity) stored = venture_database_get(database,
				VENTURE_TYPE_EXPENSE_CLAIM_LINE, venture_entity_get_id(record), error);
			g_autoptr(VentureEntity) previous_claim = NULL;
			gint64 previous_id = 0;
			if (stored == NULL)
				return FALSE;
			g_object_get(stored, "claim-id", &previous_id, NULL);
			previous_claim = venture_database_get(database, VENTURE_TYPE_EXPENSE_CLAIM,
				previous_id, error);
			if (previous_claim == NULL || !claim_editable(previous_claim, error))
				return FALSE;
		}
		g_object_get(record, "claim-id", &claim_id, NULL);
		claim = venture_database_get(database, VENTURE_TYPE_EXPENSE_CLAIM, claim_id, error);
		if (claim == NULL)
			return FALSE;
		if (!claim_editable(claim, error))
			return FALSE;
		return prepare_line(self, record, error);
	}
}

gboolean
venture_claims_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	g_autofree gchar *status = NULL;
	if (!VENTURE_IS_EXPENSE_CLAIM(record) && !VENTURE_IS_EXPENSE_CLAIM_LINE(record))
		return TRUE;
	if (!removal)
		return TRUE;
	if (VENTURE_IS_EXPENSE_CLAIM_LINE(record))
	{
		g_autoptr(VentureEntity) claim = NULL;
		gint64 claim_id = 0;
		g_object_get(record, "claim-id", &claim_id, NULL);
		claim = venture_database_get(database, VENTURE_TYPE_EXPENSE_CLAIM, claim_id, error);
		if (claim == NULL)
			return FALSE;
		return claim_editable(claim, error);
	}
	g_object_get(record, "status", &status, NULL);
	if (g_strcmp0(status, "draft") == 0 || g_strcmp0(status, "rejected") == 0)
		return TRUE;
	return refuse(error, VENTURE_ERROR_VALIDATION, "Submitted claims are retained as reimbursement evidence");
}
