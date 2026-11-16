/*
 * venture-formula-private.h - user formulas on the sandboxed suet VM
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A formula is one Tallow expression a person stores in a record: the
 * `expression` of a form rule, an alert rule or a lead scoring rule. It
 * reads `r`, the record it is asked about as JSON text, through the pure
 * accessors of the `formula` module (src/formula/venture-formula.tlw) and
 * yields a bool or an i64.
 *
 * The evaluator wraps the expression in a generated module -- `pure fn
 * venture_formula(r: str) -> bool { return (EXPR); }` -- after checking
 * that the text is one balanced expression that cannot close the wrapper,
 * compiles it on a suet VM with the sandbox on, no capability granted, no
 * GIR, and the step, memory and depth caps of the `formulas` settings,
 * and keeps the compiled VM in a bounded, thread-safe cache keyed by the
 * formula's text. Kernels are reachable through `venture_kernels`, bound
 * to their compiled C as for plugins.
 *
 * Server-only: the CLI never links the VM. docs/formulas.org is the guide.
 */

#ifndef VENTURE_FORMULA_PRIVATE_H
#define VENTURE_FORMULA_PRIVATE_H

#include <glib.h>

#include "venture.h"

G_BEGIN_DECLS

/**
 * VENTURE_FORMULA_MAX_LENGTH:
 *
 * The longest formula accepted, in bytes.
 */
#define VENTURE_FORMULA_MAX_LENGTH (4096)

/**
 * VentureFormulaKind:
 * @VENTURE_FORMULA_KIND_BOOL: a condition: the expression is a `bool`
 * @VENTURE_FORMULA_KIND_INT: a score: the expression is an `i64`
 *
 * What a formula yields. The kind is part of the cache key: the same text
 * compiled as a condition and as a score is two programs.
 */
typedef enum
{
	VENTURE_FORMULA_KIND_BOOL,
	VENTURE_FORMULA_KIND_INT
} VentureFormulaKind;

/**
 * venture_formula_configure:
 * @config: (nullable): the configuration, or %NULL for the defaults
 *
 * Reads the `formulas` settings (allow, max_steps, max_memory,
 * cache_size) and the Tallow standard library directory, and empties the
 * cache, so every formula is compiled again under the new limits. The
 * server calls it when a #VentureContext is made; a VM already lent out
 * finishes its call under the limits it was made with.
 */
void
venture_formula_configure(VentureConfig *config);

/**
 * venture_formula_check:
 * @expression: the formula's text
 * @kind: what it must yield
 * @error: (out) (optional): return location for a #GError
 *
 * Checks and compiles @expression as a record is saved. A refusal is a
 * %VENTURE_ERROR_VALIDATION whose message carries the Tallow diagnostic
 * with its line and column counted in @expression itself (line 1, column
 * 1 is the formula's first character). The compiled program is cached,
 * so the first evaluation does not compile it again.
 *
 * Returns: %TRUE when @expression is a formula of @kind
 */
gboolean
venture_formula_check(
	const gchar		 *expression,
	VentureFormulaKind	  kind,
	GError			**error
);

/**
 * venture_formula_fields:
 * @expression: the formula's text
 * @dynamic: (out) (optional): set when some accessor's field name is not
 *   a plain string literal, so the fields read cannot all be known
 * @error: (out) (optional): return location for a #GError
 *
 * The fields @expression reads: the plain string literal each accessor
 * of the `formula` module is called with, as in `field_str(r, "seats")`
 * or `r.i64_of("seats")`, each once, in the order they first appear. A
 * string compared as a value or quoted in a comment is not a field read.
 * @expression must have the shape of a formula; it is not compiled.
 *
 * Returns: (transfer full) (element-type utf8): the field names, or
 *   %NULL with @error set when @expression is not shaped as a formula
 */
GPtrArray *
venture_formula_fields(
	const gchar	 *expression,
	gboolean	 *dynamic,
	GError		**error
);

/**
 * venture_formula_eval_bool:
 * @expression: a condition formula
 * @record_json: the record as a JSON object, seen by the formula as `r`
 * @result: (out): what the formula yields
 * @error: (out) (optional): return location for a #GError
 *
 * Evaluates a condition. A formula that does not compile, panics (an
 * `.unwrap()` of none, a contract), or runs out of steps or memory is an
 * error (%VENTURE_ERROR_VALIDATION for the first, %VENTURE_ERROR_PLUGIN
 * for the rest), never an abort; what an error means -- no match, no
 * points -- is the caller's to say.
 *
 * Returns: %TRUE when the formula ran to a value
 */
gboolean
venture_formula_eval_bool(
	const gchar	 *expression,
	const gchar	 *record_json,
	gboolean	 *result,
	GError		**error
);

/**
 * venture_formula_eval_int:
 * @expression: a score formula
 * @record_json: the record as a JSON object, seen by the formula as `r`
 * @result: (out): what the formula yields
 * @error: (out) (optional): return location for a #GError
 *
 * Evaluates a score; errors as venture_formula_eval_bool().
 *
 * Returns: %TRUE when the formula ran to a value
 */
gboolean
venture_formula_eval_int(
	const gchar	 *expression,
	const gchar	 *record_json,
	gint64		 *result,
	GError		**error
);

/**
 * venture_formula_eval_bool_object: (skip)
 * @expression: a condition formula
 * @record: the record as a #JsonObject
 * @result: (out): what the formula yields
 * @error: (out) (optional): return location for a #GError
 *
 * venture_formula_eval_bool() for a record already parsed.
 *
 * Returns: %TRUE when the formula ran to a value
 */
gboolean
venture_formula_eval_bool_object(
	const gchar	 *expression,
	JsonObject	 *record,
	gboolean	 *result,
	GError		**error
);

/**
 * venture_formula_cache_size:
 *
 * Returns: how many compiled formulas the cache holds (for tests)
 */
guint
venture_formula_cache_size(void);

/**
 * venture_formula_compile_count:
 *
 * Returns: how many times a formula has been compiled since the process
 *   started (for tests: a cache hit compiles nothing)
 */
guint64
venture_formula_compile_count(void);

G_END_DECLS

#endif /* VENTURE_FORMULA_PRIVATE_H */
