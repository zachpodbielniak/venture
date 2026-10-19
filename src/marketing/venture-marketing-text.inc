/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Text consent and text campaigns (venture-marketing-text.h). */
void venture_marketing_text_channel_register(const VentureMarketingTextChannel *channel, gpointer data)
{
	(void)channel; (void)data;
}
VentureEntity *venture_marketing_service_text_consent(VentureMarketingService *self, gint64 organization_id,
	VentureEntity *contact, const gchar *phone, gboolean granted, const gchar *source,
	const gchar *evidence, const gchar *evidence_key, GDateTime *at, const VentureActor *actor, GError **error)
{
	(void)self; (void)organization_id; (void)contact; (void)phone; (void)granted; (void)source;
	(void)evidence; (void)evidence_key; (void)at; (void)actor;
	refuse(error, "Text consent is not implemented yet");
	return NULL;
}
gboolean venture_marketing_service_text_permitted(VentureMarketingService *self, gint64 organization_id,
	const gchar *phone, gint64 *consent_id, GError **error)
{
	(void)self; (void)organization_id; (void)phone; (void)error;
	if (consent_id) *consent_id = 0;
	return FALSE;
}
