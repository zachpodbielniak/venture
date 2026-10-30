/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* An owner switches plan themselves. The new price starts on the next bill:
 * nothing is charged today and nothing is prorated. A switch that charged on
 * confirmation, or a preview that wrote, would break the promise the plans
 * page makes ("the change applies from your next bill"). */

/* A second offered price in the billing organization, under a plan of its own. */
static gint64 switch_price(Fixture *f, gint64 organization, const gchar *interval, const gchar *amount, gboolean active)
{
	g_autoptr(VentureEntity) plan = g_object_new(VENTURE_TYPE_PLAN, "name", "Example annual plan", "active", TRUE, NULL);
	g_autoptr(VentureEntity) price = g_object_new(VENTURE_TYPE_PLAN_PRICE, "active", active, NULL);
	g_autoptr(GError) error = NULL;
	venture_entity_set_organization_id(plan, organization); save(f, plan);
	venture_entity_set_organization_id(price, organization);
	g_object_set(price, "plan-id", venture_entity_get_id(plan), NULL);
	g_assert_true(venture_entity_set_field_from_string(price, "interval", interval, &error));
	g_assert_true(venture_entity_set_field_from_string(price, "amount", amount, &error));
	g_assert_true(venture_entity_set_field_from_string(price, "currency", strchr(amount, ' ') + 1, &error));
	g_assert_no_error(error); save(f, price);
	return venture_entity_get_id(price);
}

/* Replaces the fixture's credential with a fresh token of @user; returns the old one. */
static gchar *switch_use_token(Fixture *f, gint64 user, VentureUserRole role)
{
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *previous = f->token;
	g_object_set(token, "name", "Owner fixture", "user-id", user, "role", role, NULL);
	f->token = venture_api_token_generate(token); save(f, VENTURE_ENTITY(token));
	return previous;
}

static gchar *switch_body(gint64 business, gint64 price, const gchar *accepted)
{
	return accepted ?
		g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT ",\"accepted\":%s}", business, price, accepted) :
		g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT "}", business, price);
}

static VentureEntity *switch_subscription(Fixture *f, gint64 id)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *sub = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, id, &error);
	g_assert_no_error(error); g_assert_nonnull(sub);
	return sub;
}

static guint switch_invoices(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->billing);
	rows = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	return rows->len;
}

static void switch_bill(Fixture *f, gint64 subscription, const gchar *action, GDateTime *at)
{
	g_autoptr(VentureEntity) request = g_object_new(VENTURE_TYPE_BILLING_REQUEST, "action", action,
		"subscription-id", subscription, "at", at, NULL);
	venture_entity_set_organization_id(request, f->billing); save(f, request);
}

/* Sets the subscription's clock columns to offsets from now, in hours, so a
 * test can date billing in the past: the settlement refuses a financial
 * event dated in the future, and an invoice number carries its day. */
static void switch_clock(Fixture *f, gint64 subscription, const gchar *column, gint hours)
{
	g_autoptr(GDateTime) now = venture_time_now(), then = g_date_time_add_hours(now, hours);
	g_autofree gchar *text = venture_time_to_string(then);
	g_autofree gchar *sql = g_strdup_printf("UPDATE customer_subscriptions SET %s='%s' WHERE id=%" G_GINT64_FORMAT,
		column, text, subscription);
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_execute(f->db, sql, NULL, &error)); g_assert_no_error(error);
}

/* The issued total of the business's latest invoice, as the staff view reads it. */
static gchar *switch_last_total(Fixture *f, const gchar *admin_token)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/lightsite/billing/%" G_GINT64_FORMAT, f->business);
	gchar *own = f->token;
	g_autoptr(JsonNode) view = NULL;
	f->token = (gchar *)admin_token;
	view = call(f, path, NULL, 200);
	f->token = own;
	return g_strdup(json_object_get_string_member(json_object_get_object_member(json_node_get_object(view), "last_invoice"), "total"));
}

/* Before go-live the next bill is the first one, issued at publication: the
 * switch is pending until then and the first invoice is at the new price. */
