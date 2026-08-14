/*
 * venture-version.c - Run-time version accessors
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These accessors report the version of the library actually linked into
 * the process. Code that was compiled against a different set of headers can
 * therefore detect the mismatch at run time -- which matters here because
 * plugins are compiled separately from the server that loads them.
 */

#include "venture.h"

const gchar *
venture_get_version_string(void)
{
	return VENTURE_VERSION_S;
}

guint
venture_get_version_major(void)
{
	return (guint)VENTURE_VERSION_MAJOR_S;
}

guint
venture_get_version_minor(void)
{
	return (guint)VENTURE_VERSION_MINOR_S;
}

guint
venture_get_version_micro(void)
{
	return (guint)VENTURE_VERSION_MICRO_S;
}

gboolean
venture_check_version(
	guint	major,
	guint	minor,
	guint	micro
){
	guint have_major;
	guint have_minor;
	guint have_micro;

	have_major = (guint)VENTURE_VERSION_MAJOR_S;
	have_minor = (guint)VENTURE_VERSION_MINOR_S;
	have_micro = (guint)VENTURE_VERSION_MICRO_S;

	/* Compare major, then minor, then micro. A higher component short
	 * circuits the comparison as satisfied; a lower one as unsatisfied. */
	if (have_major != major)
		return have_major > major;

	if (have_minor != minor)
		return have_minor > minor;

	return have_micro >= micro;
}
