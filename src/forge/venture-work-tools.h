/*
 * venture-work-tools.h - File tools an agent cannot escape from
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ai-glib ships read, write, edit, glob, grep, ls and bash as built-ins, and
 * they are not a sandbox. Its path resolver joins a relative path to the
 * working directory and returns an absolute one unchanged, with no `..`
 * normalisation; its bash tool sets a subprocess working directory, which a
 * shell is free to leave. An agent given those tools inside this process
 * could read the database, the configuration and /proc/self/environ -- where
 * the session secret and every provider key live.
 *
 * So the in-process runner gets these instead: the same five useful
 * operations, refusing anything that resolves outside one directory, and no
 * shell at all. It can edit a tree. It cannot run that tree's tests, which
 * is the honest cost of running in the server's own process -- the CLI
 * runner is the one that can do both.
 *
 * The confinement rule is one function, venture_work_tools_resolve(), and
 * every tool goes through it. Its correctness is the whole of this file's
 * security value, so it is public and directly tested.
 */

#ifndef VENTURE_WORK_TOOLS_H
#define VENTURE_WORK_TOOLS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <ai-glib.h>

G_BEGIN_DECLS

/**
 * venture_work_tools_resolve:
 * @root: the directory the agent is confined to
 * @path: the path the agent asked for, absolute or relative
 * @must_exist: whether @path itself has to exist already
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves @path against @root and refuses anything that escapes it.
 *
 * Both sides are canonicalised before they are compared, so a symlink
 * pointing out of the tree is caught even when it was created after the run
 * started -- resolution happens at every use, not once at setup.
 *
 * The comparison is against `root + G_DIR_SEPARATOR`, not against `root`
 * alone. A plain prefix test passes `/work/run-1-evil` for a root of
 * `/work/run-1`, which is a sibling directory an agent could create and then
 * read anything from.
 *
 * When @must_exist is %FALSE the parent is canonicalised instead, because a
 * file being written for the first time has no path to resolve yet. The
 * parent still has to be inside @root.
 *
 * Returns: (transfer full) (nullable): the absolute path, or %NULL if it
 *   escapes @root or cannot be resolved
 */
gchar *
venture_work_tools_resolve(
	const gchar	 *root,
	const gchar	 *path,
	gboolean	  must_exist,
	GError		**error
);

/**
 * venture_work_tools_register:
 * @executor: an executor built with ai_tool_executor_new_empty()
 * @root: the directory every tool is confined to
 *
 * Registers read, write, edit, glob and grep, all confined to @root.
 *
 * @executor must be an empty one. Registering these on an executor that
 * already carries ai-glib's built-ins would leave the unconfined versions
 * reachable alongside them -- and `bash` reachable at all, which is the one
 * thing this file exists to prevent.
 */
void
venture_work_tools_register(
	AiToolExecutor	*executor,
	const gchar	*root
);

G_END_DECLS

#endif /* VENTURE_WORK_TOOLS_H */
