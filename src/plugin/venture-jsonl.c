/*
 * venture-jsonl.c - The JSON-lines protocol plugins and feeds speak
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

struct _VentureJsonlMessage
{
	gint			 ref_count;
	VentureJsonlMessageKind	 kind;
	JsonObject		*object;
	guint			 line;
};

G_DEFINE_BOXED_TYPE(VentureJsonlMessage, venture_jsonl_message,
                    venture_jsonl_message_ref, venture_jsonl_message_unref)

struct _VentureJsonlReader
{
	GByteArray	*pending;	/* bytes of a line not yet ended */
	gsize		 max_line;
	guint		 line;
	gboolean	 failed;
};

/* ==========================================================================
 * Member checks
 *
 * Each returns FALSE with an error naming the line, the message type and
 * the member. None of them quotes the value: a line can carry a secret the
 * producer printed by mistake, and an error message travels further than
 * the line ever would.
 * ========================================================================== */

static gboolean
jsonl_refuse(
	GError		**error,
	guint		  line,
	const gchar	 *type,
	const gchar	 *member,
	const gchar	 *problem
){
	if (0 != line)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "Line %u: %s%s%s %s", line,
		            (NULL != type) ? type : "message",
		            (NULL != member) ? "." : "",
		            (NULL != member) ? member : "", problem);
	}
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "%s%s%s %s",
		            (NULL != type) ? type : "message",
		            (NULL != member) ? "." : "",
		            (NULL != member) ? member : "", problem);
	}

	return FALSE;
}

/*
 * Fetches a member that is present and not null. JSON null is read as
 * absent everywhere, because a producer serialising an optional field it
 * has no value for will write null, and refusing that would refuse every
 * ordinary serialiser.
 */
static JsonNode *
jsonl_member(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	if (!json_object_has_member(object, member))
		return NULL;

	node = json_object_get_member(object, member);

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return NULL;

	return node;
}

/*
 * A string member, or NULL when it is absent, null or another shape.
 */
static const gchar *
jsonl_string(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = jsonl_member(object, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return json_node_get_string(node);
}

static gboolean
jsonl_check_string(
	JsonObject	 *object,
	const gchar	 *type,
	const gchar	 *member,
	gboolean	  required,
	gsize		  max_length,
	guint		  line,
	GError		**error
){
	JsonNode *node;
	const gchar *value;

	node = jsonl_member(object, member);

	if (NULL == node)
	{
		if (required)
			return jsonl_refuse(error, line, type, member, "is required");

		return TRUE;
	}

	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return jsonl_refuse(error, line, type, member, "must be a string");

	value = json_node_get_string(node);

	if (required && ('\0' == value[0]))
		return jsonl_refuse(error, line, type, member, "must not be empty");

	if (strlen(value) > max_length)
		return jsonl_refuse(error, line, type, member, "is too long");

	return TRUE;
}

static gboolean
jsonl_check_choice(
	JsonObject		 *object,
	const gchar		 *type,
	const gchar		 *member,
	const gchar *const	 *choices,
	guint			  line,
	GError			**error
){
	JsonNode *node;

	if (!jsonl_check_string(object, type, member, FALSE,
	                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error))
		return FALSE;

	node = jsonl_member(object, member);

	if ((NULL != node) &&
	    !g_strv_contains(choices, json_node_get_string(node)))
	{
		g_autofree gchar *joined = NULL;
		g_autofree gchar *problem = NULL;

		joined = g_strjoinv(", ", (gchar **)choices);
		problem = g_strdup_printf("must be one of %s", joined);

		return jsonl_refuse(error, line, type, member, problem);
	}

	return TRUE;
}

static gboolean
jsonl_check_object(
	JsonObject	 *object,
	const gchar	 *type,
	const gchar	 *member,
	gboolean	  required,
	guint		  line,
	GError		**error
){
	JsonNode *node;

	node = jsonl_member(object, member);

	if (NULL == node)
	{
		if (required)
			return jsonl_refuse(error, line, type, member, "is required");

		return TRUE;
	}

	if (!JSON_NODE_HOLDS_OBJECT(node))
		return jsonl_refuse(error, line, type, member, "must be an object");

	return TRUE;
}

static gboolean
jsonl_check_bool(
	JsonObject	 *object,
	const gchar	 *type,
	const gchar	 *member,
	guint		  line,
	GError		**error
){
	JsonNode *node;

	node = jsonl_member(object, member);

	if ((NULL != node) &&
	    (!JSON_NODE_HOLDS_VALUE(node) ||
	     (G_TYPE_BOOLEAN != json_node_get_value_type(node))))
		return jsonl_refuse(error, line, type, member, "must be true or false");

	return TRUE;
}

/*
 * A whole number, as a JSON integer. A count of units written as 2.5 is a
 * producer bug, and silently truncating it would book half a unit that
 * never existed.
 */
static gboolean
jsonl_check_int(
	JsonObject	 *object,
	const gchar	 *type,
	const gchar	 *member,
	gint64		  minimum,
	guint		  line,
	GError		**error
){
	JsonNode *node;

	node = jsonl_member(object, member);

	if (NULL == node)
		return TRUE;

	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_INT64 != json_node_get_value_type(node)))
		return jsonl_refuse(error, line, type, member,
		                    "must be a whole number");

	if (json_node_get_int(node) < minimum)
	{
		g_autofree gchar *problem = NULL;

		problem = g_strdup_printf("must be at least %" G_GINT64_FORMAT,
		                          minimum);
		return jsonl_refuse(error, line, type, member, problem);
	}

	return TRUE;
}

/*
 * An exact amount. A JSON number is refused outright, even an integer: a
 * producer that writes 12 for a price will write 12.1 for the next one, and
 * that is a double on the way in.
 */
