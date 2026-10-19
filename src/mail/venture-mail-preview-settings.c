/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-mail-preview-settings.c - The mail preview's setting
 * (docs/mail.org, docs/settings.org)
 */
#include "venture.h"

static const VentureSettingInfo preview_link_check = {
	"mail.preview-link-check", "Email", "mail",
	"Check links answer on every preview",
	"Each preview also asks every web link in the message whether it answers (one request each, at most ten, "
	"from this server). Off, previews check only that links are well formed, and the preview page offers the check.",
	VENTURE_SETTING_BOOLEAN, "false", 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	"organization: mail-preview-link-check", "mail-preview-link-check", NULL, NULL, NULL, NULL
};

void venture_mail_preview_settings_register(void)
{
	venture_settings_register(&preview_link_check);
}