static void test_switch_unpublished(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *enroll = switch_body(f->business, f->price, NULL);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
	gint64 subscription = json_object_get_int_member(json_node_get_object(enrolled), "subscription_id");
	gint64 annual = switch_price(f, f->billing, "year", "240 USD", TRUE);
	g_autofree gchar *admin = switch_use_token(f, f->owner, VENTURE_USER_ROLE_EDITOR);
	g_autofree gchar *preview_body = switch_body(f->business, annual, NULL);
	g_autofree gchar *confirm = switch_body(f->business, annual, "true");
	g_autofree gchar *declined = switch_body(f->business, annual, "false");
	g_autofree gchar *total = NULL;
	g_autoptr(JsonNode) preview = NULL, done = NULL, again = NULL, refused = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	JsonObject *answer, *current, *chosen, *next;
	gint64 pending = -1, on = 0;
	(void)unused;
	preview = call(f, "/api/v1/lightsite/billing/switch-preview", preview_body, 200);
	answer = json_node_get_object(preview);
	current = json_object_get_object_member(answer, "current");
	chosen = json_object_get_object_member(answer, "chosen");
	next = json_object_get_object_member(answer, "next_amount");
	g_assert_cmpint(json_object_get_int_member(answer, "organization_id"), ==, f->business);
	g_assert_cmpint(json_object_get_int_member(current, "plan_price_id"), ==, f->price);
	g_assert_cmpstr(json_object_get_string_member(current, "amount"), ==, "10.00");
	g_assert_cmpstr(json_object_get_string_member(current, "interval"), ==, "month");
	g_assert_cmpint(json_object_get_int_member(chosen, "plan_price_id"), ==, annual);
	g_assert_cmpstr(json_object_get_string_member(chosen, "amount"), ==, "240.00");
	g_assert_cmpstr(json_object_get_string_member(chosen, "currency"), ==, "USD");
	g_assert_cmpstr(json_object_get_string_member(chosen, "interval"), ==, "year");
	g_assert_cmpstr(json_object_get_string_member(answer, "effective"), ==, "next_bill");
	g_assert_cmpstr(json_object_get_string_member(answer, "state"), ==, "preview");
	/* Unpublished: the next bill is the first one, at go-live, on no date yet. */
	g_assert_true(json_object_get_null_member(answer, "next_bill_at"));
	g_assert_true(json_object_get_boolean_member(answer, "starts_at_go_live"));
	g_assert_cmpstr(json_object_get_string_member(next, "amount"), ==, "240.00");
	g_assert_false(json_object_get_boolean_member(answer, "charged_today"));
	g_assert_false(json_object_get_boolean_member(answer, "prorated"));
	g_assert_nonnull(strstr(json_object_get_string_member(answer, "summary"), "Nothing is charged today"));
	/* A preview writes nothing. */
	sub = switch_subscription(f, subscription);
	g_object_get(sub, "pending-plan-price-id", &pending, "plan-price-id", &on, NULL);
	g_assert_cmpint(pending, ==, 0); g_assert_cmpint(on, ==, f->price);
	g_assert_cmpuint(switch_invoices(f), ==, 0);
	/* Confirmation must be explicit. */
	refused = call(f, "/api/v1/lightsite/billing/switch", preview_body, 422); g_clear_pointer(&refused, json_node_unref);
	refused = call(f, "/api/v1/lightsite/billing/switch", declined, 422); g_clear_pointer(&refused, json_node_unref);
	done = call(f, "/api/v1/lightsite/billing/switch", confirm, 201);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(done), "state"), ==, "scheduled");
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(json_node_get_object(done), "chosen"), "plan_price_id"), ==, annual);
	g_clear_object(&sub); sub = switch_subscription(f, subscription);
	g_object_get(sub, "pending-plan-price-id", &pending, "plan-price-id", &on, NULL);
	g_assert_cmpint(pending, ==, annual); g_assert_cmpint(on, ==, f->price);
	g_assert_cmpuint(switch_invoices(f), ==, 0);
	/* The same confirmation again is the same answer and writes nothing. */
	again = call(f, "/api/v1/lightsite/billing/switch", confirm, 200);
	g_assert_true(json_node_equal(done, again));
	/* Go-live issues the first bill, at the chosen price. */
	g_date_time_unref(now); now = venture_time_now();
	switch_bill(f, subscription, "activate", now);
	g_clear_object(&sub); sub = switch_subscription(f, subscription);
	g_object_get(sub, "pending-plan-price-id", &pending, "plan-price-id", &on, NULL);
	g_assert_cmpint(pending, ==, 0); g_assert_cmpint(on, ==, annual);
	g_assert_cmpuint(switch_invoices(f), ==, 1);
	total = switch_last_total(f, admin);
	g_assert_cmpstr(total, ==, "240.00");
	g_test_message("Unpublished switch: preview wrote nothing, confirm scheduled it, first bill at the new price");
}

/* After go-live the switch waits for the renewal, whole: no charge today, no
 * credit for the unused month and no part-period line. */