static gboolean
jsonl_check_decimal(
	JsonObject	 *object,
	const gchar	 *type,
	const gchar	 *member,
	gboolean	  required,
	gboolean	  allow_negative,
	guint		  line,
	GError		**error
){
	JsonNode *node;

	node = jsonl_member(object, member);

	if (NULL == node)
	{
		if (required)
			return jsonl_refuse(error, line, type, member, "is required");

		return TRUE;
	}

	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return jsonl_refuse(error, line, type, member,
		                    "must be a decimal string such as \"12.50\", "
		                    "never a JSON number");

	if (!venture_jsonl_is_decimal(json_node_get_string(node), allow_negative))
		return jsonl_refuse(error, line, type, member,
		                    allow_negative
		                        ? "must be a decimal such as \"-12.50\""
		                        : "must be a non-negative decimal such as "
		                          "\"12.50\"");

	return TRUE;
}

static gboolean
jsonl_check_currency(
	JsonObject	 *object,
	const gchar	 *type,
	guint		  line,
	GError		**error
){
	JsonNode *node;

	if (!jsonl_check_string(object, type, "currency", FALSE,
	                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error))
		return FALSE;

	node = jsonl_member(object, "currency");

	if ((NULL != node) && !venture_currency_is_valid(json_node_get_string(node)))
		return jsonl_refuse(error, line, type, "currency",
		                    "is not a currency code");

	return TRUE;
}

/*
 * A moment, with its zone. A time with no offset means whatever the
 * reader's clock means, and two readers in two zones would then file the
 * same snapshot an hour apart.
 */
static gboolean
jsonl_check_time(
	JsonObject	 *object,
	const gchar	 *type,
	const gchar	 *member,
	gboolean	  required,
	guint		  line,
	GError		**error
){
	g_autoptr(GDateTime) parsed = NULL;
	JsonNode *node;

	if (!jsonl_check_string(object, type, member, required,
	                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error))
		return FALSE;

	node = jsonl_member(object, member);

	if (NULL == node)
		return TRUE;

	parsed = g_date_time_new_from_iso8601(json_node_get_string(node), NULL);

	if (NULL == parsed)
		return jsonl_refuse(error, line, type, member,
		                    "must be an ISO 8601 time with a zone, such as "
		                    "\"2026-10-03T12:00:00Z\"");

	return TRUE;
}

static gboolean
jsonl_check_url(
	JsonObject	 *object,
	const gchar	 *type,
	const gchar	 *member,
	guint		  line,
	GError		**error
){
	JsonNode *node;
	const gchar *value;

	if (!jsonl_check_string(object, type, member, FALSE,
	                        VENTURE_JSONL_MAX_TEXT_LENGTH, line, error))
		return FALSE;

	node = jsonl_member(object, member);

	if (NULL == node)
		return TRUE;

	value = json_node_get_string(node);

	/* Only a web address. A javascript: or file: link from a feed would
	 * otherwise reach a page that renders it. */
	if (!g_str_has_prefix(value, "https://") &&
	    !g_str_has_prefix(value, "http://"))
		return jsonl_refuse(error, line, type, member,
		                    "must be an http or https address");

	return TRUE;
}

/*
 * Odds in one of the three spellings bookmakers use. Validated for shape
 * and for being a possible price: decimal odds of 1.0 or less pay nothing
 * back, an American line inside (-100, 100) does not exist, and a
 * fractional price needs a positive numerator and denominator.
 */
static gboolean
jsonl_check_odds(
	const gchar	*odds,
	const gchar	*format
){
	if ((NULL == format) || (0 == g_strcmp0(format, "decimal")))
	{
		if (!venture_jsonl_is_decimal(odds, FALSE))
			return FALSE;

		/* A ratio may be a double; this compares, it does not store. */
		return g_ascii_strtod(odds, NULL) > 1.0;
	}

	if (0 == g_strcmp0(format, "american"))
	{
		const gchar *digits;
		gint64 value;

		digits = (('+' == odds[0]) || ('-' == odds[0])) ? odds + 1 : odds;

		if (('\0' == digits[0]) || (strlen(digits) > 12))
			return FALSE;

		if (strspn(digits, "0123456789") != strlen(digits))
			return FALSE;

		value = g_ascii_strtoll(digits, NULL, 10);

		return value >= 100;
	}

	if (0 == g_strcmp0(format, "fractional"))
	{
		g_auto(GStrv) parts = NULL;
		gsize i;

		parts = g_strsplit(odds, "/", -1);

		if (2 != g_strv_length(parts))
			return FALSE;

		for (i = 0; i < 2; i++)
		{
			if (('\0' == parts[i][0]) || (strlen(parts[i]) > 12) ||
			    (strspn(parts[i], "0123456789") != strlen(parts[i])) ||
			    (0 == g_ascii_strtoll(parts[i], NULL, 10)))
				return FALSE;
		}

		return TRUE;
	}

	return FALSE;
}


/* ==========================================================================
 * The account-operations types
 *
 * Their rows replace what a store holds -- a snapshot of an account erases
 * every holding it does not restate -- so these types are strict where the
 * market-data types are forgiving: a member the type does not define is
 * refused, not ignored. A producer that spells `expires_at` as `expiry`
 * would otherwise store listings that never expire, and nothing would say
 * so.
 * ========================================================================== */

static const gchar *const jsonl_account_kinds[] = {
	"character", "shared", "guild", "other", NULL
};
static const gchar *const jsonl_login_kinds[] = {
	"game_account", "platform_account", "other", NULL
};
static const gchar *const jsonl_covers[] = {
	"holdings", "positions", "inbound", "balances", NULL
};
static const gchar *const jsonl_places[] = {
	"bag", "bank", "reagent_bank", "warbank", "guild", "mail", "auction",
	"void", "equipped", "currency", "other", NULL
};
static const gchar *const jsonl_txn_kinds[] = {
	"sale", "buy", "income", "expense", "expired", "cancelled", NULL
};

/*
 * Refuses a member @allowed does not name. The member's name is said when
 * it looks like one -- short lower-case ASCII -- because "unknown member
 * expiry" is the whole diagnosis; anything else could be producer text of
 * any length and is not echoed.
 */
