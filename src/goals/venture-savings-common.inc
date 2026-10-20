/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* One validation contract for generic writes and report-time rechecks. */
static gboolean
savings_check(VentureDatabase *db, VentureEntity *goal, GError **error)
{
	gint64 account_id = 0, envelope_id = 0;
	g_autoptr(VentureMoney) target = NULL, monthly = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_object_get(goal, "savings-account-id", &account_id, "savings-envelope-id", &envelope_id,
		"savings-target", &target, "savings-monthly", &monthly, NULL);
	if (account_id == 0 && envelope_id == 0 && target == NULL && monthly == NULL) return TRUE;
	if (account_id < 0 || envelope_id < 0 || (account_id > 0) == (envelope_id > 0) ||
		target == NULL || target->amount <= 0 ||
		(monthly != NULL && (monthly->amount < 0 || g_strcmp0(monthly->currency, target->currency) != 0)))
		goto invalid;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "journal") == 0 ||
		venture_entity_registry_lookup(venture_entity_registry_get_default(), account_id > 0 ? "account" : "budget_line") == 0)
		goto invalid;
	source = venture_database_get(db, account_id > 0 ? VENTURE_TYPE_ACCOUNT : VENTURE_TYPE_BUDGET_LINE,
		account_id > 0 ? account_id : envelope_id, error);
	if (source == NULL) return FALSE;
	if (venture_entity_is_deleted(source) || venture_entity_get_organization_id(source) != venture_entity_get_organization_id(goal))
		goto invalid;
	if (account_id > 0)
	{
		gint kind = 0;
		g_object_get(source, "kind", &kind, NULL);
		if (kind != VENTURE_ACCOUNT_KIND_ASSET) goto invalid;
	}
	else
	{
		gboolean envelope = FALSE, rollover = FALSE;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureEntity) budget = NULL;
		g_autofree gchar *status = NULL;
		gint64 budget_id = 0;
		g_object_get(source, "envelope", &envelope, "rollover", &rollover, "amount", &amount, "budget-id", &budget_id, NULL);
		if (!envelope || !rollover || amount == NULL || g_strcmp0(amount->currency, target->currency) != 0) goto invalid;
		budget = venture_database_get(db, VENTURE_TYPE_BUDGET, budget_id, error);
		if (budget == NULL) return FALSE;
		g_object_get(budget, "status", &status, NULL);
		if (venture_entity_is_deleted(budget) || venture_entity_get_organization_id(budget) != venture_entity_get_organization_id(goal) ||
			g_strcmp0(status, "active") != 0) goto invalid;
	}
	return TRUE;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Savings goals need exactly one live asset account or active rollover envelope in the same organization, a positive money target and nonnegative monthly contribution in the same currency; ledger and source modules must be enabled");
	return FALSE;
}
