/*
 * venture-error.h - The VENTURE error domain
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A single error domain covers the whole system. The codes are coarse on
 * purpose: they map onto HTTP status codes for the REST API and onto process
 * exit codes for venturectl, so a failure deep in the database layer surfaces
 * as a sensible 409 or a sensible exit status without every layer having to
 * translate between vocabularies.
 */

#ifndef VENTURE_ERROR_H
#define VENTURE_ERROR_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_ERROR:
 *
 * The #GQuark identifying the VENTURE error domain.
 */
#define VENTURE_ERROR (venture_error_quark())

/**
 * VentureError:
 * @VENTURE_ERROR_FAILED: a generic failure with no better code
 * @VENTURE_ERROR_INVALID_ARGUMENT: a caller passed something unusable
 * @VENTURE_ERROR_NOT_FOUND: the requested record or resource does not exist
 * @VENTURE_ERROR_ALREADY_EXISTS: a uniqueness constraint would be violated
 * @VENTURE_ERROR_CONFLICT: the operation conflicts with the current state,
 *   for example an optimistic-concurrency mismatch
 * @VENTURE_ERROR_VALIDATION: a record failed its own validation rules
 * @VENTURE_ERROR_UNAUTHENTICATED: no valid credentials were presented
 * @VENTURE_ERROR_PERMISSION_DENIED: credentials were valid but insufficient
 * @VENTURE_ERROR_DATABASE: the storage layer reported a failure
 * @VENTURE_ERROR_MIGRATION: a schema migration could not be applied
 * @VENTURE_ERROR_CONFIG: the configuration is missing or malformed
 * @VENTURE_ERROR_PLUGIN: a plugin failed to load, register or run
 * @VENTURE_ERROR_AI: an AI provider or tool call failed
 * @VENTURE_ERROR_AI_CONFIRMATION_REQUIRED: a mutation that policy requires a
 *   human to approve first; the accompanying #VentureConfirmation identifier
 *   is in the error message
 * @VENTURE_ERROR_AUTOMATION: an automation binding or the engine failed
 * @VENTURE_ERROR_SERIALIZATION: a value could not be encoded or decoded
 * @VENTURE_ERROR_UNSUPPORTED: the operation is not supported by this build,
 *   for example a PostgreSQL URI in a build without libpq
 * @VENTURE_ERROR_TIMEOUT: the operation did not complete in time
 * @VENTURE_ERROR_NETWORK: a transport-level failure talking to the server
 *   or to an external service
 * @VENTURE_ERROR_BALANCE: a double-entry transaction does not balance
 *
 * Error codes in the %VENTURE_ERROR domain.
 */
typedef enum
{
	VENTURE_ERROR_FAILED = 0,
	VENTURE_ERROR_INVALID_ARGUMENT,
	VENTURE_ERROR_NOT_FOUND,
	VENTURE_ERROR_ALREADY_EXISTS,
	VENTURE_ERROR_CONFLICT,
	VENTURE_ERROR_VALIDATION,
	VENTURE_ERROR_UNAUTHENTICATED,
	VENTURE_ERROR_PERMISSION_DENIED,
	VENTURE_ERROR_DATABASE,
	VENTURE_ERROR_MIGRATION,
	VENTURE_ERROR_CONFIG,
	VENTURE_ERROR_PLUGIN,
	VENTURE_ERROR_AI,
	VENTURE_ERROR_AI_CONFIRMATION_REQUIRED,
	VENTURE_ERROR_AUTOMATION,
	VENTURE_ERROR_SERIALIZATION,
	VENTURE_ERROR_UNSUPPORTED,
	VENTURE_ERROR_TIMEOUT,
	VENTURE_ERROR_NETWORK,
	VENTURE_ERROR_BALANCE,
	VENTURE_ERROR_MAIL_UNCERTAIN,
	VENTURE_ERROR_MAIL_TRANSIENT,
	VENTURE_ERROR_MAIL_PERMANENT,
	VENTURE_ERROR_DOCS_BROKEN_LINK,
	VENTURE_ERROR_DOCS_UNLINKED
} VentureError;

#define VENTURE_TYPE_ERROR (venture_error_get_type())

GType
venture_error_get_type(void) G_GNUC_CONST;

/**
 * venture_error_quark:
 *
 * Retrieves the #GQuark for the VENTURE error domain.
 *
 * Returns: the error domain quark
 */
GQuark
venture_error_quark(void);

/**
 * venture_error_to_http_status:
 * @code: a #VentureError code
 *
 * Maps an error code to the HTTP status the REST API should return for it.
 * Unknown codes map to 500.
 *
 * Returns: an HTTP status code
 */
guint
venture_error_to_http_status(VentureError code);

/**
 * venture_error_to_exit_code:
 * @code: a #VentureError code
 *
 * Maps an error code to the process exit status venturectl should use.
 * These follow the usual convention of 1 for a general failure with
 * distinct small integers for the cases a script is likely to branch on
 * (not found, permission denied, conflict).
 *
 * Returns: a process exit code
 */
gint
venture_error_to_exit_code(VentureError code);

/**
 * venture_error_get_slug:
 * @code: a #VentureError code
 *
 * Retrieves the stable machine-readable slug for an error code, for example
 * "not_found". This is what appears in the "error" field of a JSON error
 * response, so clients can branch on it without parsing prose.
 *
 * Returns: (transfer none): the slug
 */
const gchar *
venture_error_get_slug(VentureError code);

/**
 * venture_error_from_slug:
 * @slug: a slug previously produced by venture_error_get_slug()
 * @out_code: (out): return location for the code
 *
 * Reverses venture_error_get_slug(), so venturectl can reconstruct a real
 * #GError from a JSON error body returned by the server.
 *
 * Returns: %TRUE if @slug was recognised
 */
gboolean
venture_error_from_slug(
	const gchar	*slug,
	VentureError	*out_code
);

/**
 * venture_set_error_validation:
 * @error: (out) (optional): return location for a #GError
 * @field: (nullable): the name of the offending field
 * @format: a printf-style format string
 * @...: format arguments
 *
 * Convenience for raising %VENTURE_ERROR_VALIDATION with the field name
 * prefixed onto the message, which is the form the web UI expects so it can
 * highlight the right input.
 */
void
venture_set_error_validation(
	GError		**error,
	const gchar	 *field,
	const gchar	 *format,
	...
) G_GNUC_PRINTF(3, 4);

G_END_DECLS

#endif /* VENTURE_ERROR_H */
