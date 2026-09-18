/*
 * venture-error.c - The VENTURE error domain
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <stdarg.h>

/*
 * The slug table is the single source of truth for the machine-readable
 * name of each code. It is indexed by the code itself, so the order here
 * must match the order of #VentureError exactly. The trailing NULL lets the
 * lookup helpers detect the end without a separate count.
 */
static const gchar *const venture_error_slugs[] = {
	"failed",
	"invalid_argument",
	"not_found",
	"already_exists",
	"conflict",
	"validation",
	"unauthenticated",
	"permission_denied",
	"database",
	"migration",
	"config",
	"plugin",
	"ai",
	"ai_confirmation_required",
	"automation",
	"serialization",
	"unsupported",
	"timeout",
	"network",
	"balance",
	"mail_uncertain",
	"mail_transient",
	"mail_permanent",
	"docs_broken_link",
	"docs_unlinked",
	NULL
};

static const GEnumValue venture_error_values[] = {
	{ VENTURE_ERROR_FAILED,                   "VENTURE_ERROR_FAILED",                   "failed" },
	{ VENTURE_ERROR_INVALID_ARGUMENT,         "VENTURE_ERROR_INVALID_ARGUMENT",         "invalid_argument" },
	{ VENTURE_ERROR_NOT_FOUND,                "VENTURE_ERROR_NOT_FOUND",                "not_found" },
	{ VENTURE_ERROR_ALREADY_EXISTS,           "VENTURE_ERROR_ALREADY_EXISTS",           "already_exists" },
	{ VENTURE_ERROR_CONFLICT,                 "VENTURE_ERROR_CONFLICT",                 "conflict" },
	{ VENTURE_ERROR_VALIDATION,               "VENTURE_ERROR_VALIDATION",               "validation" },
	{ VENTURE_ERROR_UNAUTHENTICATED,          "VENTURE_ERROR_UNAUTHENTICATED",          "unauthenticated" },
	{ VENTURE_ERROR_PERMISSION_DENIED,        "VENTURE_ERROR_PERMISSION_DENIED",        "permission_denied" },
	{ VENTURE_ERROR_DATABASE,                 "VENTURE_ERROR_DATABASE",                 "database" },
	{ VENTURE_ERROR_MIGRATION,                "VENTURE_ERROR_MIGRATION",                "migration" },
	{ VENTURE_ERROR_CONFIG,                   "VENTURE_ERROR_CONFIG",                   "config" },
	{ VENTURE_ERROR_PLUGIN,                   "VENTURE_ERROR_PLUGIN",                   "plugin" },
	{ VENTURE_ERROR_AI,                       "VENTURE_ERROR_AI",                       "ai" },
	{ VENTURE_ERROR_AI_CONFIRMATION_REQUIRED, "VENTURE_ERROR_AI_CONFIRMATION_REQUIRED", "ai_confirmation_required" },
	{ VENTURE_ERROR_AUTOMATION,               "VENTURE_ERROR_AUTOMATION",               "automation" },
	{ VENTURE_ERROR_SERIALIZATION,            "VENTURE_ERROR_SERIALIZATION",            "serialization" },
	{ VENTURE_ERROR_UNSUPPORTED,              "VENTURE_ERROR_UNSUPPORTED",              "unsupported" },
	{ VENTURE_ERROR_TIMEOUT,                  "VENTURE_ERROR_TIMEOUT",                  "timeout" },
	{ VENTURE_ERROR_NETWORK,                  "VENTURE_ERROR_NETWORK",                  "network" },
	{ VENTURE_ERROR_BALANCE,                  "VENTURE_ERROR_BALANCE",                  "balance" },
	{ VENTURE_ERROR_MAIL_UNCERTAIN, "VENTURE_ERROR_MAIL_UNCERTAIN", "mail_uncertain" },
	{ VENTURE_ERROR_MAIL_TRANSIENT, "VENTURE_ERROR_MAIL_TRANSIENT", "mail_transient" },
	{ VENTURE_ERROR_MAIL_PERMANENT, "VENTURE_ERROR_MAIL_PERMANENT", "mail_permanent" },
	{ VENTURE_ERROR_DOCS_BROKEN_LINK, "VENTURE_ERROR_DOCS_BROKEN_LINK", "docs_broken_link" },
	{ VENTURE_ERROR_DOCS_UNLINKED, "VENTURE_ERROR_DOCS_UNLINKED", "docs_unlinked" },
	{ 0, NULL, NULL }
};

GType
venture_error_get_type(void)
{
	static gsize venture_error_type_id = 0;

	if (g_once_init_enter(&venture_error_type_id))
	{
		GType registered;

		registered = g_enum_register_static("VentureError",
		                                    venture_error_values);
		g_once_init_leave(&venture_error_type_id, registered);
	}

	return (GType)venture_error_type_id;
}

