/*
 * venture-org-html.h - A small org-mode to HTML renderer
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of VENTURE.
 *
 * The builtin renderer behind the documentation site, used when Emacs is
 * not on the machine building it. It handles what the documents under docs/ actually
 * use -- headings, paragraphs, plain and ordered lists with nesting,
 * tables with a header rule, src, example and quote blocks, file and web
 * links, and the four inline markups -- and deliberately nothing more.
 * Its output mirrors the shape of Org's own HTML exporter so a page looks
 * the same whichever renderer built it.
 */

#ifndef VENTURE_ORG_HTML_H
#define VENTURE_ORG_HTML_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

gchar *
venture_org_to_html(const gchar *org);

gchar *
venture_org_get_keyword(
	const gchar	*org,
	const gchar	*keyword
);

gchar *
venture_org_heading_slug(const gchar *heading);

gchar *
venture_org_html_to_text(const gchar *html);

void
venture_org_html_escape_append(
	GString		*out,
	const gchar	*text
);

G_END_DECLS

#endif /* VENTURE_ORG_HTML_H */
