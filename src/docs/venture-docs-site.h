/*
 * venture-docs-site.h - The documentation site, rendered from every .org under docs/
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of VENTURE.
 *
 * One object, one job: turn a source tree's every .org under docs/ and README.org into
 * a static site -- a page per document, a left nav generated from
 * docs/index.org, links rewritten to point within the site, a
 * client-side search index -- and refuse to do so when a link is broken
 * or a document is not reachable from the index. `make docs-site`,
 * `venturectl docs build` and the test suite all go through it.
 */

#ifndef VENTURE_DOCS_SITE_H
#define VENTURE_DOCS_SITE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VentureDocsRenderer:
 * @VENTURE_DOCS_RENDERER_AUTO: Emacs when it is on PATH, the builtin otherwise
 * @VENTURE_DOCS_RENDERER_EMACS: Org's own HTML exporter, run in batch
 * @VENTURE_DOCS_RENDERER_BUILTIN: the small renderer in venture-org-html.c
 *
 * Which program turns an org document into an HTML body. The rest of the
 * site -- nav, links, search, the checks -- is the same whichever is used.
 */
typedef enum
{
	VENTURE_DOCS_RENDERER_AUTO,
	VENTURE_DOCS_RENDERER_EMACS,
	VENTURE_DOCS_RENDERER_BUILTIN
} VentureDocsRenderer;

#define VENTURE_TYPE_DOCS_RENDERER (venture_docs_renderer_get_type())

GType
venture_docs_renderer_get_type(void) G_GNUC_CONST;

const gchar *
venture_docs_renderer_to_nick(VentureDocsRenderer renderer);

gboolean
venture_docs_renderer_from_nick(
	const gchar		*nick,
	VentureDocsRenderer	*renderer
);

#define VENTURE_TYPE_DOCS_SITE (venture_docs_site_get_type())

G_DECLARE_FINAL_TYPE(VentureDocsSite, venture_docs_site, VENTURE, DOCS_SITE, GObject)

VentureDocsSite *
venture_docs_site_new(
	const gchar	*source_dir,
	const gchar	*output_dir
);

const gchar *
venture_docs_site_get_source_dir(VentureDocsSite *self);

const gchar *
venture_docs_site_get_output_dir(VentureDocsSite *self);

VentureDocsRenderer
venture_docs_site_get_renderer(VentureDocsSite *self);

void
venture_docs_site_set_renderer(
	VentureDocsSite		*self,
	VentureDocsRenderer	 renderer
);

gboolean
venture_docs_site_build(
	VentureDocsSite	 *self,
	GCancellable	 *cancellable,
	GError		**error
);

VentureDocsRenderer
venture_docs_site_get_renderer_used(VentureDocsSite *self);

guint
venture_docs_site_get_page_count(VentureDocsSite *self);

G_END_DECLS

#endif /* VENTURE_DOCS_SITE_H */