static gboolean
jsonl_check_members(
	JsonObject		 *object,
	const gchar		 *type,
	const gchar *const	 *allowed,
	guint			  line,
	GError			**error
){
	JsonObjectIter iter;
	const gchar *member;
	JsonNode *node;

	json_object_iter_init(&iter, object);

	while (json_object_iter_next(&iter, &member, &node))
	{
		gsize length;

		if ((0 == strcmp(member, "type")) || g_strv_contains(allowed, member))
			continue;

		length = strlen(member);

		if ((length > 0) && (length <= 64) &&
		    (strspn(member, "abcdefghijklmnopqrstuvwxyz0123456789_") == length))
		{
			g_autofree gchar *problem = NULL;

			problem = g_strdup_printf("is not a member of %s (unknown members are "
			                          "refused for this type)", type);
			return jsonl_refuse(error, line, type, member, problem);
		}

		return jsonl_refuse(error, line, type, NULL,
		                    "carries a member this type does not define");
	}

	return TRUE;
}

/*
 * An account's attributes: an object of scalars, bounded in members and
 * in size. A nested object or array is refused -- attributes are shown
 * beside the account, not walked.
 */
static gboolean
jsonl_check_attrs(
	JsonObject	 *object,
	const gchar	 *type,
	guint		  line,
	GError		**error
){
	g_autoptr(JsonGenerator) generator = NULL;
	g_autofree gchar *text = NULL;
	JsonObjectIter iter;
	const gchar *member;
	JsonNode *node;
	JsonNode *attrs;
	gsize length;

	if (!jsonl_check_object(object, type, "attrs", FALSE, line, error))
		return FALSE;

	attrs = jsonl_member(object, "attrs");

	if (NULL == attrs)
		return TRUE;

	if (json_object_get_size(json_node_get_object(attrs)) > VENTURE_JSONL_MAX_ATTRS)
	{
		g_autofree gchar *problem = NULL;

		problem = g_strdup_printf("may carry at most %d members", VENTURE_JSONL_MAX_ATTRS);
		return jsonl_refuse(error, line, type, "attrs", problem);
	}

	json_object_iter_init(&iter, json_node_get_object(attrs));

	while (json_object_iter_next(&iter, &member, &node))
	{
		if (strlen(member) > VENTURE_JSONL_MAX_KEY_LENGTH)
			return jsonl_refuse(error, line, type, "attrs",
			                    "has a member name that is too long");

		if (JSON_NODE_HOLDS_NULL(node))
			continue;

		if (!JSON_NODE_HOLDS_VALUE(node))
			return jsonl_refuse(error, line, type, "attrs",
			                    "must hold only strings, numbers and booleans");

		if ((G_TYPE_STRING == json_node_get_value_type(node)) &&
		    (strlen(json_node_get_string(node)) > VENTURE_JSONL_MAX_KEY_LENGTH))
			return jsonl_refuse(error, line, type, "attrs",
			                    "has a value that is too long");
	}

	generator = json_generator_new();
	json_generator_set_root(generator, attrs);
	text = json_generator_to_data(generator, &length);

	if (length > VENTURE_JSONL_MAX_ATTRS_BYTES)
	{
		g_autofree gchar *problem = NULL;

		problem = g_strdup_printf("is longer than %d bytes", VENTURE_JSONL_MAX_ATTRS_BYTES);
		return jsonl_refuse(error, line, type, "attrs", problem);
	}

	return TRUE;
}

/* A choice that must be present. */
static gboolean
jsonl_check_required_choice(
	JsonObject		 *object,
	const gchar		 *type,
	const gchar		 *member,
	const gchar *const	 *choices,
	guint			  line,
	GError			**error
){
	if (NULL == jsonl_member(object, member))
		return jsonl_refuse(error, line, type, member, "is required");

	return jsonl_check_choice(object, type, member, choices, line, error);
}

/* A whole number that must be present. */
static gboolean
jsonl_check_required_int(
	JsonObject	 *object,
	const gchar	 *type,
	const gchar	 *member,
	gint64		  minimum,
	guint		  line,
	GError		**error
){
	if (NULL == jsonl_member(object, member))
		return jsonl_refuse(error, line, type, member, "is required");

	return jsonl_check_int(object, type, member, minimum, line, error);
}

/*
 * A decimal ratio between 0 and 1 inclusive: a sale rate. Compared as a
 * double, which is fine -- a ratio is not money, and the grammar check
 * before it already refused anything that is not a plain decimal.
 */
static gboolean
jsonl_check_ratio(
	JsonObject	 *object,
	const gchar	 *type,
	const gchar	 *member,
	guint		  line,
	GError		**error
){
	JsonNode *node;

	if (!jsonl_check_decimal(object, type, member, FALSE, FALSE, line, error))
		return FALSE;

	node = jsonl_member(object, member);

	if ((NULL != node) && (g_ascii_strtod(json_node_get_string(node), NULL) > 1.0))
		return jsonl_refuse(error, line, type, member,
		                    "must be a fraction from \"0\" to \"1\"");

	return TRUE;
}

/* Whether a present decimal is zero: "0", "0.00". */
static gboolean
jsonl_decimal_is_zero(const gchar *text)
{
	return (NULL != text) && (strspn(text, "0.") == strlen(text));
}

/*
 * `instrument` and `quantity` travel together: an instrument with no count
 * is not a holding of anything, and a count of nothing is a number with
 * no meaning.
 */
