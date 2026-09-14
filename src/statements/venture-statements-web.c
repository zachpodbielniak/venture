/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "statements/venture-statements-private.h"

gboolean
venture_statements_owns_report(VentureContext *context, const gchar *name)
{
	VentureModule *module = venture_module_registry_lookup(venture_context_get_modules(context), "statements");
	return module != NULL && g_strv_contains(venture_module_get_reports(module), name);
}

void
venture_statements_append_index(VentureContext *context, GString *html)
{
	g_autoptr(GPtrArray) reports = venture_report_registry_list(venture_context_get_report_registry(context));
	gboolean opened = FALSE;
	guint i;
	for (i = 0; i < reports->len; i++)
	{
		VentureReport *report = g_ptr_array_index(reports, i);
		const gchar *name = venture_report_get_name(report);
		if (!venture_statements_owns_report(context, name))
			continue;
		if (!opened)
		{
			g_string_append(html, "<h2>Statements</h2><div class=\"grid cols-2\">");
			opened = TRUE;
		}
		g_string_append(html, "<div class=\"card\"><div class=\"card-body\"><h3>");
		venture_html_escape_append(html, venture_report_get_title(report));
		g_string_append(html, "</h3><p class=\"muted\">");
		venture_html_escape_append(html, venture_report_get_description(report));
		g_string_append_printf(html, "</p><a class=\"btn btn-primary btn-sm\" href=\"/reports/%s\">Open</a></div></div>", name);
	}
	if (opened)
		g_string_append(html, "</div>");
}

void
venture_statements_append_controls(VentureContext *context, VentureReport *report,
	JsonObject *options, GString *html)
{
	if (!venture_statements_owns_report(context, venture_report_get_name(report)))
		return;
	g_string_append(html, "<label>Compare to<input name=\"compare_to\" placeholder=\"Prior period, e.g. 2026-07\" value=\"");
	venture_html_escape_append(html, venture_json_object_get_string(options, "compare_to", ""));
	g_string_append(html, "\"></label>");
	g_string_append(html, "<label>Basis<select name=\"basis\"><option value=\"accrual\"");
	if (g_strcmp0(venture_json_object_get_string(options, "basis", "accrual"), "cash") != 0)
		g_string_append(html, " selected");
	g_string_append(html, ">Accrual</option><option value=\"cash\"");
	if (g_strcmp0(venture_json_object_get_string(options, "basis", "accrual"), "cash") == 0)
		g_string_append(html, " selected");
	g_string_append(html, ">Cash</option></select></label>");
}

void
venture_statements_append_reference(GString *html, const gchar *key, const gchar *cell)
{
	g_autofree gchar *type = NULL;
	g_autofree gchar *encoded = NULL;
	gchar *end = NULL;
	gint64 id = g_ascii_strtoll(cell, &end, 10);
	gboolean linked = FALSE;
	/* IDs name registered entities, never caller-supplied link targets. */
	if (g_str_has_suffix(key, "_id"))
		type = g_strndup(key, strlen(key) - 3);
	if (id > 0 && end != cell && *end == '\0' && type != NULL &&
		venture_entity_registry_lookup(venture_entity_registry_get_default(), type) != 0)
	{
		encoded = g_uri_escape_string(type, NULL, FALSE);
		g_string_append_printf(html, "<a href=\"/e/%s/%" G_GINT64_FORMAT "\">", encoded, id);
		linked = TRUE;
	}
	venture_html_escape_append(html, cell);
	if (linked)
		g_string_append(html, "</a>");
}
