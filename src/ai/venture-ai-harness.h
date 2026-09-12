/*
 * venture-ai-harness.h - the assistant's harness, on the web
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A harness is the layer between a person typing and a model answering:
 * the slash commands that expand to a prompt, the completion that offers
 * them, and the references that pull something into the conversation.
 * ai-glib has one, built for terminals -- `ai-tui` wires an
 * #AiResourceRegistry to an #AiCommandSet to an #AiCompletionContext and
 * reads keystrokes. This is the same harness with a browser on the front
 * and this install's records behind it.
 *
 * Three things can be completed in the composer, and each answers a
 * different question a person has mid-sentence:
 *
 *   /command   what can I ask for?      commands, from here and from disk
 *   @record    which one do I mean?     a ticket, a release, an incident
 *   #base      what should it read?     a knowledge base
 *
 * The commands come from two places on purpose. VENTURE's own skills are
 * records an operator edits in the web UI; the rest are ordinary markdown
 * files in the directories every other agent tool already uses --
 * `~/.claude/commands`, `~/.config/ai-glib/commands`, `.agents/skills` and
 * their project-local equivalents. An operator who has written commands
 * for a terminal agent does not write them again here, and a repository
 * that ships a `/release` command has it in the assistant the moment the
 * working directory is set to that checkout.
 *
 * The @ references are what makes this a harness for *this* system rather
 * than a general one. In a terminal an @ names a file; here it names a
 * record -- `@ticket/12`, `@release/4`, `@incident/2` -- and the model is
 * shown that record's fields ahead of the question. A conversation about
 * the software factory can therefore name what it is about instead of
 * describing it.
 */

#ifndef VENTURE_AI_HARNESS_H
#define VENTURE_AI_HARNESS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

#include <ai-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_AI_HARNESS (venture_ai_harness_get_type())

G_DECLARE_FINAL_TYPE(VentureAiHarness, venture_ai_harness,
                     VENTURE, AI_HARNESS, GObject)

/**
 * VentureHarnessCompletion:
 * @VENTURE_HARNESS_COMPLETION_NONE: nothing at the cursor can be completed
 * @VENTURE_HARNESS_COMPLETION_COMMAND: a `/name`
 * @VENTURE_HARNESS_COMPLETION_RECORD: an `@type` or `@type/id`
 * @VENTURE_HARNESS_COMPLETION_BASE: a `#slug`
 *
 * What is being completed at the cursor.
 */
typedef enum
{
	VENTURE_HARNESS_COMPLETION_NONE = 0,
	VENTURE_HARNESS_COMPLETION_COMMAND,
	VENTURE_HARNESS_COMPLETION_RECORD,
	VENTURE_HARNESS_COMPLETION_BASE
} VentureHarnessCompletion;

/**
 * VentureHarnessItem:
 * @insert: what replaces the range being completed
 * @label: the token, shown first: `/triage`, `@ticket/12`, `#contracts`
 * @name: what it is called
 * @description: (nullable): one line about it
 * @origin: (nullable): where it came from -- "venture", "claude", a type
 * @kind: which sort of completion produced it
 *
 * One thing the composer can offer.
 *
 * @origin is kept apart from @description because a menu truncates, and
 * the piece that tells two same-named commands apart must not be the
 * first thing to fall off the end.
 */
typedef struct
{
	gchar				*insert;
	gchar				*label;
	gchar				*name;
	gchar				*description;
	gchar				*origin;
	VentureHarnessCompletion	 kind;
} VentureHarnessItem;

/**
 * VentureHarnessAllowFunc:
 * @entity_type: the record type being considered
 * @user_data: the data passed alongside
 *
 * Whether the person asking may see records of @entity_type.
 *
 * The harness does not decide this. Which role a type needs is the web
 * layer's policy -- users, tokens and other people's conversations are
 * owner-only there -- and a second copy of that rule in here is a second
 * place for it to go stale.
 *
 * Returns: %TRUE if records of @entity_type may be offered and read
 */
typedef gboolean (*VentureHarnessAllowFunc)(
	GType		 entity_type,
	gpointer	 user_data
);

/**
 * venture_harness_item_free:
 * @item: (transfer full) (nullable): an item
 *
 * Frees @item.
 */
void
venture_harness_item_free(VentureHarnessItem *item);

/**
 * venture_ai_harness_new:
 * @context: a #VentureContext
 *
 * Builds the harness and scans for resources once.
 *
 * Returns: (transfer full): a new #VentureAiHarness
 */
VentureAiHarness *
venture_ai_harness_new(VentureContext *context);