static gboolean
jsonl_check_instrument_pair(
	JsonObject	 *object,
	const gchar	 *type,
	gboolean	  required,
	guint		  line,
	GError		**error
){
	gboolean has_instrument;
	gboolean has_quantity;

	if (!jsonl_check_string(object, type, "instrument", required,
	                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
	    !jsonl_check_int(object, type, "quantity", 1, line, error))
		return FALSE;

	has_instrument = (NULL != jsonl_member(object, "instrument"));
	has_quantity = (NULL != jsonl_member(object, "quantity"));

	if (required && !has_quantity)
		return jsonl_refuse(error, line, type, "quantity", "is required");

	if (has_instrument != has_quantity)
		return jsonl_refuse(error, line, type, has_instrument ? "quantity" : "instrument",
		                    "is required with the other: instrument and quantity "
		                    "travel together");

	return TRUE;
}

static gboolean
jsonl_check_covers(
	JsonObject	 *object,
	const gchar	 *type,
	guint		  line,
	GError		**error
){
	JsonNode *node;
	JsonArray *covers;
	guint seen;
	guint i;

	node = jsonl_member(object, "covers");

	if (NULL == node)
		return jsonl_refuse(error, line, type, "covers", "is required");

	if (!JSON_NODE_HOLDS_ARRAY(node))
		return jsonl_refuse(error, line, type, "covers",
		                    "must be a list such as [\"holdings\", \"positions\"]");

	covers = json_node_get_array(node);

	if (0 == json_array_get_length(covers))
		return jsonl_refuse(error, line, type, "covers",
		                    "must name at least one of holdings, positions, inbound, "
		                    "balances: a snapshot that covers nothing replaces nothing");

	seen = 0;

	for (i = 0; i < json_array_get_length(covers); i++)
	{
		JsonNode *element;
		guint j;

		element = json_array_get_element(covers, i);

		if (!JSON_NODE_HOLDS_VALUE(element) ||
		    (G_TYPE_STRING != json_node_get_value_type(element)))
			return jsonl_refuse(error, line, type, "covers",
			                    "must be a list of kind names");

		for (j = 0; NULL != jsonl_covers[j]; j++)
		{
			if (0 == strcmp(jsonl_covers[j], json_node_get_string(element)))
				break;
		}

		if (NULL == jsonl_covers[j])
			return jsonl_refuse(error, line, type, "covers",
			                    "must be a subset of holdings, positions, inbound, "
			                    "balances");

		if (0 != (seen & (1u << j)))
			return jsonl_refuse(error, line, type, "covers",
			                    "names a kind twice");

		seen |= (1u << j);
	}

	return TRUE;
}

static gboolean
jsonl_validate_account_kind(
	VentureJsonlMessageKind	  kind,
	const gchar		 *type,
	JsonObject		 *object,
	guint			  line,
	GError			**error
){
	switch (kind)
	{
	case VENTURE_JSONL_MESSAGE_ACCOUNT:
	{
		static const gchar *const members[] = {
			"key", "name", "kind", "group", "venue", "last_seen", "attrs", "login", NULL
		};

		/* `login` names the credential the account is reached through;
		 * an empty one says it is reached through none (a guild bank, a
		 * warband several licences share), which is how a producer
		 * takes a login back off an account. */
		return jsonl_check_members(object, type, members, line, error) &&
		       jsonl_check_string(object, type, "key", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "name", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_required_choice(object, type, "kind", jsonl_account_kinds,
		                                   line, error) &&
		       jsonl_check_string(object, type, "group", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "venue", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_time(object, type, "last_seen", FALSE, line, error) &&
		       jsonl_check_attrs(object, type, line, error) &&
		       jsonl_check_string(object, type, "login", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error);
	}

	case VENTURE_JSONL_MESSAGE_LOGIN:
	{
		static const gchar *const members[] = {
			"key", "name", "kind", "group", "attrs", NULL
		};

		/* Only the key is required: a login is often nothing more than
		 * the folder a game keeps an account's settings in. */
		return jsonl_check_members(object, type, members, line, error) &&
		       jsonl_check_string(object, type, "key", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "name", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_choice(object, type, "kind", jsonl_login_kinds, line, error) &&
		       jsonl_check_string(object, type, "group", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_attrs(object, type, line, error);
	}

	case VENTURE_JSONL_MESSAGE_ACCOUNT_SNAPSHOT:
	{
		static const gchar *const members[] = { "account", "at", "covers", NULL };

		return jsonl_check_members(object, type, members, line, error) &&
		       jsonl_check_string(object, type, "account", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_time(object, type, "at", TRUE, line, error) &&
		       jsonl_check_covers(object, type, line, error);
	}

	case VENTURE_JSONL_MESSAGE_BALANCE:
	{
		static const gchar *const members[] = {
			"account", "currency", "amount", "at", NULL
		};

		if (!jsonl_check_members(object, type, members, line, error) ||
		    !jsonl_check_string(object, type, "account", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error))
			return FALSE;

		if (NULL == jsonl_member(object, "currency"))
			return jsonl_refuse(error, line, type, "currency", "is required");

		return jsonl_check_currency(object, type, line, error) &&
		       jsonl_check_decimal(object, type, "amount", TRUE, FALSE, line, error) &&
		       jsonl_check_time(object, type, "at", FALSE, line, error);
	}

	case VENTURE_JSONL_MESSAGE_HOLDING:
	{
		static const gchar *const members[] = {
			"account", "place", "instrument", "quantity", "at", NULL
		};

		/* Zero is allowed: it says the account no longer holds it. */
		return jsonl_check_members(object, type, members, line, error) &&
		       jsonl_check_string(object, type, "account", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_required_choice(object, type, "place", jsonl_places,
		                                   line, error) &&
		       jsonl_check_string(object, type, "instrument", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_required_int(object, type, "quantity", 0, line, error) &&
		       jsonl_check_time(object, type, "at", FALSE, line, error);
	}

	case VENTURE_JSONL_MESSAGE_POSITION:
	{
		static const gchar *const members[] = {
			"account", "venue", "id", "instrument", "quantity", "price", "bid",
			"expires_at", "posted_at", NULL
		};

		return jsonl_check_members(object, type, members, line, error) &&
		       jsonl_check_string(object, type, "account", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "venue", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "id", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "instrument", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_required_int(object, type, "quantity", 1, line, error) &&
		       jsonl_check_decimal(object, type, "price", TRUE, FALSE, line, error) &&
		       jsonl_check_decimal(object, type, "bid", FALSE, FALSE, line, error) &&
		       jsonl_check_time(object, type, "expires_at", TRUE, line, error) &&
		       jsonl_check_time(object, type, "posted_at", FALSE, line, error);
	}

	case VENTURE_JSONL_MESSAGE_INBOUND:
	{
		static const gchar *const members[] = {
			"account", "id", "sender", "subject", "money", "instrument",
			"quantity", "expires_at", "returned", "cod", NULL
		};

		if (!jsonl_check_members(object, type, members, line, error) ||
		    !jsonl_check_string(object, type, "account", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_string(object, type, "id", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_string(object, type, "sender", FALSE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_string(object, type, "subject", FALSE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_decimal(object, type, "money", FALSE, FALSE, line, error) ||
		    !jsonl_check_instrument_pair(object, type, FALSE, line, error) ||
		    !jsonl_check_time(object, type, "expires_at", FALSE, line, error) ||
		    !jsonl_check_bool(object, type, "returned", line, error) ||
		    !jsonl_check_decimal(object, type, "cod", FALSE, FALSE, line, error))
			return FALSE;

		/* Something to collect, or something owed: one of them. */
		if ((NULL == jsonl_member(object, "money")) &&
		    (NULL == jsonl_member(object, "instrument")) &&
		    (NULL == jsonl_member(object, "cod")))
			return jsonl_refuse(error, line, type, NULL,
			                    "carries nothing to collect: give at least one of "
			                    "money, instrument (with quantity) or cod");

		return TRUE;
	}

	case VENTURE_JSONL_MESSAGE_TXN:
	{
		static const gchar *const members[] = {
			"id", "account", "venue", "kind", "instrument", "quantity",
			"unit_price", "amount", "counterparty", "source", "at", NULL
		};
		const gchar *txn_kind;
		gboolean goods;
		gboolean moneyless;

		if (!jsonl_check_members(object, type, members, line, error) ||
		    !jsonl_check_string(object, type, "id", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_string(object, type, "account", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_string(object, type, "venue", FALSE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_required_choice(object, type, "kind", jsonl_txn_kinds,
		                                 line, error) ||
		    !jsonl_check_decimal(object, type, "unit_price", FALSE, FALSE, line, error) ||
		    !jsonl_check_decimal(object, type, "amount", FALSE, FALSE, line, error) ||
		    !jsonl_check_string(object, type, "counterparty", FALSE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_string(object, type, "source", FALSE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_time(object, type, "at", TRUE, line, error))
			return FALSE;

		txn_kind = jsonl_string(object, "kind");

		/* Goods change hands in a sale, a buy, and an auction that
		 * expired or was cancelled; income and expense may be money
		 * alone -- postage, a repair bill, a transfer. */
		goods = (0 == strcmp(txn_kind, "sale")) || (0 == strcmp(txn_kind, "buy")) ||
		        (0 == strcmp(txn_kind, "expired")) || (0 == strcmp(txn_kind, "cancelled"));
		moneyless = (0 == strcmp(txn_kind, "expired")) ||
		            (0 == strcmp(txn_kind, "cancelled"));

		if (!jsonl_check_instrument_pair(object, type, goods, line, error))
			return FALSE;

		if (moneyless)
		{
			/* Nothing was paid for an auction that did not sell. A
			 * zero is tolerated; a figure would be booked as money
			 * that never moved. */
			if (((NULL != jsonl_member(object, "amount")) &&
			     !jsonl_decimal_is_zero(jsonl_string(object, "amount"))) ||
			    ((NULL != jsonl_member(object, "unit_price")) &&
			     !jsonl_decimal_is_zero(jsonl_string(object, "unit_price"))))
				return jsonl_refuse(error, line, type, NULL,
				                    "of kind expired or cancelled carries no money: "
				                    "leave out amount and unit_price");
		}
		else if (NULL == jsonl_member(object, "amount"))
			return jsonl_refuse(error, line, type, "amount", "is required");

		return TRUE;
	}

	default:
		break;
	}

	return jsonl_refuse(error, line, type, NULL, "is not understood");
}

/* ==========================================================================
 * Per-type rules
 * ========================================================================== */

static const gchar *const jsonl_listing_sides[] = { "sell", "buy", NULL };
static const gchar *const jsonl_quote_sides[] = {
	"back", "lay", "bid", "ask", NULL
};
static const gchar *const jsonl_odds_formats[] = {
	"decimal", "american", "fractional", NULL
};
static const gchar *const jsonl_log_levels[] = {
	"debug", "info", "warning", NULL
};

/*
 * The money members of a stat. At least one figure must be present: a stat
 * that says nothing is a producer that lost its data, and accepting it
 * would file a row of blanks that reads like a quiet market.
 */
static const gchar *const jsonl_stat_prices[] = {
	"min", "market", "mean", "median", "sale_avg", "historical", NULL
};
static const gchar *const jsonl_stat_counts[] = {
	"quantity", "listings", "sold", NULL
};

static gboolean
jsonl_validate(
	VentureJsonlMessageKind	  kind,
	const gchar		 *type,
	JsonObject		 *object,
	guint			  line,
	GError			**error
){
	gsize i;

	switch (kind)
	{
	case VENTURE_JSONL_MESSAGE_VENUE:
		return jsonl_check_string(object, type, "key", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "name", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "kind", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "group", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_currency(object, type, line, error) &&
		       jsonl_check_object(object, type, "attrs", FALSE, line, error);

	case VENTURE_JSONL_MESSAGE_INSTRUMENT:
		return jsonl_check_string(object, type, "key", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "name", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "kind", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "category", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "parent", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_object(object, type, "attrs", FALSE, line, error);

	case VENTURE_JSONL_MESSAGE_SNAPSHOT:
		return jsonl_check_string(object, type, "venue", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_time(object, type, "taken_at", TRUE, line, error) &&
		       jsonl_check_bool(object, type, "complete", line, error);

	case VENTURE_JSONL_MESSAGE_LISTING:
		return jsonl_check_string(object, type, "venue", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "instrument", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_decimal(object, type, "price", TRUE, FALSE,
		                           line, error) &&
		       jsonl_check_int(object, type, "quantity", 1, line, error) &&
		       jsonl_check_currency(object, type, line, error) &&
		       jsonl_check_string(object, type, "id", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_choice(object, type, "side", jsonl_listing_sides,
		                          line, error) &&
		       jsonl_check_int(object, type, "expires_in_min", 0, line,
		                       error) &&
		       jsonl_check_time(object, type, "taken_at", FALSE, line, error);

	case VENTURE_JSONL_MESSAGE_STAT:
	{
		gboolean any;

		if (!jsonl_check_string(object, type, "venue", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_string(object, type, "instrument", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_currency(object, type, line, error) ||
		    !jsonl_check_time(object, type, "taken_at", FALSE, line, error))
			return FALSE;

		any = FALSE;

		for (i = 0; NULL != jsonl_stat_prices[i]; i++)
		{
			if (!jsonl_check_decimal(object, type, jsonl_stat_prices[i],
			                         FALSE, FALSE, line, error))
				return FALSE;

			any = any || (NULL != jsonl_member(object, jsonl_stat_prices[i]));
		}

		for (i = 0; NULL != jsonl_stat_counts[i]; i++)
		{
			if (!jsonl_check_int(object, type, jsonl_stat_counts[i], 0,
			                     line, error))
				return FALSE;

			any = any || (NULL != jsonl_member(object, jsonl_stat_counts[i]));
		}

		/* The source's own sales figures: a rate is a fraction of what
		 * was listed, a speed is units a day. */
		if (!jsonl_check_ratio(object, type, "sale_rate", line, error) ||
		    !jsonl_check_decimal(object, type, "sold_per_day", FALSE, FALSE,
		                         line, error))
			return FALSE;

		any = any || (NULL != jsonl_member(object, "sale_rate")) ||
		      (NULL != jsonl_member(object, "sold_per_day"));

		if (!any)
			return jsonl_refuse(error, line, type, NULL,
			                    "carries no figure: give at least one of min, "
			                    "market, mean, median, sale_avg, historical, "
			                    "quantity, listings, sold, sale_rate or "
			                    "sold_per_day");

		return TRUE;
	}

	case VENTURE_JSONL_MESSAGE_QUOTE:
	{
		JsonNode *odds;
		JsonNode *price;

		if (!jsonl_check_string(object, type, "venue", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_string(object, type, "instrument", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_decimal(object, type, "price", FALSE, FALSE,
		                         line, error) ||
		    !jsonl_check_string(object, type, "odds", FALSE,
		                        VENTURE_JSONL_MAX_DECIMAL_LENGTH, line, error) ||
		    !jsonl_check_choice(object, type, "format", jsonl_odds_formats,
		                        line, error) ||
		    !jsonl_check_choice(object, type, "side", jsonl_quote_sides,
		                        line, error) ||
		    !jsonl_check_decimal(object, type, "liquidity", FALSE, FALSE,
		                         line, error) ||
		    !jsonl_check_currency(object, type, line, error) ||
		    !jsonl_check_time(object, type, "taken_at", FALSE, line, error))
			return FALSE;

		odds = jsonl_member(object, "odds");
		price = jsonl_member(object, "price");

		/* Exactly one: a quote with both is two quotes, and one with
		 * neither quotes nothing. */
		if ((NULL == odds) == (NULL == price))
			return jsonl_refuse(error, line, type, NULL,
			                    "must carry exactly one of price and odds");

		if ((NULL != odds) &&
		    !jsonl_check_odds(json_node_get_string(odds),
		                      jsonl_string(object, "format")))
			return jsonl_refuse(error, line, type, "odds",
			                    "is not a price in its format");

		return TRUE;
	}

	case VENTURE_JSONL_MESSAGE_ENTRY:
		return jsonl_check_string(object, type, "key", TRUE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "title", TRUE,
		                          VENTURE_JSONL_MAX_TEXT_LENGTH, line, error) &&
		       jsonl_check_url(object, type, "url", line, error) &&
		       jsonl_check_string(object, type, "summary", FALSE,
		                          VENTURE_JSONL_MAX_TEXT_LENGTH, line, error) &&
		       jsonl_check_time(object, type, "published_at", FALSE, line,
		                        error) &&
		       jsonl_check_string(object, type, "venue", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "instrument", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error);

	case VENTURE_JSONL_MESSAGE_RECORD:
	{
		JsonNode *match;
		const gchar *record_type;

		if (!jsonl_check_string(object, type, "record_type", TRUE,
		                        VENTURE_JSONL_MAX_KEY_LENGTH, line, error) ||
		    !jsonl_check_object(object, type, "fields", TRUE, line, error))
			return FALSE;

		/* A type name, not a table name or a path: the consumer looks
		 * it up in the entity registry, and a name that could not be
		 * one is refused before it gets there. */
		record_type = json_object_get_string_member(object, "record_type");

		if (!g_ascii_islower(record_type[0]) ||
		    (strspn(record_type, "abcdefghijklmnopqrstuvwxyz0123456789_") !=
		     strlen(record_type)))
			return jsonl_refuse(error, line, type, "record_type",
			                    "must be a record type name such as "
			                    "\"exchange_rate\"");

		match = jsonl_member(object, "match");

		if (NULL != match)
		{
			JsonArray *names;
			guint j;

			if (!JSON_NODE_HOLDS_ARRAY(match))
				return jsonl_refuse(error, line, type, "match",
				                    "must be a list of field names");

			names = json_node_get_array(match);

			for (j = 0; j < json_array_get_length(names); j++)
			{
				JsonNode *name;

				name = json_array_get_element(names, j);

				if (!JSON_NODE_HOLDS_VALUE(name) ||
				    (G_TYPE_STRING != json_node_get_value_type(name)) ||
				    ('\0' == json_node_get_string(name)[0]))
					return jsonl_refuse(error, line, type, "match",
					                    "must be a list of field names");
			}
		}

		return TRUE;
	}

	case VENTURE_JSONL_MESSAGE_CURSOR:
		return jsonl_check_string(object, type, "value", TRUE,
		                          VENTURE_JSONL_MAX_TEXT_LENGTH, line, error);

	case VENTURE_JSONL_MESSAGE_NOT_MODIFIED:
		return jsonl_check_string(object, type, "venue", FALSE,
		                          VENTURE_JSONL_MAX_KEY_LENGTH, line, error);

	case VENTURE_JSONL_MESSAGE_LOG:
		return jsonl_check_string(object, type, "message", TRUE,
		                          VENTURE_JSONL_MAX_TEXT_LENGTH, line, error) &&
		       jsonl_check_choice(object, type, "level", jsonl_log_levels,
		                          line, error);

	case VENTURE_JSONL_MESSAGE_ERROR:
		return jsonl_check_string(object, type, "message", TRUE,
		                          VENTURE_JSONL_MAX_TEXT_LENGTH, line, error) &&
		       jsonl_check_int(object, type, "retry_after", 0, line, error);

	/* Every member optional: "it ran" is an answer too. */
	case VENTURE_JSONL_MESSAGE_RESULT:
		return jsonl_check_int(object, type, "count", 0, line, error) &&
		       jsonl_check_string(object, type, "summary", FALSE,
		                          VENTURE_JSONL_MAX_TEXT_LENGTH, line, error) &&
		       jsonl_check_string(object, type, "detail", FALSE,
		                          VENTURE_JSONL_MAX_TEXT_LENGTH, line, error);

	case VENTURE_JSONL_MESSAGE_ACCOUNT:
	case VENTURE_JSONL_MESSAGE_ACCOUNT_SNAPSHOT:
	case VENTURE_JSONL_MESSAGE_BALANCE:
	case VENTURE_JSONL_MESSAGE_HOLDING:
	case VENTURE_JSONL_MESSAGE_POSITION:
	case VENTURE_JSONL_MESSAGE_INBOUND:
	case VENTURE_JSONL_MESSAGE_TXN:
	case VENTURE_JSONL_MESSAGE_LOGIN:
		return jsonl_validate_account_kind(kind, type, object, line, error);

	default:
		break;
	}

	return jsonl_refuse(error, line, type, NULL, "is not understood");
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

/*
 * The wire names, in enum order. Kept beside the GEnum's nicks rather than
 * read from them so the parser's hot path -- every line of a feed -- does
 * not take the type-class lock; test-plugin-runtime checks the two agree.
 */
static const gchar *const jsonl_kind_names[] = {
	"venue", "instrument", "snapshot", "listing", "stat", "quote",
	"entry", "record", "cursor", "not_modified", "log", "error", "result",
	"account", "account_snapshot", "balance", "holding", "position",
	"inbound", "txn", "login",
	NULL
};

gboolean
venture_jsonl_kind_from_name(
	const gchar		*name,
	VentureJsonlMessageKind	*out_kind
){
	gsize i;

	if (NULL == name)
		return FALSE;

	for (i = 0; NULL != jsonl_kind_names[i]; i++)
	{
		if (0 == strcmp(jsonl_kind_names[i], name))
		{
			if (NULL != out_kind)
				*out_kind = (VentureJsonlMessageKind)i;

			return TRUE;
		}
	}

	return FALSE;
}

gboolean
venture_jsonl_is_decimal(
	const gchar	*text,
	gboolean	 allow_negative
){
	const gchar *cursor;
	gsize digits;

	if ((NULL == text) || ('\0' == text[0]) ||
	    (strlen(text) > VENTURE_JSONL_MAX_DECIMAL_LENGTH))
		return FALSE;

	cursor = text;

	if ('-' == *cursor)
	{
		if (!allow_negative)
			return FALSE;

		cursor++;
	}

	/* The integer part: at least one digit, so ".5" is refused. */
	digits = strspn(cursor, "0123456789");

	if (0 == digits)
		return FALSE;

	cursor += digits;

	if ('\0' == *cursor)
		return TRUE;

	if ('.' != *cursor)
		return FALSE;

	cursor++;

	/* The fraction: at least one digit, so "5." is refused, and nothing
	 * after it -- no exponent, no unit. */
	digits = strspn(cursor, "0123456789");

	return (0 != digits) && ('\0' == cursor[digits]);
}

VentureJsonlMessage *
venture_jsonl_message_new_from_object(
	JsonObject	 *object,
	guint		  line_number,
	GError		**error
){
	VentureJsonlMessage *self;
	VentureJsonlMessageKind kind;
	JsonNode *type_node;
	const gchar *type;

	g_return_val_if_fail(NULL != object, NULL);

	type_node = jsonl_member(object, "type");

	if ((NULL == type_node) || !JSON_NODE_HOLDS_VALUE(type_node) ||
	    (G_TYPE_STRING != json_node_get_value_type(type_node)))
	{
		jsonl_refuse(error, line_number, NULL, "type",
		             "is required and must be a string");
		return NULL;
	}

	type = json_node_get_string(type_node);

	if (!venture_jsonl_kind_from_name(type, &kind))
	{
		/* The type is not echoed: it is producer text, and the error
		 * may be logged. The documentation lists the vocabulary. */
		jsonl_refuse(error, line_number, NULL, "type",
		             "is not a protocol-1 message type (see "
		             "docs/plugins.org)");
		return NULL;
	}

	if (!jsonl_validate(kind, type, object, line_number, error))
		return NULL;

	self = g_new0(VentureJsonlMessage, 1);
	self->ref_count = 1;
	self->kind = kind;
	self->object = json_object_ref(object);
	self->line = line_number;

	return self;
}

VentureJsonlMessage *
venture_jsonl_message_parse(
	const gchar	 *line,
	gssize		  length,
	guint		  line_number,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) local_error = NULL;
	JsonNode *root;

	g_return_val_if_fail(NULL != line, NULL);

	if (length < 0)
		length = (gssize)strlen(line);

	/* JSON-GLib would read up to the first NUL and stop, so a line with
	 * one embedded would be judged on half its bytes. */
	if (NULL != memchr(line, '\0', (gsize)length))
	{
		jsonl_refuse(error, line_number, NULL, NULL,
		             "contains a NUL byte");
		return NULL;
	}

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, line, length, &local_error))
	{
		jsonl_refuse(error, line_number, NULL, NULL, "is not valid JSON");
		return NULL;
	}

	root = json_parser_get_root(parser);

	if ((NULL == root) || !JSON_NODE_HOLDS_OBJECT(root))
	{
		jsonl_refuse(error, line_number, NULL, NULL,
		             "must be a JSON object");
		return NULL;
	}

	return venture_jsonl_message_new_from_object(json_node_get_object(root),
	                                             line_number, error);
}

VentureJsonlMessage *
venture_jsonl_message_ref(VentureJsonlMessage *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	g_atomic_int_inc(&self->ref_count);

	return self;
}

void
venture_jsonl_message_unref(VentureJsonlMessage *self)
{
	if (NULL == self)
		return;

	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;

	json_object_unref(self->object);
	g_free(self);
}

VentureJsonlMessageKind
venture_jsonl_message_get_kind(VentureJsonlMessage *self)
{
	g_return_val_if_fail(NULL != self, VENTURE_JSONL_MESSAGE_LOG);

	return self->kind;
}

const gchar *
venture_jsonl_message_get_kind_name(VentureJsonlMessage *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return jsonl_kind_names[self->kind];
}

JsonObject *
venture_jsonl_message_get_object(VentureJsonlMessage *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->object;
}

guint
venture_jsonl_message_get_line(VentureJsonlMessage *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->line;
}

const gchar *
venture_jsonl_message_get_string(
	VentureJsonlMessage	*self,
	const gchar		*member
){
	g_return_val_if_fail(NULL != self, NULL);
	g_return_val_if_fail(NULL != member, NULL);

	return jsonl_string(self->object, member);
}

gint64
venture_jsonl_message_get_int(
	VentureJsonlMessage	*self,
	const gchar		*member,
	gint64			 fallback
){
	JsonNode *node;

	g_return_val_if_fail(NULL != self, fallback);
	g_return_val_if_fail(NULL != member, fallback);

	node = jsonl_member(self->object, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_INT64 != json_node_get_value_type(node)))
		return fallback;

	return json_node_get_int(node);
}

/* ==========================================================================
 * The incremental reader
 * ========================================================================== */

VentureJsonlReader *
venture_jsonl_reader_new(gsize max_line)
{
	VentureJsonlReader *self;

	self = g_new0(VentureJsonlReader, 1);
	self->pending = g_byte_array_new();
	self->max_line = max_line;

	return self;
}

void
venture_jsonl_reader_free(VentureJsonlReader *self)
{
	if (NULL == self)
		return;

	g_byte_array_unref(self->pending);
	g_free(self);
}

guint
venture_jsonl_reader_get_line(VentureJsonlReader *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->line;
}

/*
 * Reads one complete line out of @data. Blank lines -- whitespace only --
 * are counted and skipped, because a producer that ends with an extra
 * newline has not said anything wrong.
 */
static gboolean
jsonl_reader_take_line(
	VentureJsonlReader	 *self,
	const gchar		 *data,
	gsize			  length,
	GPtrArray		 *out_messages,
	GError			**error
){
	VentureJsonlMessage *message;
	gsize i;

	self->line++;

	if ((length > 0) && ('\r' == data[length - 1]))
		length--;

	for (i = 0; i < length; i++)
	{
		if (!g_ascii_isspace(data[i]))
			break;
	}

	if (i == length)
		return TRUE;

	message = venture_jsonl_message_parse(data, (gssize)length, self->line,
	                                      error);

	if (NULL == message)
		return FALSE;

	g_ptr_array_add(out_messages, message);

	return TRUE;
}

gboolean
venture_jsonl_reader_feed(
	VentureJsonlReader	 *self,
	const guint8		 *data,
	gsize			  length,
	GPtrArray		 *out_messages,
	GError			**error
){
	gsize start;
	gsize i;

	g_return_val_if_fail(NULL != self, FALSE);
	g_return_val_if_fail((NULL != data) || (0 == length), FALSE);
	g_return_val_if_fail(NULL != out_messages, FALSE);

	if (self->failed)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "The reader already refused a line");
		return FALSE;
	}

	start = 0;

	for (i = 0; i < length; i++)
	{
		if ('\n' != data[i])
			continue;

		/* A line that began in an earlier chunk is assembled in the
		 * pending buffer; one wholly inside this chunk is parsed in
		 * place, which is the common case and copies nothing. */
		if (self->pending->len > 0)
		{
			gboolean ok;

			g_byte_array_append(self->pending, data + start, (guint)(i - start));

			if ((0 != self->max_line) && (self->pending->len > self->max_line))
			{
				self->failed = TRUE;
				return jsonl_refuse(error, self->line + 1, NULL, NULL,
				                    "is longer than the limit");
			}

			ok = jsonl_reader_take_line(self, (const gchar *)self->pending->data,
			                            self->pending->len, out_messages,
			                            error);
			g_byte_array_set_size(self->pending, 0);

			if (!ok)
			{
				self->failed = TRUE;
				return FALSE;
			}
		}
		else
		{
			if ((0 != self->max_line) && ((i - start) > self->max_line))
			{
				self->failed = TRUE;
				return jsonl_refuse(error, self->line + 1, NULL, NULL,
				                    "is longer than the limit");
			}

			if (!jsonl_reader_take_line(self, (const gchar *)data + start,
			                            i - start, out_messages, error))
			{
				self->failed = TRUE;
				return FALSE;
			}
		}

		start = i + 1;
	}

	if (start < length)
	{
		g_byte_array_append(self->pending, data + start,
		                    (guint)(length - start));

		/* Refused as soon as it is too long, not when its newline
		 * finally arrives: a producer that never ends a line would
		 * otherwise grow this buffer for as long as it is allowed to
		 * write. */
		if ((0 != self->max_line) && (self->pending->len > self->max_line))
		{
			self->failed = TRUE;
			return jsonl_refuse(error, self->line + 1, NULL, NULL,
			                    "is longer than the limit");
		}
	}

	return TRUE;
}

gboolean
venture_jsonl_reader_finish(
	VentureJsonlReader	 *self,
	GPtrArray		 *out_messages,
	GError			**error
){
	gboolean ok;

	g_return_val_if_fail(NULL != self, FALSE);
	g_return_val_if_fail(NULL != out_messages, FALSE);

	if (self->failed)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "The reader already refused a line");
		return FALSE;
	}

	if (0 == self->pending->len)
		return TRUE;

	ok = jsonl_reader_take_line(self, (const gchar *)self->pending->data,
	                            self->pending->len, out_messages, error);
	g_byte_array_set_size(self->pending, 0);

	if (!ok)
		self->failed = TRUE;

	return ok;
}
