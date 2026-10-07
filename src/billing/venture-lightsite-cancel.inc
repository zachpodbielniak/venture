/* SPDX-License-Identifier: AGPL-3.0-or-later */
/**
 * venture_lightsite_billing_cancel:
 * @database: hosted database
 * @billing: configured billing organization
 * @business: enrolled customer business
 * @error: return location for an error
 *
 * Cancels an unpublished enrollment strictly before its fourteen-day deadline
 * without a charge. The subscription and immutable receipt commit together;
 * delayed publication, setup and collection cannot revive it. Publication or
 * deadline expiry uses normal period-end cancellation and preserves incurred
 * invoices, including outstanding prepaid installments. Requires the trusted
 * service authority; the hosted client authenticates its dashboard owner.
 *
 * Returns: (transfer full) (nullable): the original cancellation decision
 */
JsonNode *
venture_lightsite_billing_cancel(VentureDatabase *database, gint64 billing,
	gint64 business, GError **error)
{
	g_autoptr(GError) discarded = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(JsonNode) enrollment = NULL, publication = NULL, result = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(VentureBillingRequest) instruction = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), enrolled_at = NULL, deadline = NULL, effective = NULL;
	VentureActor actor = { VENTURE_ACTOR_KIND_IMPORT, "Hosted cancellation", NULL, NULL, NULL };
	JsonObject *object;
	gint64 subscription, invoice;
	gboolean no_charge, prepaid;
	if (!error) error = &discarded;
	if (!prepare(database, billing, NULL, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	if (!business_valid(database, billing, business, error)) return NULL;
	operation = venture_accounting_operation_begin(database, "hosted.cancel", NULL, NULL,
		g_variant_new("(x)", business), billing, &actor, error);
	if (!operation || !venture_database_begin(database, error)) return NULL;
	result = enrollment_read(database, billing, business, "cancel", error);
	if (result) {
		venture_database_rollback(database);
		return g_steal_pointer(&result);
	}
	if (*error) goto fail;
	enrollment = enrollment_read(database, billing, business, "enroll", error);
	if (!enrollment) {
		if (!*error) refuse(error, VENTURE_ERROR_NOT_FOUND, "business is not enrolled");
		goto fail;
	}
	publication = enrollment_read(database, billing, business, "publish", error);
	if (*error) goto fail;
	object = json_node_get_object(enrollment);
	subscription = json_object_get_int_member(object, "subscription_id");
	prepaid = json_object_has_member(object, "payment_path");
	enrolled_at = venture_time_from_string(json_object_get_string_member(object, "enrolled_at"), error);
	if (!enrolled_at) goto fail;
	deadline = g_date_time_add_days(enrolled_at, 14);
	invoice = enrollment_first_invoice(database, billing, subscription, error);
	if (*error) goto fail;
	sub = get_record(database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, subscription, error);
	if (!sub) goto fail;
	no_charge = !publication && g_date_time_compare(now, deadline) < 0 && !invoice;
	if (no_charge && g_strcmp0(enum_nick(sub, "status"), "trialing") &&
		g_strcmp0(enum_nick(sub, "status"), "cancelled")) {
		refuse(error, VENTURE_ERROR_CONFLICT, "enrollment already left its unpaid trial"); goto fail;
	}
	if (strcmp(enum_nick(sub, "status"), "cancelled")) {
		instruction = venture_billing_request_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(instruction), billing);
		g_object_set(instruction, "action", "cancel", "subscription-id", subscription,
			"at-period-end", !no_charge, "at", now, "expected-version", venture_entity_get_version(sub), NULL);
		if (!execute(database, instruction, &actor, error)) goto fail;
	}
	if (no_charge) effective = g_date_time_ref(now);
	else if (prepaid && publication && !invoice) {
		g_autoptr(GDateTime) live = venture_time_from_string(
			json_object_get_string_member(json_node_get_object(publication), "go_live_at"), error);
		if (!live) goto fail;
		effective = g_date_time_add_months(live, 24);
	} else g_object_get(sub, "current-period-end", &effective, NULL);
	result = json_node_new(JSON_NODE_OBJECT); object = json_object_new(); json_node_take_object(result, object);
	json_object_set_int_member(object, "organization_id", business);
	json_object_set_int_member(object, "subscription_id", subscription);
	json_object_set_boolean_member(object, "no_charge", no_charge);
	json_object_set_string_member(object, "state", no_charge ? "cancelled" : "cancel_at_period_end");
	set_time(object, "cancelled_at", now);
	set_time(object, "effective_at", effective);
	if (!enrollment_write(database, billing, business, "cancel", result, &actor, error)) goto fail;
	if (!venture_database_commit(database, error) || !venture_accounting_operation_finish(operation, error)) return NULL;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(database);
	return NULL;
}
