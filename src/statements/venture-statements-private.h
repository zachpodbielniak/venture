/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_STATEMENTS_PRIVATE_H
#define VENTURE_STATEMENTS_PRIVATE_H
gboolean venture_statements_owns_report(VentureContext *context, const gchar *name);
void venture_statements_append_index(VentureContext *context, GString *html);
void venture_statements_append_controls(VentureContext *context, VentureReport *report,
	JsonObject *options, GString *html);
void venture_statements_append_reference(GString *html, const gchar *key, const gchar *cell);
#endif
