/*
 * venture-jsonl.h - The JSON-lines protocol plugins and feeds speak
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One JSON object per line, each naming its `type`. An exec plugin writes
 * these on its standard output, and a `file_jsonl` data source reads the
 * same lines out of a file, so the vocabulary is defined once, here, and
 * documented once, in docs/plugins.org.
 *
 * The parser checks the envelope and the members each type declares --
 * that a price is a decimal string and never a JSON number, that a time
 * carries its zone, that a quantity is a whole number. It does not decide
 * what a message means: that is the consumer's business, and a consumer
 * that only wants records can ignore every listing without the parser
 * knowing.
 *
 * Nothing here touches the database or the configuration, so a reader can
 * run on any thread.
 */

#ifndef VENTURE_JSONL_H
#define VENTURE_JSONL_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_JSONL_PROTOCOL_VERSION:
 *
 * The protocol version this build speaks. A manifest or a source declares
 * the version it was written for, and anything else is refused rather
 * than read under rules it was not written to.
 */
#define VENTURE_JSONL_PROTOCOL_VERSION (1)

/**
 * VENTURE_JSONL_MAX_KEY_LENGTH:
 *
 * The longest key, name or identifier a message may carry, in bytes.
 */
#define VENTURE_JSONL_MAX_KEY_LENGTH (512)

/**
 * VENTURE_JSONL_MAX_TEXT_LENGTH:
 *
 * The longest free text (a title, a summary, a log line) a message may
 * carry, in bytes.
 */
#define VENTURE_JSONL_MAX_TEXT_LENGTH (16384)

/**
 * VENTURE_JSONL_MAX_DECIMAL_LENGTH:
 *
 * The longest decimal string accepted for a price or a ratio. Longer is
 * not a price, it is an attempt to make somebody's parser allocate.
 */
#define VENTURE_JSONL_MAX_DECIMAL_LENGTH (40)

/**
 * VENTURE_JSONL_MAX_ATTRS:
 *
 * The most members an account's `attrs` object may carry. Attributes are
 * a handful of scalars a page shows beside the account; a producer that
 * sends a thousand is dumping its state into a column nobody reads.
 */
#define VENTURE_JSONL_MAX_ATTRS (64)

/**
 * VENTURE_JSONL_MAX_ATTRS_BYTES:
 *
 * The longest an account's `attrs` object may be once serialised, in
 * bytes.
 */
#define VENTURE_JSONL_MAX_ATTRS_BYTES (8192)

typedef struct _VentureJsonlMessage VentureJsonlMessage;

#define VENTURE_TYPE_JSONL_MESSAGE (venture_jsonl_message_get_type())

GType venture_jsonl_message_get_type(void) G_GNUC_CONST;

/**
 * venture_jsonl_message_parse:
 * @line: one line of text, without its newline
 * @length: the length of @line in bytes, or -1 if it is NUL-terminated
 * @line_number: where the line came from, for error messages; 0 if unknown
 * @error: (out) (optional): return location for a #GError
 *
 * Parses and validates one line. A line that is not a JSON object, names
 * an unknown `type`, or carries a member of the wrong shape is refused
 * with %VENTURE_ERROR_SERIALIZATION and a message naming the line and the
 * member -- never the line's text, which may carry anything.
 *
 * Unknown members of the market-data types are ignored, so a later
 * protocol revision can add optional members without breaking an older
 * reader. The account-operations types (`login`, `account`,
 * `account_snapshot`, `balance`, `holding`, `position`, `inbound`, `txn`)
 * refuse one: their rows replace stored state, and a misspelt member --
 * `expiry` for
 * `expires_at` -- would otherwise erase a value without a word.
 *
 * Returns: (transfer full) (nullable): the message, or %NULL on error
 */
VentureJsonlMessage *
venture_jsonl_message_parse(
	const gchar	 *line,
	gssize		  length,
	guint		  line_number,
	GError		**error
);

/**
 * venture_jsonl_message_new_from_object:
 * @object: a JSON object, which the message references
 * @line_number: where it came from; 0 if unknown
 * @error: (out) (optional): return location for a #GError
 *
 * Validates an object that is already parsed, by the same rules as
 * venture_jsonl_message_parse().
 *
 * Returns: (transfer full) (nullable): the message, or %NULL on error
 */
VentureJsonlMessage *
venture_jsonl_message_new_from_object(
	JsonObject	 *object,
	guint		  line_number,
	GError		**error
);

/**
 * venture_jsonl_message_ref:
 * @self: a #VentureJsonlMessage
 *
 * Returns: (transfer full): @self
 */
VentureJsonlMessage *
venture_jsonl_message_ref(VentureJsonlMessage *self);

/**
 * venture_jsonl_message_unref:
 * @self: (transfer full): a #VentureJsonlMessage
 */
void
venture_jsonl_message_unref(VentureJsonlMessage *self);

/**
 * venture_jsonl_message_get_kind:
 * @self: a #VentureJsonlMessage
 *
 * Returns: which message this is
 */
VentureJsonlMessageKind
venture_jsonl_message_get_kind(VentureJsonlMessage *self);

