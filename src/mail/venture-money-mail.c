/* Financial confirmations commit with the action they describe.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
gboolean
venture_mail_confirm_money(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) company = NULL, contact = NULL;
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	g_autofree gchar *email = NULL, *number = NULL;
	g_autofree gchar *body = NULL, *escaped = NULL, *html = NULL, *key = NULL;
	const gchar *subject = "Your quote acceptance is confirmed";
	gint64 company_id = 0, contact_id = 0, org = venture_entity_get_organization_id(record);
	if (!VENTURE_IS_QUOTE(record)) {
		venture_set_error_validation(error, "mail", "An accepted quote is required"); return FALSE;
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") == G_TYPE_INVALID) return TRUE;
	g_object_get(record, "company-id", &company_id, NULL);
	company = venture_database_get(database, VENTURE_TYPE_COMPANY, company_id, error);
	if (!company) return FALSE;
	if (venture_entity_get_organization_id(company) != org) {
		venture_set_error_validation(error, "mail", "Customer belongs to another organization"); return FALSE;
	}
	g_object_get(company, "email", &email, NULL);
	{
		g_object_get(record, "contact-id", &contact_id, NULL);
		if (contact_id > 0) {
			g_autofree gchar *address = NULL;
			contact = venture_database_get(database, VENTURE_TYPE_CONTACT, contact_id, error);
			if (!contact) return FALSE;
			if (venture_entity_get_organization_id(contact) != org) {
				venture_set_error_validation(error, "mail", "Contact belongs to another organization"); return FALSE;
			}
			g_object_get(contact, "email", &address, NULL);
			if (!venture_string_is_empty(address)) { g_free(email); email = g_steal_pointer(&address); }
		}
	}
	/* A missing address must not reject acceptance. Never guess a recipient. */
	if (venture_string_is_empty(email) || strpbrk(email, "\r\n") || !strchr(email, '@')) return TRUE;
	g_object_get(record, "number", &number, NULL);
	body = g_strdup_printf("Your acceptance of quote %s has been recorded. Thank you. This confirmation is not a payment receipt.", number ? number : "");
	escaped = g_markup_escape_text(body, -1);
	html = g_strdup_printf("<!doctype html><html><body><h1>%s</h1><p>%s</p></body></html>", subject, escaped);
	key = g_strdup_printf("quote-accepted:%s", venture_entity_get_uuid(record));
	message = venture_mail_message_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(message), org);
	g_object_set(message, "to", email, "subject", subject, "text-body", body, "html-body", html,
		"idempotency-key", key, "related-type", "quote", "related-id", venture_entity_get_id(record), NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(database), message, actor, error);
	return queued != NULL;
}