G_DEFINE_QUARK(venture-error-quark, venture_error)

guint
venture_error_to_http_status(VentureError code)
{
	switch (code)
	{
	case VENTURE_ERROR_INVALID_ARGUMENT:
	case VENTURE_ERROR_SERIALIZATION:
		return 400;

	case VENTURE_ERROR_UNAUTHENTICATED:
		return 401;

	case VENTURE_ERROR_PERMISSION_DENIED:
		return 403;

	case VENTURE_ERROR_NOT_FOUND:
		return 404;

	case VENTURE_ERROR_ALREADY_EXISTS:
	case VENTURE_ERROR_CONFLICT:
		return 409;

	/* 422 is the right answer for a well-formed request whose contents
	 * are semantically wrong -- a ledger that does not balance, a date
	 * range that runs backwards, a required field left blank. */
	case VENTURE_ERROR_VALIDATION:
	case VENTURE_ERROR_BALANCE:
	case VENTURE_ERROR_DOCS_BROKEN_LINK:
	case VENTURE_ERROR_DOCS_UNLINKED:
		return 422;

	/* A staged AI write is not an error the client did anything wrong to
	 * cause; 202 Accepted communicates "understood, awaiting approval",
	 * and the response body carries the confirmation to approve. */
	case VENTURE_ERROR_AI_CONFIRMATION_REQUIRED:
		return 202;

	case VENTURE_ERROR_UNSUPPORTED:
		return 501;

	case VENTURE_ERROR_NETWORK:
		return 502;

	case VENTURE_ERROR_TIMEOUT:
		return 504;

	case VENTURE_ERROR_FAILED:
	case VENTURE_ERROR_DATABASE:
	case VENTURE_ERROR_MIGRATION:
	case VENTURE_ERROR_CONFIG:
	case VENTURE_ERROR_PLUGIN:
	case VENTURE_ERROR_AI:
	case VENTURE_ERROR_AUTOMATION:
	default:
		return 500;
	}
}

gint
venture_error_to_exit_code(VentureError code)
{
	/* Small, stable exit codes so a shell script can branch:
	 *   1 general failure   2 usage      3 not found
	 *   4 conflict          5 auth       6 unsupported
	 *   7 network/timeout   8 validation                       */
	switch (code)
	{
	case VENTURE_ERROR_INVALID_ARGUMENT:
		return 2;

	case VENTURE_ERROR_NOT_FOUND:
		return 3;

	case VENTURE_ERROR_ALREADY_EXISTS:
	case VENTURE_ERROR_CONFLICT:
		return 4;

	case VENTURE_ERROR_UNAUTHENTICATED:
	case VENTURE_ERROR_PERMISSION_DENIED:
		return 5;

	case VENTURE_ERROR_UNSUPPORTED:
		return 6;

	case VENTURE_ERROR_NETWORK:
	case VENTURE_ERROR_TIMEOUT:
		return 7;

	case VENTURE_ERROR_VALIDATION:
	case VENTURE_ERROR_BALANCE:
	case VENTURE_ERROR_DOCS_BROKEN_LINK:
	case VENTURE_ERROR_DOCS_UNLINKED:
		return 8;

	default:
		return 1;
	}
}

const gchar *
venture_error_get_slug(VentureError code)
{
	guint index;

	index = (guint)code;

	/* G_N_ELEMENTS counts the trailing NULL, so the last valid index is
	 * two less than the element count. */
	if (index >= G_N_ELEMENTS(venture_error_slugs) - 1)
		return "failed";

	return venture_error_slugs[index];
}

gboolean
venture_error_from_slug(
	const gchar	*slug,
	VentureError	*out_code
){
	guint i;

	g_return_val_if_fail(NULL != slug, FALSE);
	g_return_val_if_fail(NULL != out_code, FALSE);

	for (i = 0; NULL != venture_error_slugs[i]; i++)
	{
		if (0 == g_strcmp0(venture_error_slugs[i], slug))
		{
			*out_code = (VentureError)i;
			return TRUE;
		}
	}

	return FALSE;
}

void
venture_set_error_validation(
	GError		**error,
	const gchar	 *field,
	const gchar	 *format,
	...
){
	g_autofree gchar *message = NULL;
	va_list args;

	/* Nothing to do if the caller does not want the error; formatting the
	 * message would be wasted work. */
	if (NULL == error)
		return;

	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);

	/* Prefixing the field name keeps the message self-describing when it
	 * is shown on its own, and lets the web UI split on ": " to find the
	 * input to highlight. */
	if (NULL != field)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s: %s", field, message);
	}
	else
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_VALIDATION, message);
	}
}
