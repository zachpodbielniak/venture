/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Shared between the inbox service and its web chat channel only. */
#ifndef VENTURE_INBOX_PRIVATE_H
#define VENTURE_INBOX_PRIVATE_H
#include <venture.h>
G_BEGIN_DECLS

struct _VentureInboxService {
	GObject parent_instance;
	VentureDatabase *database;	/* weak */
	GWeakRef context;
	gint writing;
	gboolean installed;
	GHashTable *windows;	/* limit key -> InboxWindow */
};

/* Saves an inbox record past the write guard. */
gboolean venture_inbox_service_write(VentureInboxService *self, VentureEntity *row, const VentureActor *actor, GError **error);
/* Counts one more in a window; FALSE when the limit is already reached. */
gboolean venture_inbox_service_allow(VentureInboxService *self, const gchar *key, guint limit, GDateTime *now);
/* The system actor for derived writes. */
const VentureActor *venture_inbox_service_actor(const gchar *name);
/* A lower-case hex random token of @bytes bytes. */
gchar *venture_inbox_random_token(gsize bytes, GError **error);

G_END_DECLS
#endif
