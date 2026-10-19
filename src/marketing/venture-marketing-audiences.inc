/* SPDX-License-Identifier: AGPL-3.0-or-later */
GPtrArray *venture_marketing_service_segment_members(VentureMarketingService *self,
	gint64 organization_id, gint64 list_id, GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) list = NULL;
	GPtrArray *rows;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), NULL);
	if (!allowed(self, organization_id, error)) return NULL;
	if (!now) { refuse(error, "A segment needs a clock"); return NULL; }
	list = get_row(self, VENTURE_TYPE_MARKETING_LIST, organization_id, list_id, error);
	if (!list) return NULL;
	if (choice(list, "mode") != 1) { refuse(error, "Only a saved-search segment has members by search"); return NULL; }
	rows = segment_members(self, list, now, error);
	if (rows && rows->len > 10000) { g_ptr_array_unref(rows); refuse(error, "The segment selects more than 10000 records; narrow it"); return NULL; }
	return rows;
}
gchar **venture_marketing_ab_assign(const gchar *send_uuid, const gchar *const *emails, guint percent)
{ (void)send_uuid; (void)emails; (void)percent; return NULL; }
GDateTime *venture_marketing_local_due(const gchar *zone, gint hour, const gchar *day,
	GDateTime *after, GError **error)
{ (void)zone; (void)hour; (void)day; (void)after; refuse(error, "Not implemented"); return NULL; }
VentureMarketingSend *venture_marketing_service_resend(VentureMarketingService *self, gint64 organization_id,
	gint64 send_id, gint64 days, const gchar *subject, const VentureActor *actor, GError **error)
{ (void)self; (void)organization_id; (void)send_id; (void)days; (void)subject; (void)actor; refuse(error, "Not implemented"); return NULL; }
