/*
 * venture-ai-skills.h - saved ways of asking the assistant
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef VENTURE_AI_SKILLS_H
#define VENTURE_AI_SKILLS_H

#include <glib-object.h>

#include "venture-types.h"

G_BEGIN_DECLS

/**
 * VentureAiSkillInfo:
 * @trigger: what is typed after the slash
 * @name: the skill's name
 * @description: one line on what it does
 * @builtin: %TRUE for a skill compiled in rather than stored
 *
 * One entry in the / menu.
 */
typedef struct
{
	gchar		*trigger;
	gchar		*name;
	gchar		*description;
	gboolean	 builtin;
} VentureAiSkillInfo;

/**
 * venture_ai_skill_info_free:
 * @info: (transfer full): an info
 *
 * Frees @info.
 */
void
venture_ai_skill_info_free(VentureAiSkillInfo *info);

/**
 * venture_ai_skills_list:
 * @context: a #VentureContext
 * @error: (out) (optional): return location for a #GError
 *
 * Every skill the assistant will expand: the built-in ones, then the
 * enabled ai_skill records, with a record replacing a built-in that shares
 * its trigger. Sorted by trigger.
 *
 * Returns: (transfer container) (element-type VentureAiSkillInfo): the
 *   skills, or %NULL on error
 */
GPtrArray *
venture_ai_skills_list(
	VentureContext	 *context,
	GError		**error
);

/**
 * venture_ai_skills_expand:
 * @context: a #VentureContext
 * @message: what the operator typed
 *
 * If @message begins with a slash and a known trigger, the prompt it
 * stands for, with whatever followed the trigger filled in for {input} --
 * or appended as instructions when the prompt has no placeholder. The
 * trigger is matched case-insensitively. Anything else, including an
 * unknown trigger, is not a skill.
 *
 * Returns: (transfer full) (nullable): the expanded prompt, or %NULL when
 *   @message is not a skill invocation
 */
gchar *
venture_ai_skills_expand(
	VentureContext	*context,
	const gchar	*message
);

G_END_DECLS

#endif /* VENTURE_AI_SKILLS_H */