/**
 * venture_ai_harness_set_working_directory:
 * @self: a #VentureAiHarness
 * @path: (nullable): a directory, or %NULL for none
 *
 * Where project-scoped resources are looked for: a `.claude/commands` in
 * a checkout, say. Rescans.
 */
void
venture_ai_harness_set_working_directory(
	VentureAiHarness	*self,
	const gchar		*path
);

/**
 * venture_ai_harness_refresh:
 * @self: a #VentureAiHarness
 *
 * Reads the resource directories again.
 *
 * Cheap enough to call when a menu is opened, which is when it matters:
 * a command written a minute ago should be there without a restart.
 */
void
venture_ai_harness_refresh(VentureAiHarness *self);

/**
 * venture_ai_harness_complete:
 * @self: a #VentureAiHarness
 * @buffer: the whole composer contents
 * @cursor: the caret's byte offset into @buffer
 * @allow: (scope call) (nullable): which record types may be offered
 * @allow_data: data for @allow
 * @out_start: (out) (optional): byte offset the completion replaces from
 * @out_end: (out) (optional): byte offset it replaces to
 *
 * What the composer can offer at the cursor.
 *
 * Returns: (transfer full) (element-type VentureHarnessItem) (nullable):
 *   the candidates, or %NULL when nothing can be completed there
 */
GPtrArray *
venture_ai_harness_complete(
	VentureAiHarness		 *self,
	const gchar			 *buffer,
	guint				  cursor,
	VentureHarnessAllowFunc		  allow,
	gpointer			  allow_data,
	guint				 *out_start,
	guint				 *out_end
);

/**
 * venture_ai_harness_completion_kind:
 * @buffer: the whole composer contents
 * @cursor: the caret's byte offset into @buffer
 *
 * Which sort of completion the cursor sits in, without building any
 * candidates.
 *
 * Returns: the kind
 */
VentureHarnessCompletion
venture_ai_harness_completion_kind(
	const gchar	*buffer,
	guint		 cursor
);

/**
 * venture_ai_harness_expand:
 * @self: a #VentureAiHarness
 * @line: what the operator typed
 *
 * The prompt a slash command stands for, or %NULL when @line is not one.
 *
 * VENTURE's own skills answer first, so an operator's `ai_skill` record
 * still shadows everything; a command file on disk answers after that.
 * Anything the arguments do not fit into is appended as instructions, so
 * a command written without `$ARGUMENTS` still takes them.
 *
 * Returns: (transfer full) (nullable): the prompt, or %NULL
 */
gchar *
venture_ai_harness_expand(
	VentureAiHarness	*self,
	const gchar		*line
);

/**
 * venture_ai_harness_expand_mentions:
 * @self: a #VentureAiHarness
 * @text: what the operator typed
 * @allow: (scope call) (nullable): which record types may be read
 * @allow_data: data for @allow
 *
 * The records an `@type/id` names, written out for the model.
 *
 * The text itself is left alone: the operator wrote `@ticket/12` and the
 * transcript should say so a month later. What comes back is the block to
 * put ahead of the question, or %NULL when nothing was named.
 *
 * Returns: (transfer full) (nullable): the block, or %NULL
 */
gchar *
venture_ai_harness_expand_mentions(
	VentureAiHarness		*self,
	const gchar			*text,
	VentureHarnessAllowFunc		 allow,
	gpointer			 allow_data
);

/**
 * venture_ai_harness_describe_record:
 * @self: a #VentureAiHarness
 * @type_name: a registered record type
 * @id: its id
 *
 * One record, as the fields a person would read.
 *
 * This is the one place a record is written out for a model: the page
 * context, an `@` mention and anything added later all go through it, so
 * they cannot disagree about what a ticket looks like.
 *
 * Returns: (transfer full) (nullable): the description, or %NULL when
 *   there is no such record
 */
gchar *
venture_ai_harness_describe_record(
	VentureAiHarness	*self,
	const gchar		*type_name,
	gint64			 id
);

/**
 * venture_ai_harness_list:
 * @self: a #VentureAiHarness
 * @kind: which resources to list
 *
 * Everything the harness knows of one kind, for a page that shows it.
 *
 * Returns: (transfer full) (element-type VentureHarnessItem): the entries
 */
GPtrArray *
venture_ai_harness_list(
	VentureAiHarness	*self,
	AiResourceKind		 kind
);

/**
 * venture_ai_harness_search_paths:
 * @self: a #VentureAiHarness
 * @kind: which resources
 *
 * The directories scanned for @kind, in the order they are read.
 *
 * Returns: (transfer full) (array zero-terminated=1): the paths
 */
gchar **
venture_ai_harness_search_paths(
	VentureAiHarness	*self,
	AiResourceKind		 kind
);

G_END_DECLS

#endif /* VENTURE_AI_HARNESS_H */
