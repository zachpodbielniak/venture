/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-quote-mail.h"
#include <string.h>

static VentureEntity *
customer_record(VentureDatabase *database, GType type, gint64 id, gint64 org, GError **error)
{
	VentureEntity *record = venture_database_get(database, type, id, error);
	if (!record) return NULL;
	if (venture_entity_is_deleted(record) || venture_entity_get_organization_id(record) != org) {
		g_object_unref(record);
		venture_set_error_validation(error, "customer", "Quote customer must belong to this business");
		return NULL;
	}
	return record;
}

gchar *
venture_quote_mail_recipient(VentureDatabase *database, VentureEntity *quote, GError **error)
{
	g_autoptr(VentureEntity) company = NULL, contact = NULL;
	g_autofree gchar *email = NULL;
	gint64 company_id = 0, contact_id = 0, contact_company = 0;
	gint64 org = venture_entity_get_organization_id(quote);
	g_object_get(quote, "company-id", &company_id, "contact-id", &contact_id, NULL);
	company = customer_record(database, VENTURE_TYPE_COMPANY, company_id, org, error);
	if (!company) return NULL;
	if (contact_id > 0) {
		contact = customer_record(database, VENTURE_TYPE_CONTACT, contact_id, org, error);
		if (!contact) return NULL;
		g_object_get(contact, "company-id", &contact_company, "email", &email, NULL);
		if (contact_company != company_id) {
			venture_set_error_validation(error, "contact", "Quote contact must belong to its customer");
			return NULL;
		}
	}
	if (venture_string_is_empty(email)) {
		g_clear_pointer(&email, g_free);
		g_object_get(company, "email", &email, NULL);
	}
	if (venture_string_is_empty(email)) return NULL;
	return g_steal_pointer(&email);
}

/* The quote's mail, writing nothing; a reminder (day set) carries the same
 * private link, says so in its subject and is keyed by the day it was asked
 * for. The send, the reminder and the preview all build it here. */
static VentureMailMessage *
build_message(VentureDatabase *database, VentureEntity *quote, const gchar *base_url,
	const gchar *acceptance_url, const gchar *day, GError **error)
{
	g_autoptr(GUri) origin = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autofree gchar *email = NULL, *number = NULL, *url = NULL;
	g_autofree gchar *key = NULL, *subject = NULL, *body = NULL, *formatted = NULL;
	gint64 org = venture_entity_get_organization_id(quote);
	origin = !venture_string_is_empty(base_url) ? g_uri_parse(base_url, G_URI_FLAGS_NONE, NULL) : NULL;
	if (!origin || g_strcmp0(g_uri_get_scheme(origin), "https") || !g_uri_get_host(origin) ||
		g_uri_get_userinfo(origin) || g_uri_get_query(origin) || g_uri_get_fragment(origin)) {
		venture_set_error_validation(error, "base-url", "Quote delivery needs a configured public HTTPS address");
		return NULL;
	}
	g_object_get(quote, "number", &number, "total", &total, NULL);
	email = venture_quote_mail_recipient(database, quote, error);
	if (error && *error) return NULL;
	if (venture_string_is_empty(email)) {
		venture_set_error_validation(error, "customer", "Add a customer email before sending this quote");
		return NULL;
	}
	url = g_uri_resolve_relative(base_url, acceptance_url, G_URI_FLAGS_NONE, error);
	if (!url) return NULL;
	formatted = total ? venture_money_to_display_string(total, TRUE) : g_strdup("");
	subject = day ? g_strdup_printf("Reminder: Quote %s", number) : g_strdup_printf("Quote %s", number);
	body = day ? g_strdup_printf("A reminder about your quote %s, which totals %s and is still waiting for your answer.\n\n"
			"Review the full quote and accept it here:\n%s\n", number, formatted, url)
		: g_strdup_printf("Your quote %s totals %s.\n\nReview the full quote and accept it here:\n%s\n", number, formatted, url);
	key = day ? g_strdup_printf("quote-reminder:%s:%s", venture_entity_get_uuid(quote), day)
		: g_strdup_printf("quote:%s", venture_entity_get_uuid(quote));
	message = venture_mail_message_new();
	g_object_set(message, "organization-id", org, "to", email, "subject", subject,
		"text-body", "A private quote acceptance link is included in this message.",
		"private-text-body", body, "idempotency-key", key,
		"related-type", "quote", "related-id", venture_entity_get_id(quote), NULL);
	return g_steal_pointer(&message);
}

VentureMailMessage *
venture_quote_mail_message(VentureDatabase *database, VentureEntity *quote, const gchar *base_url,
	const gchar *acceptance_url, GError **error)
{
	return build_message(database, quote, base_url, acceptance_url, NULL, error);
}

static gboolean
quote_mail(VentureDatabase *database, VentureEntity *quote, VentureEntity *delivery, const gchar *base_url,
	const gchar *day, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	g_autofree gchar *relative = NULL, *email = NULL;
	/* Offline quote ledgers may have no public address. Their retained
	 * delivery intent remains available to a separately configured sender. */
	if (!base_url || !*base_url) return TRUE;
	g_object_get(delivery, "acceptance-url", &relative, NULL);
	message = build_message(database, quote, base_url, relative, day, error);
	if (!message) return FALSE;
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(database), message, actor, error);
	if (!queued) return FALSE;
	g_object_get(message, "to", &email, NULL);
	g_object_set(delivery, "recipient", email, NULL);
	return TRUE;
}

gboolean
venture_quote_mail_enqueue(VentureDatabase *database, VentureEntity *quote,
	VentureEntity *delivery, const gchar *base_url, const VentureActor *actor, GError **error)
{
	return quote_mail(database, quote, delivery, base_url, NULL, actor, error);
}

gboolean
venture_quote_mail_remind(VentureDatabase *database, VentureEntity *quote, VentureEntity *delivery,
	const gchar *base_url, const gchar *day, const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(day != NULL, FALSE);
	if (!base_url || !*base_url) {
		venture_set_error_validation(error, "base-url", "A quote reminder needs a configured public HTTPS address");
		return FALSE;
	}
	return quote_mail(database, quote, delivery, base_url, day, actor, error);
}

/* A draft has no acceptance link yet: the token is minted by the send. The
 * preview withholds the link anyway, so any path stands in for it. */
static VentureMailMessage *
quote_preview(VentureContext *context, gint64 org, gint64 id, GDateTime *now, GPtrArray *notes, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureEntity) quote = venture_database_get(database, VENTURE_TYPE_QUOTE, id, NULL);
	g_autofree gchar *base = NULL;
	gint status = 0;
	(void)now;
	if (!quote || venture_entity_is_deleted(quote) || venture_entity_get_organization_id(quote) != org) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Quote not found");
		return NULL;
	}
	g_object_get(venture_database_get_quote_service(database), "base-url", &base, NULL);
	if (venture_string_is_empty(base)) {
		venture_set_error_validation(error, "base-url",
			"This install has no public HTTPS address (server.base_url), so quotes are recorded as sent but not emailed");
		return NULL;
	}
	g_object_get(quote, "status", &status, NULL);
	if (status != VENTURE_QUOTE_DRAFT && notes)
		g_ptr_array_add(notes, g_strdup("Only a draft quote is emailed; this one has already left draft, so this shows how it reads today."));
	return venture_quote_mail_message(database, quote, base, "/q/preview", error);
}
static const VentureMailPreviewKind quote_kind = {
	"quote", "Quote", "quotes", "quote", "quote", FALSE, quote_preview
};
void
venture_quote_mail_preview_register(void)
{
	venture_mail_preview_register(&quote_kind);
}
