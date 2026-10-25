/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_TELEPHONY_VOICE_H
#define VENTURE_TELEPHONY_VOICE_H
#include <venture.h>
G_BEGIN_DECLS

#define VENTURE_TYPE_VOICE_DESK (venture_voice_desk_get_type())
VENTURE_DECLARE_ENTITY(VentureVoiceDesk, venture_voice_desk, VOICE_DESK)

gboolean venture_telephony_service_voice_turn(VentureDatabase *database, gint64 organization_id,
	const gchar *call_id, const gchar *caller_number, const gchar *caller_said, const gchar *model_text,
	GDateTime *now, gchar **spoken, GError **error);
gboolean venture_telephony_service_voice_finish(VentureDatabase *database, gint64 organization_id,
	const gchar *call_id, const gchar *book_title, GDateTime *window_start, GDateTime *window_end,
	gboolean urgent, GDateTime *now, GError **error);

G_END_DECLS
#endif
