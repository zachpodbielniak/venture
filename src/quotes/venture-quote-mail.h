/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_QUOTE_MAIL_H
#define VENTURE_QUOTE_MAIL_H
/* Called inside the quote action transaction; performs no network I/O. */
gboolean venture_quote_mail_enqueue(VentureDatabase *database, VentureEntity *quote,
	VentureEntity *delivery, const gchar *base_url, const VentureActor *actor, GError **error);
#endif
