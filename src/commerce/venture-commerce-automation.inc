/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean venture_pod_module_handle_commerce(VenturePodModule *self, GVariant *params, GVariant **result)
{
	VentureCommerceService *service = venture_context_get_commerce_service(self->context);
	const gchar *organization = venture_pod_module_argument(params, 0);
	const gchar *name = venture_pod_module_argument(params, 1);
	g_autoptr(GError) error = NULL;
	gchar *end;
	gint64 org;
	gint imported;
	if (!service || !organization) return FALSE;
	org = g_ascii_strtoll(organization, &end, 10);
	if (*end || org <= 0) return FALSE;
	imported = venture_commerce_service_import(service, name, NULL, NULL, NULL, &error);
	if (imported < 0) {
		g_warning("venture->commerce_import: %s", error->message);
		return FALSE;
	}
	if (result) *result = g_variant_ref_sink(g_variant_new_parsed("{'imported': <%i>}", imported));
	return TRUE;
}