static void test_switch_published(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *enroll = switch_body(f->business, f->price, NULL);
	g_autoptr(JsonNode) enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
	gint64 subscription = json_object_get_int_member(json_node_get_object(enrolled), "subscription_id");
	gint64 growth = switch_price(f, f->billing, "month", "20 USD", TRUE);
	g_autoptr(GDateTime) now = venture_time_now(), end = NULL;
	g_autofree gchar *admin = NULL, *end_text = NULL, *total = NULL;
	g_autofree gchar *confirm = switch_body(f->business, growth, "true");
	g_autoptr(JsonNode) preview = NULL, done = NULL, back = NULL;
	g_autoptr(VentureEntity) sub = NULL;
	gint64 pending = -1, on = 0;
	(void)unused;
	/* Went live 35 days ago, on another day than the renewal below; the
	 * period is then held open until tomorrow so the switch falls inside it. */
	switch_clock(f, subscription, "last_event_at", -40 * 24);
	switch_clock(f, subscription, "current_period_start", -40 * 24);
	{
		g_autoptr(GDateTime) live = g_date_time_add_days(now, -35);
		switch_bill(f, subscription, "activate", live);
	}
	g_assert_cmpuint(switch_invoices(f), ==, 1);
	switch_clock(f, subscription, "current_period_end", 24);
	sub = switch_subscription(f, subscription);
	g_object_get(sub, "current-period-end", &end, NULL);
	end_text = venture_time_to_string(end);
	admin = switch_use_token(f, f->owner, VENTURE_USER_ROLE_EDITOR);
	{
		g_autofree gchar *body = switch_body(f->business, growth, NULL);
		preview = call(f, "/api/v1/lightsite/billing/switch-preview", body, 200);
	}
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(preview), "next_bill_at"), ==, end_text);
	g_assert_false(json_object_get_boolean_member(json_node_get_object(preview), "starts_at_go_live"));
	done = call(f, "/api/v1/lightsite/billing/switch", confirm, 201);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(done), "next_bill_at"), ==, end_text);
	/* Nothing today: no invoice, no credit note, the plan still the old one. */
	g_assert_cmpuint(switch_invoices(f), ==, 1);
	g_clear_object(&sub); sub = switch_subscription(f, subscription);
	g_object_get(sub, "pending-plan-price-id", &pending, "plan-price-id", &on, NULL);
	g_assert_cmpint(pending, ==, growth); g_assert_cmpint(on, ==, f->price);
	/* Changing their mind back to today's price is a switch like any other. */
	{
		g_autofree gchar *body = switch_body(f->business, f->price, "true");
		back = call(f, "/api/v1/lightsite/billing/switch", body, 201);
		g_clear_pointer(&back, json_node_unref);
		g_clear_object(&sub); sub = switch_subscription(f, subscription);
		g_object_get(sub, "pending-plan-price-id", &pending, NULL);
		g_assert_cmpint(pending, ==, f->price);
		back = call(f, "/api/v1/lightsite/billing/switch", confirm, 201);
	}
	/* The next bill, at the period end, is the whole new price. The end is
	 * brought back to an hour ago so the renewal is not dated tomorrow. */
	switch_clock(f, subscription, "last_event_at", -2);
	switch_clock(f, subscription, "current_period_end", -1);
	{
		g_autoptr(GDateTime) renewal = venture_time_now();
		switch_bill(f, subscription, "renew", renewal);
	}
	g_clear_object(&sub); sub = switch_subscription(f, subscription);
	g_object_get(sub, "pending-plan-price-id", &pending, "plan-price-id", &on, NULL);
	g_assert_cmpint(pending, ==, 0); g_assert_cmpint(on, ==, growth);
	g_assert_cmpuint(switch_invoices(f), ==, 2);
	total = switch_last_total(f, admin);
	g_assert_cmpstr(total, ==, "20.00");
	g_test_message("Published switch: next bill on the period end at the whole new price, nothing today");
}

/* Only this business's owner, or the trusted service, may switch it, and only
 * to a price the billing organization offers in the same currency. */
