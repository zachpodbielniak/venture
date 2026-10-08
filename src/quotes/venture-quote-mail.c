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

gboolean
venture_quote_mail_enqueue(VentureDatabase *database, VentureEntity *quote,
	VentureEntity *delivery, const gchar *base_url, const VentureActor *actor, GError **error)
{
	g_autoptr(GUri) origin = NULL;
	g_autoptr(VentureEntity) company = NULL, contact = NULL;
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autofree gchar *email = NULL, *number = NULL, *relative = NULL, *url = NULL;
	g_autofree gchar *key = NULL, *subject = NULL, *body = NULL, *formatted = NULL;
	gint64 company_id, contact_id, contact_company, org = venture_entity_get_organization_id(quote);
	/* Offline quote ledgers may have no public address. Their retained
	 * delivery intent remains available to a separately configured sender. */
	if (!base_url || !*base_url) return TRUE;
	origin = g_uri_parse(base_url, G_URI_FLAGS_NONE, NULL);
	if (!origin || g_strcmp0(g_uri_get_scheme(origin), "https") || !g_uri_get_host(origin) ||
		g_uri_get_userinfo(origin) || g_uri_get_query(origin) || g_uri_get_fragment(origin)) {
		venture_set_error_validation(error, "base-url", "Quote delivery needs a configured public HTTPS address");
		return FALSE;
	}
	g_object_get(quote, "company-id", &company_id, "contact-id", &contact_id,
		"number", &number, "total", &total, NULL);
	company = customer_record(database, VENTURE_TYPE_COMPANY, company_id, org, error);
	if (!company) return FALSE;
	if (contact_id > 0) {
		contact = customer_record(database, VENTURE_TYPE_CONTACT, contact_id, org, error);
		if (!contact) return FALSE;
		g_object_get(contact, "company-id", &contact_company, "email", &email, NULL);
		if (contact_company != company_id) {
			venture_set_error_validation(error, "contact", "Quote contact must belong to its customer");
			return FALSE;
		}
	}
	if (venture_string_is_empty(email)) {
		g_clear_pointer(&email, g_free);
		g_object_get(company, "email", &email, NULL);
	}
	if (venture_string_is_empty(email)) {
		venture_set_error_validation(error, "customer", "Add a customer email before sending this quote");
		return FALSE;
	}
	g_object_get(delivery, "acceptance-url", &relative, NULL);
	url = g_uri_resolve_relative(base_url, relative, G_URI_FLAGS_NONE, error);
	if (!url) return FALSE;
	formatted = total ? venture_money_to_display_string(total, TRUE) : g_strdup("");
	subject = g_strdup_printf("Quote %s", number);
	body = g_strdup_printf("Your quote %s totals %s.\n\nReview the full quote and accept it here:\n%s\n", number, formatted, url);
	key = g_strdup_printf("quote:%s", venture_entity_get_uuid(quote));
	message = venture_mail_message_new();
	g_object_set(message, "organization-id", org, "to", email, "subject", subject,
		"text-body", "A private quote acceptance link is included in this message.",
		"private-text-body", body, "idempotency-key", key,
		"related-type", "quote", "related-id", venture_entity_get_id(quote), NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(database), message, actor, error);
	if (!queued) return FALSE;
	g_object_set(delivery, "recipient", email, NULL);
	return TRUE;
}
