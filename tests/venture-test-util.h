/*
 * venture-test-util.h - Shared helpers for the test suite
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of VENTURE.
 *
 * Header-only on purpose: rules.mk compiles one tests/test-*.c into one
 * binary, so a shared .o would mean teaching it about a second one.
 */

#pragma once

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <stdlib.h>

/*
 * Whether @path is @root, or something under it.
 *
 * The trailing separator is what makes this a containment test rather
 * than a string prefix test: without it a root of
 * "/tmp/venture-routes-ab12cd" accepts "/tmp/venture-routes-ab12cd-evil",
 * which is the classic version of this bug. The same rule, for the same
 * reason, as venture_work_tools_resolve().
 */
static inline gboolean
venture_test_path_is_within(
	const gchar	*path,
	const gchar	*root
){
	g_autofree gchar *bounded = NULL;

	if (NULL == path || NULL == root)
		return FALSE;

	if (0 == g_strcmp0(path, root))
		return TRUE;

	bounded = g_strconcat(root, G_DIR_SEPARATOR_S, NULL);

	return g_str_has_prefix(path, bounded);
}

/*
 * One entry, checked against the root it must stay inside.
 *
 * @real_root is already canonical, resolved once by the caller: doing it
 * per entry would be the same answer for every one of them, and a root
 * reached through a symlink -- /tmp is one on some installs -- would then
 * refuse everything and remove nothing.
 */
static inline void
venture_test_remove_within(
	const gchar	*path,
	const gchar	*real_root
){
	g_autofree gchar *real_path = NULL;

	if (NULL == path)
		return;

	/*
	 * A symlink is unlinked, never followed -- and it is the link's own
	 * *location* that is checked, not where it points.
	 *
	 * realpath() on a link answers with its target, so a link pointing
	 * outside the fixture's directory would be refused here: neither
	 * removed nor followed, leaving the directory non-empty and the
	 * litter this exists to clear.
	 */
	if (g_file_test(path, G_FILE_TEST_IS_SYMLINK))
	{
		g_autofree gchar *parent = g_path_get_dirname(path);
		g_autofree gchar *base = g_path_get_basename(path);
		g_autofree gchar *real_parent = realpath(parent, NULL);
		g_autofree gchar *link = NULL;

		if (NULL == real_parent ||
		    !venture_test_path_is_within(real_parent, real_root))
			return;

		link = g_build_filename(real_parent, base, NULL);
		g_unlink(link);
		return;
	}

	real_path = realpath(path, NULL);

	/*
	 * Refused rather than removed, on the canonical path. A ".." in a
	 * name, or a directory reached through a link, would otherwise carry
	 * this outside the one the fixture made -- and a teardown that
	 * deletes somebody's files is a far worse outcome than a directory
	 * left behind in /tmp.
	 */
	if (NULL == real_path ||
	    !venture_test_path_is_within(real_path, real_root))
		return;

	if (!g_file_test(real_path, G_FILE_TEST_IS_DIR))
	{
		g_unlink(real_path);
		return;
	}

	{
		g_autoptr(GDir) directory = NULL;
		const gchar *entry;

		directory = g_dir_open(real_path, 0, NULL);

		while (NULL != directory &&
		       NULL != (entry = g_dir_read_name(directory)))
		{
			g_autofree gchar *child = NULL;

			child = g_build_filename(real_path, entry, NULL);
			venture_test_remove_within(child, real_root);
		}
	}

	g_rmdir(real_path);
}

/*
 * Removes a temporary directory and everything in it.
 *
 * g_rmdir() does nothing at all to a directory that is not empty, and
 * g_file_delete() on one fails the same way with an error nobody read --
 * so a fixture that wrote a file inside the directory it made leaves that
 * directory behind, silently, once per test per run, for ever. Five
 * fixtures here did; a full green suite left seventeen, and 44 had piled
 * up in /tmp over a few hours of ordinary runs before anybody counted.
 *
 * Deleting the tree is what every one of those fixtures always meant.
 *
 * Note what does *not* call this: a failing test. A g_assert failure
 * aborts the process, so no teardown runs on that path -- which is
 * correct and should stay that way, because a failing run's directory is
 * the evidence somebody needs to read.
 */
static inline void
venture_test_remove_tree(
	const gchar	*root
){
	g_autofree gchar *real_root = NULL;

	if (NULL == root)
		return;

	real_root = realpath(root, NULL);

	if (NULL == real_root)
		return;

	venture_test_remove_within(real_root, real_root);
}

/*
 * How many fresh ports a test tries before it gives up on starting a
 * server it had to name a port for in advance. Bounded, because a test
 * that can hang is worse than one that fails.
 */
#define VENTURE_TEST_PORT_ATTEMPTS (5)

/*
 * A loopback TCP port nothing was listening on a moment ago.
 *
 * Only for a server that has to be *told* its port before it starts -- a
 * subprocess given --port or $PORT. An in-process VentureWebServer never
 * needs this: configure it with port 0 and read the kernel's choice back
 * with venture_web_server_get_port(), which leaves no window at all.
 *
 * This one has a window. The socket is closed before the caller's server
 * binds, and anything may take the port in between, so the caller must
 * retry with a fresh port when the start fails because the port was taken
 * -- up to VENTURE_TEST_PORT_ATTEMPTS times.
 *
 * It is still far better than what it replaced. Ports used to be derived
 * from the pid, which collided with other suites running in other
 * worktrees or PID namespaces (equal pids, equal ports), with every other
 * listener on the machine, with sockets in TIME_WAIT and with a second
 * server started by the same process; the kernel's choice avoids all of
 * those, and SO_REUSEADDR is left off so a port with a lingering
 * connection is not the one handed back.
 */
static inline guint16
venture_test_free_port(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocket) socket = NULL;
	g_autoptr(GInetAddress) loopback = NULL;
	g_autoptr(GSocketAddress) any = NULL;
	g_autoptr(GSocketAddress) bound = NULL;
	guint16 port;

	socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_TCP, &error);
	g_assert_no_error(error);

	loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	any = g_inet_socket_address_new(loopback, 0);
	g_assert_true(g_socket_bind(socket, any, FALSE, &error));
	g_assert_no_error(error);

	bound = g_socket_get_local_address(socket, &error);
	g_assert_no_error(error);
	port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
	g_assert_cmpuint(port, >, 0);

	g_socket_close(socket, NULL);

	return port;
}