static void test_switch_refusals(Fixture *f, gconstpointer unused)
{
	gint64 annual = switch_price(f, f->billing, "year", "240 USD", TRUE);
	gint64 retired = switch_price(f, f->billing, "month", "15 USD", FALSE);
	gint64 euro = switch_price(f, f->billing, "month", "9 EUR", TRUE);
	gint64 foreign = switch_price(f, f->business, "month", "5 USD", TRUE);
	g_autofree gchar *admin = NULL, *viewer = NULL;
	g_autofree gchar *body = switch_body(f->business, annual, NULL);
	g_autofree gchar *enroll = switch_body(f->business, f->price, NULL);
	g_autoptr(JsonNode) refused = NULL, other = NULL, ok = NULL, enrolled = NULL;
	gint64 stranger = 0;
	guint i;
	gint64 offered[4];
	(void)unused;
	offered[0] = retired; offered[1] = euro; offered[2] = foreign; offered[3] = 987654;
	/* Another business and its owner. */
	other = call(f, "/api/v1/lightsite/signups", "{\"idempotency_key\":\"switch-stranger\",\"email\":\"stranger@example.test\",\"issuer\":\"https://id.example.test\",\"subject\":\"stranger\",\"business_name\":\"Other business\",\"first_name\":\"Sam\",\"last_name\":\"Other\"}", 201);
	stranger = json_object_get_int_member(json_node_get_object(other), "user_id");
	admin = switch_use_token(f, f->owner, VENTURE_USER_ROLE_EDITOR);
	/* Nothing to switch before enrollment. */
	refused = call(f, "/api/v1/lightsite/billing/switch-preview", body, 422); g_clear_pointer(&refused, json_node_unref);
	{
		gchar *own = f->token;
		f->token = admin;
		enrolled = call(f, "/api/v1/lightsite/billing/enroll", enroll, 201);
		/* The trusted service may preview for the owner it has checked. */
		ok = call(f, "/api/v1/lightsite/billing/switch-preview", body, 200);
		f->token = own;
	}
	g_clear_pointer(&ok, json_node_unref);
	ok = call(f, "/api/v1/lightsite/billing/switch-preview", body, 200);
	/* Not offered, retired, another currency, another organization's. */
	for (i = 0; i < G_N_ELEMENTS(offered); i++) {
		g_autofree gchar *wrong = switch_body(f->business, offered[i], NULL);
		g_autofree gchar *wrong_confirm = switch_body(f->business, offered[i], "true");
		refused = call(f, "/api/v1/lightsite/billing/switch-preview", wrong, 422); g_clear_pointer(&refused, json_node_unref);
		refused = call(f, "/api/v1/lightsite/billing/switch", wrong_confirm, 422); g_clear_pointer(&refused, json_node_unref);
	}
	/* Malformed requests. */
	refused = call(f, "/api/v1/lightsite/billing/switch-preview", "{\"organization_id\":1}", 422); g_clear_pointer(&refused, json_node_unref);
	refused = call(f, "/api/v1/lightsite/billing/switch-preview", "{\"organization_id\":\"1\",\"plan_price_id\":2}", 422); g_clear_pointer(&refused, json_node_unref);
	{
		g_autofree gchar *extra = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"plan_price_id\":%" G_GINT64_FORMAT ",\"accepted\":true,\"note\":1}", f->business, annual);
		refused = call(f, "/api/v1/lightsite/billing/switch", extra, 422); g_clear_pointer(&refused, json_node_unref);
	}
	/* The owner's read-only token, and another business's owner, are refused. */
	g_free(switch_use_token(f, f->owner, VENTURE_USER_ROLE_VIEWER));
	refused = call(f, "/api/v1/lightsite/billing/switch-preview", body, 403); g_clear_pointer(&refused, json_node_unref);
	viewer = switch_use_token(f, stranger, VENTURE_USER_ROLE_EDITOR);
	refused = call(f, "/api/v1/lightsite/billing/switch-preview", body, 403); g_clear_pointer(&refused, json_node_unref);
	{
		g_autofree gchar *confirm = switch_body(f->business, annual, "true");
		refused = call(f, "/api/v1/lightsite/billing/switch", confirm, 403); g_clear_pointer(&refused, json_node_unref);
	}
	{
		g_autoptr(VentureEntity) sub = switch_subscription(f, json_object_get_int_member(json_node_get_object(enrolled), "subscription_id"));
		gint64 pending = -1;
		g_object_get(sub, "pending-plan-price-id", &pending, NULL);
		g_assert_cmpint(pending, ==, 0);
	}
	/* A cancelled enrollment has no plan to switch. */
	g_free(f->token); f->token = g_strdup(admin);
	{
		g_autofree gchar *cancel = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT "}", f->business);
		g_autoptr(JsonNode) cancelled = call(f, "/api/v1/lightsite/billing/cancel", cancel, 200);
	}
	refused = call(f, "/api/v1/lightsite/billing/switch-preview", body, 409);
	g_test_message("Switch refusals: before enrollment, unoffered prices, malformed bodies, viewer and stranger tokens, cancelled");
}