/**
 * venture_jsonl_message_get_kind_name:
 * @self: a #VentureJsonlMessage
 *
 * Returns: (transfer none): the message's `type`, as written on the wire
 */
const gchar *
venture_jsonl_message_get_kind_name(VentureJsonlMessage *self);

/**
 * venture_jsonl_message_get_object:
 * @self: a #VentureJsonlMessage
 *
 * Returns: (transfer none): the whole message, `type` included
 */
JsonObject *
venture_jsonl_message_get_object(VentureJsonlMessage *self);

/**
 * venture_jsonl_message_get_line:
 * @self: a #VentureJsonlMessage
 *
 * Returns: the line it was read from, or 0 if unknown
 */
guint
venture_jsonl_message_get_line(VentureJsonlMessage *self);

/**
 * venture_jsonl_message_get_string:
 * @self: a #VentureJsonlMessage
 * @member: a member name
 *
 * A convenience for the string members every type has. A member that is
 * absent, null or not a string answers %NULL.
 *
 * Returns: (transfer none) (nullable): the value
 */
const gchar *
venture_jsonl_message_get_string(
	VentureJsonlMessage	*self,
	const gchar		*member
);

/**
 * venture_jsonl_message_get_int:
 * @self: a #VentureJsonlMessage
 * @member: a member name
 * @fallback: returned when the member is absent or null
 *
 * Returns: the whole-number value, or @fallback
 */
gint64
venture_jsonl_message_get_int(
	VentureJsonlMessage	*self,
	const gchar		*member,
	gint64			 fallback
);

/**
 * venture_jsonl_kind_from_name:
 * @name: (nullable): a `type` as written on the wire
 * @out_kind: (out) (optional): the kind
 *
 * An exact, case-sensitive lookup. The forgiving
 * venture_enum_from_nick() is deliberately not used: the wire spelling is
 * the protocol.
 *
 * Returns: %TRUE if @name is a protocol-1 message type
 */
gboolean
venture_jsonl_kind_from_name(
	const gchar		*name,
	VentureJsonlMessageKind	*out_kind
);

/**
 * venture_jsonl_is_decimal:
 * @text: (nullable): a candidate
 * @allow_negative: whether a leading minus is accepted
 *
 * The protocol's spelling of an exact number: digits, optionally a point
 * and more digits, optionally a leading minus. No exponent, no leading
 * plus, no bare point, and at most %VENTURE_JSONL_MAX_DECIMAL_LENGTH bytes.
 * Prices travel as these strings because a JSON number is a double, and
 * money is never a double.
 *
 * Returns: %TRUE if @text is a decimal
 */
gboolean
venture_jsonl_is_decimal(
	const gchar	*text,
	gboolean	 allow_negative
);

typedef struct _VentureJsonlReader VentureJsonlReader;

/**
 * venture_jsonl_reader_new:
 * @max_line: the longest line accepted, in bytes; 0 for no limit beyond
 *   whatever the caller caps the whole input at
 *
 * Creates an incremental reader: feed it bytes as they arrive, in chunks
 * of any size, and it hands back each complete message. An exec plugin's
 * output and a JSON-lines file go through the same reader, so a line split
 * across two reads is the reader's problem, once.
 *
 * Blank lines are skipped. A carriage return before the newline is
 * dropped, so a file written on another system reads the same.
 *
 * Returns: (transfer full): a new reader; free with
 *   venture_jsonl_reader_free()
 */
VentureJsonlReader *
venture_jsonl_reader_new(gsize max_line);

/**
 * venture_jsonl_reader_free:
 * @self: (nullable) (transfer full): a reader
 */
void
venture_jsonl_reader_free(VentureJsonlReader *self);

/**
 * venture_jsonl_reader_feed:
 * @self: a reader
 * @data: (array length=length): bytes
 * @length: how many
 * @out_messages: (element-type VentureJsonlMessage): where complete
 *   messages are appended; the array must free its elements with
 *   venture_jsonl_message_unref()
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %FALSE at the first line that is refused; the reader is then
 *   unusable
 */
gboolean
venture_jsonl_reader_feed(
	VentureJsonlReader	 *self,
	const guint8		 *data,
	gsize			  length,
	GPtrArray		 *out_messages,
	GError			**error
);

/**
 * venture_jsonl_reader_finish:
 * @self: a reader
 * @out_messages: (element-type VentureJsonlMessage): as for
 *   venture_jsonl_reader_feed()
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a final line that had no newline. Call it once, at end of input.
 *
 * Returns: %FALSE if that line is refused
 */
gboolean
venture_jsonl_reader_finish(
	VentureJsonlReader	 *self,
	GPtrArray		 *out_messages,
	GError			**error
);

/**
 * venture_jsonl_reader_get_line:
 * @self: a reader
 *
 * Returns: how many lines have been read so far, blank ones included
 */
guint
venture_jsonl_reader_get_line(VentureJsonlReader *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureJsonlMessage, venture_jsonl_message_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureJsonlReader, venture_jsonl_reader_free)

G_END_DECLS

#endif /* VENTURE_JSONL_H */
