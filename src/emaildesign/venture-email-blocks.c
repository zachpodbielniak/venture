/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The block model of a designed email: a strict validator and a renderer
 * to email-safe HTML and a plain-text alternative. Neither touches the
 * database; sections and hosted images arrive through a resolver.
 *
 * The HTML is written for the mail apps people actually use, which means
 * the 2005 web: table layout, every style inline (a <style> block carries
 * only what cannot be inline -- the narrow-screen and dark-mode rules), a
 * fixed 600-pixel body, no script, no forms, no linked style sheets, and
 * Outlook's conditional comments where they are cheap.
 */
#include "venture-email-design-private.h"
#include <math.h>
#include <string.h>

#define PAD 32
#define INNER (VENTURE_EMAIL_DESIGN_WIDTH - 2 * PAD)
#define MAX_COLUMN_BLOCKS 6
#define MAX_SOCIAL_LINKS 8
#define MAX_URL 2000

/* --- Fonts and colours ------------------------------------------------------- */

static const struct { const gchar *name; const gchar *stack; } fonts[] = {
	{ "sans", "'Helvetica Neue', Helvetica, Arial, sans-serif" },
	{ "serif", "Georgia, 'Times New Roman', Times, serif" },
	{ "rounded", "Verdana, Geneva, Tahoma, sans-serif" },
	{ "system", "-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif" },
	{ "mono", "'Courier New', Courier, monospace" }
};
const gchar *venture_email_font_stack(const gchar *font)
{
	guint i;
	for (i = 0; font != NULL && i < G_N_ELEMENTS(fonts); i++)
		if (g_str_equal(fonts[i].name, font))
			return fonts[i].stack;
	return NULL;
}

typedef struct { gdouble r, g, b; } Rgb;
static gboolean parse_colour(const gchar *hex, Rgb *out)
{
	guint i, value = 0;
	if (hex == NULL || strlen(hex) != 7 || hex[0] != '#')
		return FALSE;
	for (i = 1; i < 7; i++) {
		if (!g_ascii_isxdigit(hex[i])) return FALSE;
		value = value * 16 + (guint)g_ascii_xdigit_value(hex[i]);
	}
	out->r = ((value >> 16) & 0xff) / 255.0;
	out->g = ((value >> 8) & 0xff) / 255.0;
	out->b = (value & 0xff) / 255.0;
	return TRUE;
}
static gchar *colour_text(Rgb c)
{
	return g_strdup_printf("#%02x%02x%02x", (guint)(c.r * 255.0 + 0.5), (guint)(c.g * 255.0 + 0.5), (guint)(c.b * 255.0 + 0.5));
}
/* @amount of @b mixed into @a. */
static gchar *mix(const gchar *a, const gchar *b, gdouble amount)
{
	Rgb x = { 0, 0, 0 }, y = { 1, 1, 1 }, out;
	parse_colour(a, &x);
	parse_colour(b, &y);
	out.r = x.r + (y.r - x.r) * amount;
	out.g = x.g + (y.g - x.g) * amount;
	out.b = x.b + (y.b - x.b) * amount;
	return colour_text(out);
}
static gdouble channel(gdouble c) { return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static gdouble luminance(const gchar *hex)
{
	Rgb c = { 0, 0, 0 };
	parse_colour(hex, &c);
	return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
}
static gdouble contrast(const gchar *a, const gchar *b)
{
	gdouble x = luminance(a), y = luminance(b);
	return (MAX(x, y) + 0.05) / (MIN(x, y) + 0.05);
}
/* White or near-black, whichever reads on @background. */
static const gchar *ink_on(const gchar *background)
{
	return contrast(background, "#ffffff") >= 3.0 ? "#ffffff" : "#111111";
}
/* A dark-mode primary: the light one, lifted until it reads on @card. */
static gchar *lift(const gchar *primary, const gchar *card)
{
	gdouble amount;
	for (amount = 0.0; amount < 0.9; amount += 0.1) {
		g_autofree gchar *candidate = mix(primary, "#ffffff", amount);
		if (contrast(candidate, card) >= 4.0)
			return g_steal_pointer(&candidate);
	}
	return mix(primary, "#ffffff", 0.9);
}

VentureEmailLook *venture_email_look_new_default(const gchar *name)
{
	VentureEmailLook *look = g_new0(VentureEmailLook, 1);
	look->name = g_strdup(!venture_string_is_empty(name) ? name : "");
	look->logo_width = 0;
	look->primary = g_strdup("#1f5fbf");
	look->accent = g_strdup("#0f766e");
	look->text = g_strdup("#1f2933");
	look->background = g_strdup("#f3f4f6");
	look->card = g_strdup("#ffffff");
	look->dark_background = g_strdup("#111418");
	look->dark_card = g_strdup("#1b1f24");
	look->dark_text = g_strdup("#e5e7eb");
	look->dark_primary = lift(look->primary, look->dark_card);
	look->font = g_strdup(venture_email_font_stack("sans"));
	return look;
}
static void lower(gchar *colour)
{
	gchar *p;
	for (p = colour; p != NULL && *p; p++)
		*p = g_ascii_tolower(*p);
}
void venture_email_look_finish(VentureEmailLook *look)
{
	lower(look->primary); lower(look->accent); lower(look->text); lower(look->background); lower(look->card);
	lower(look->dark_primary); lower(look->dark_text); lower(look->dark_background); lower(look->dark_card);
	if (look->dark_background == NULL)
		look->dark_background = g_strdup("#111418");
	/* The message sits on a card a shade lighter than its surround. */
	if (look->dark_card == NULL)
		look->dark_card = mix(look->dark_background, "#ffffff", 0.07);
	if (look->dark_text == NULL)
		look->dark_text = g_strdup("#e5e7eb");
	if (look->dark_primary == NULL)
		look->dark_primary = lift(look->primary, look->dark_card);
}
void venture_email_look_free(VentureEmailLook *look)
{
	if (look == NULL) return;
	g_free(look->name); g_free(look->logo_url); g_free(look->primary); g_free(look->accent);
	g_free(look->text); g_free(look->background); g_free(look->card); g_free(look->dark_primary);
	g_free(look->dark_text); g_free(look->dark_background); g_free(look->dark_card); g_free(look->font);
	g_free(look->website); g_free(look->address); g_free(look);
}

/* --- Validation ---------------------------------------------------------------- */

typedef enum { K_STRING, K_TEXT, K_URL, K_INT, K_ID, K_BOOL, K_CHOICE, K_COLUMNS, K_LINKS } Kind;
typedef struct {
	const gchar *name;
	Kind kind;
	gboolean required;
	gint64 min, max;
	const gchar *const *choices;
} Rule;
static const gchar *const aligns2[] = { "left", "center", NULL };
static const gchar *const aligns3[] = { "left", "center", "right", NULL };
static const gchar *const sizes[] = { "small", "medium", "large", NULL };
static const gchar *const colours[] = { "primary", "accent", NULL };
static const gchar *const networks[] = { "facebook", "instagram", "x", "linkedin", "youtube", "tiktok", "pinterest", "website", NULL };
static const gchar *const network_labels[] = { "Facebook", "Instagram", "X", "LinkedIn", "YouTube", "TikTok", "Pinterest", "Website", NULL };

static const Rule header_rules[] = {
	{ "show_logo", K_BOOL, FALSE, 0, 0, NULL }, { "tagline", K_STRING, FALSE, 0, 200, NULL },
	{ "align", K_CHOICE, FALSE, 0, 0, aligns2 }, { NULL, K_BOOL, FALSE, 0, 0, NULL } };
static const Rule text_rules[] = {
	{ "heading", K_STRING, FALSE, 0, 200, NULL }, { "body", K_TEXT, TRUE, 1, 5000, NULL },
	{ "align", K_CHOICE, FALSE, 0, 0, aligns3 }, { NULL, K_BOOL, FALSE, 0, 0, NULL } };
static const Rule image_rules[] = {
	{ "src", K_URL, FALSE, 0, 0, NULL }, { "image_id", K_ID, FALSE, 0, 0, NULL },
	{ "alt", K_STRING, TRUE, 1, 300, NULL }, { "width", K_INT, FALSE, 16, VENTURE_EMAIL_DESIGN_WIDTH, NULL },
	{ "link", K_URL, FALSE, 0, 0, NULL }, { NULL, K_BOOL, FALSE, 0, 0, NULL } };
static const Rule button_rules[] = {
	{ "label", K_STRING, TRUE, 1, 60, NULL }, { "url", K_URL, TRUE, 0, 0, NULL },
	{ "size", K_CHOICE, FALSE, 0, 0, sizes }, { "align", K_CHOICE, FALSE, 0, 0, aligns3 },
	{ "color", K_CHOICE, FALSE, 0, 0, colours }, { NULL, K_BOOL, FALSE, 0, 0, NULL } };
static const Rule columns_rules[] = {
	{ "columns", K_COLUMNS, TRUE, 2, 3, NULL }, { NULL, K_BOOL, FALSE, 0, 0, NULL } };
static const Rule divider_rules[] = { { NULL, K_BOOL, FALSE, 0, 0, NULL } };
static const Rule social_rules[] = {
	{ "links", K_LINKS, TRUE, 1, MAX_SOCIAL_LINKS, NULL }, { NULL, K_BOOL, FALSE, 0, 0, NULL } };
static const Rule footer_rules[] = {
	{ "text", K_TEXT, FALSE, 0, 1000, NULL }, { "address", K_TEXT, FALSE, 0, 500, NULL },
	{ NULL, K_BOOL, FALSE, 0, 0, NULL } };
static const Rule video_rules[] = {
	{ "url", K_URL, TRUE, 0, 0, NULL }, { "poster_src", K_URL, FALSE, 0, 0, NULL },
	{ "poster_image_id", K_ID, FALSE, 0, 0, NULL }, { "alt", K_STRING, TRUE, 1, 300, NULL },
	{ "caption", K_STRING, FALSE, 0, 200, NULL }, { NULL, K_BOOL, FALSE, 0, 0, NULL } };
static const Rule section_rules[] = {
	{ "section_id", K_ID, TRUE, 0, 0, NULL }, { NULL, K_BOOL, FALSE, 0, 0, NULL } };

static const struct { const gchar *type; const Rule *rules; gboolean in_columns; } block_types[] = {
	{ "header", header_rules, FALSE },
	{ "text", text_rules, TRUE },
	{ "image", image_rules, TRUE },
	{ "button", button_rules, TRUE },
	{ "columns", columns_rules, FALSE },
	{ "divider", divider_rules, TRUE },
	{ "social", social_rules, FALSE },
	{ "footer", footer_rules, FALSE },
	{ "video", video_rules, FALSE },
	{ "section", section_rules, FALSE }
};

static gboolean refuse(GError **error, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static gboolean refuse(GError **error, const gchar *format, ...)
{
	va_list args;
	g_autofree gchar *message = NULL;
	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, message);
	return FALSE;
}
static gboolean in_list(const gchar *value, const gchar *const *list)
{
	guint i;
	for (i = 0; list[i]; i++)
		if (g_str_equal(list[i], value))
			return TRUE;
	return FALSE;
}
static const gchar *string_member(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}
static gint64 int_member(JsonObject *object, const gchar *name, gint64 fallback)
{
	JsonNode *node = json_object_get_member(object, name);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return fallback;
	return json_node_get_int(node);
}
static gboolean bool_member(JsonObject *object, const gchar *name, gboolean fallback)
{
	JsonNode *node = json_object_get_member(object, name);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
		return fallback;
	return json_node_get_boolean(node);
}

/* Text keeps the template syntax: "{field}" is a placeholder filled when
 * the email is sent and "{{" a literal brace. Anything else with a brace
 * would fail at send time, so it fails here. */
static gboolean check_braces(const gchar *text, const gchar *where, GError **error)
{
	const gchar *p = text;
	while ((p = strchr(p, '{')) != NULL) {
		const gchar *q;
		if (p[1] == '{') { p += 2; continue; }
		q = p + 1;
		if (!g_ascii_islower(*q))
			return refuse(error, "%s: write a literal brace as {{, or a placeholder as {field}", where);
		while (g_ascii_islower(*q) || g_ascii_isdigit(*q) || *q == '_') q++;
		if (*q != '}' || q - p > 64)
			return refuse(error, "%s: write a literal brace as {{, or a placeholder as {field}", where);
		p = q + 1;
	}
	return TRUE;
}
static gboolean check_text(const gchar *value, const Rule *rule, const gchar *where, GError **error)
{
	const gchar *p;
	glong length = g_utf8_strlen(value, -1);
	g_autofree gchar *trimmed = g_strstrip(g_strdup(value));
	if (rule->required && trimmed[0] == '\0')
		return refuse(error, "%s: \"%s\" must not be empty", where, rule->name);
	if (length > rule->max)
		return refuse(error, "%s: \"%s\" is longer than %" G_GINT64_FORMAT " characters", where, rule->name, rule->max);
	for (p = value; *p; p++)
		if ((guchar)*p < 0x20 && !(rule->kind == K_TEXT && (*p == '\n' || *p == '\r')))
			return refuse(error, "%s: \"%s\" holds a control character", where, rule->name);
	return check_braces(value, where, error);
}
/* Only a web address: no javascript:, no data:, no relative path, and
 * nothing that could break out of an attribute or read as a placeholder. */
static gboolean check_url(const gchar *value, const gchar *where, const gchar *name, GError **error)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *scheme, *p;
	if (strlen(value) > MAX_URL)
		return refuse(error, "%s: \"%s\" is too long", where, name);
	for (p = value; *p; p++)
		if ((guchar)*p <= 0x20 || *p == '"' || *p == '\'' || *p == '<' || *p == '>' || *p == '{' || *p == '}' || *p == '\\')
			return refuse(error, "%s: \"%s\" is not a usable web address", where, name);
	uri = g_uri_parse(value, G_URI_FLAGS_NONE, NULL);
	scheme = uri != NULL ? g_uri_get_scheme(uri) : NULL;
	if (uri == NULL || scheme == NULL || (g_ascii_strcasecmp(scheme, "http") != 0 && g_ascii_strcasecmp(scheme, "https") != 0) ||
	    venture_string_is_empty(g_uri_get_host(uri)))
		return refuse(error, "%s: \"%s\" must be an http or https address", where, name);
	return TRUE;
}
static gboolean check_blocks(JsonNode *node, gboolean nested, guint limit, guint *total, const gchar *where, GError **error);

static gboolean check_block(JsonNode *node, gboolean nested, guint *total, guint index, const gchar *outer, GError **error)
{
	JsonObject *block;
	const gchar *type;
	const Rule *rules = NULL;
	g_autoptr(GList) members = NULL;
	g_autofree gchar *where = NULL;
	gboolean in_columns = FALSE;
	GList *l;
	guint i;
	if (!JSON_NODE_HOLDS_OBJECT(node))
		return refuse(error, "%s block %u is not an object", outer, index + 1);
	block = json_node_get_object(node);
	type = string_member(block, "type");
	if (type == NULL)
		return refuse(error, "%s block %u has no type", outer, index + 1);
	for (i = 0; i < G_N_ELEMENTS(block_types); i++)
		if (g_str_equal(block_types[i].type, type)) {
			rules = block_types[i].rules;
			in_columns = block_types[i].in_columns;
		}
	if (rules == NULL)
		return refuse(error, "%s block %u: there is no block type \"%s\"", outer, index + 1, type);
	where = g_strdup_printf("%s block %u (%s)", outer, index + 1, type);
	if (nested && !in_columns)
		return refuse(error, "%s cannot go inside columns", where);
	if (++(*total) > VENTURE_EMAIL_DESIGN_MAX_BLOCKS * 4)
		return refuse(error, "The design has too many blocks");
	members = json_object_get_members(block);
	for (l = members; l != NULL; l = l->next) {
		const gchar *name = l->data;
		gboolean known = g_str_equal(name, "type");
		for (i = 0; rules[i].name != NULL && !known; i++)
			known = g_str_equal(rules[i].name, name);
		if (!known)
			return refuse(error, "%s has no field \"%s\"", where, name);
	}
	for (i = 0; rules[i].name != NULL; i++) {
		const Rule *rule = &rules[i];
		JsonNode *value = json_object_get_member(block, rule->name);
		if (value == NULL || JSON_NODE_HOLDS_NULL(value)) {
			if (rule->required)
				return refuse(error, "%s needs \"%s\"", where, rule->name);
			continue;
		}
		switch (rule->kind) {
		case K_STRING:
		case K_TEXT:
		case K_URL:
		case K_CHOICE:
			if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING)
				return refuse(error, "%s: \"%s\" must be text", where, rule->name);
			if (rule->kind == K_URL && !check_url(json_node_get_string(value), where, rule->name, error))
				return FALSE;
			if (rule->kind == K_CHOICE && !in_list(json_node_get_string(value), rule->choices))
				return refuse(error, "%s: \"%s\" cannot be \"%s\"", where, rule->name, json_node_get_string(value));
			if ((rule->kind == K_STRING || rule->kind == K_TEXT) && !check_text(json_node_get_string(value), rule, where, error))
				return FALSE;
			break;
		case K_INT:
		case K_ID:
			if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_INT64)
				return refuse(error, "%s: \"%s\" must be a whole number", where, rule->name);
			if (rule->kind == K_ID && json_node_get_int(value) <= 0)
				return refuse(error, "%s: \"%s\" must name a saved record", where, rule->name);
			if (rule->kind == K_INT && (json_node_get_int(value) < rule->min || json_node_get_int(value) > rule->max))
				return refuse(error, "%s: \"%s\" must be from %" G_GINT64_FORMAT " to %" G_GINT64_FORMAT,
					where, rule->name, rule->min, rule->max);
			break;
		case K_BOOL:
			if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_BOOLEAN)
				return refuse(error, "%s: \"%s\" must be true or false", where, rule->name);
			break;
		case K_COLUMNS:
		{
			JsonArray *columns;
			guint c;
			if (!JSON_NODE_HOLDS_ARRAY(value))
				return refuse(error, "%s: \"columns\" must be a list", where);
			columns = json_node_get_array(value);
			if (json_array_get_length(columns) < (guint)rule->min || json_array_get_length(columns) > (guint)rule->max)
				return refuse(error, "%s: columns come in twos or threes", where);
			for (c = 0; c < json_array_get_length(columns); c++) {
				JsonNode *column = json_array_get_element(columns, c);
				g_autofree gchar *inner = g_strdup_printf("%s column %u", where, c + 1);
				if (!JSON_NODE_HOLDS_OBJECT(column) || json_object_get_size(json_node_get_object(column)) != 1 ||
				    !json_object_has_member(json_node_get_object(column), "blocks"))
					return refuse(error, "%s must be an object with only \"blocks\"", inner);
				if (!check_blocks(json_object_get_member(json_node_get_object(column), "blocks"), TRUE, MAX_COLUMN_BLOCKS, total, inner, error))
					return FALSE;
			}
			break;
		}
		case K_LINKS:
		{
			JsonArray *links;
			guint k;
			if (!JSON_NODE_HOLDS_ARRAY(value))
				return refuse(error, "%s: \"links\" must be a list", where);
			links = json_node_get_array(value);
			if (json_array_get_length(links) < (guint)rule->min || json_array_get_length(links) > (guint)rule->max)
				return refuse(error, "%s: give from 1 to %d links", where, MAX_SOCIAL_LINKS);
			for (k = 0; k < json_array_get_length(links); k++) {
				JsonNode *link = json_array_get_element(links, k);
				JsonObject *object;
				const gchar *network, *url;
				if (!JSON_NODE_HOLDS_OBJECT(link))
					return refuse(error, "%s: link %u must be an object", where, k + 1);
				object = json_node_get_object(link);
				network = string_member(object, "network");
				url = string_member(object, "url");
				if (json_object_get_size(object) != 2 || network == NULL || url == NULL)
					return refuse(error, "%s: link %u needs only \"network\" and \"url\"", where, k + 1);
				if (!in_list(network, networks))
					return refuse(error, "%s: there is no network \"%s\"", where, network);
				if (!check_url(url, where, "url", error))
					return FALSE;
			}
			break;
		}
		}
	}
	if (g_str_equal(type, "image") && (json_object_has_member(block, "src") == json_object_has_member(block, "image_id")))
		return refuse(error, "%s needs exactly one of \"src\" and \"image_id\"", where);
	if (g_str_equal(type, "video") && (json_object_has_member(block, "poster_src") == json_object_has_member(block, "poster_image_id")))
		return refuse(error, "%s needs exactly one of \"poster_src\" and \"poster_image_id\"", where);
	return TRUE;
}
static gboolean check_blocks(JsonNode *node, gboolean nested, guint limit, guint *total, const gchar *where, GError **error)
{
	JsonArray *blocks;
	guint i;
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
		return refuse(error, "%s: \"blocks\" must be a list", where);
	blocks = json_node_get_array(node);
	if (json_array_get_length(blocks) > limit)
		return refuse(error, "%s: at most %u blocks", where, limit);
	for (i = 0; i < json_array_get_length(blocks); i++)
		if (!check_block(json_array_get_element(blocks, i), nested, total, i, where, error))
			return FALSE;
	return TRUE;
}
static JsonNode *parse(const gchar *json, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GList) members = NULL;
	JsonObject *object;
	GList *l;
	guint total = 0;
	if (json == NULL || json[0] == '\0') {
		refuse(error, "The design is empty");
		return NULL;
	}
	if (strlen(json) > VENTURE_EMAIL_DESIGN_MAX_BYTES) {
		refuse(error, "The design is larger than %d KB", VENTURE_EMAIL_DESIGN_MAX_BYTES / 1024);
		return NULL;
	}
	node = json_from_string(json, NULL);
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) {
		refuse(error, "The design must be a JSON object");
		return NULL;
	}
	object = json_node_get_object(node);
	members = json_object_get_members(object);
	for (l = members; l != NULL; l = l->next)
		if (!g_str_equal(l->data, "blocks") && !g_str_equal(l->data, "preheader") && !g_str_equal(l->data, "version")) {
			refuse(error, "A design has no field \"%s\"", (const gchar *)l->data);
			return NULL;
		}
	if (json_object_has_member(object, "version") && int_member(object, "version", 0) != 1) {
		refuse(error, "This is design version 1");
		return NULL;
	}
	if (json_object_has_member(object, "preheader")) {
		static const Rule preheader = { "preheader", K_STRING, FALSE, 0, 200, NULL };
		const gchar *text = string_member(object, "preheader");
		if (text == NULL) {
			refuse(error, "The preheader must be text");
			return NULL;
		}
		if (!check_text(text, &preheader, "The preheader", error))
			return NULL;
	}
	if (!check_blocks(json_object_get_member(object, "blocks"), FALSE, VENTURE_EMAIL_DESIGN_MAX_BLOCKS, &total, "The design", error))
		return NULL;
	return g_steal_pointer(&node);
}
gboolean venture_email_design_validate(const gchar *json, GError **error)
{
	g_autoptr(JsonNode) node = parse(json, error);
	return node != NULL;
}

/* --- Rendering ------------------------------------------------------------------ */

typedef struct {
	const VentureEmailLook *look;
	const VentureEmailResolver *resolver;
	gboolean template_syntax;
	GString *html;
	GString *text;
	GHashTable *visiting;
	guint depth;
	guint top;
	/* The look with every colour decided. */
	gchar *muted, *divider, *dark_muted, *dark_divider, *dark_primary, *button_ink, *dark_button_ink;
} Render;

/* A person's words: HTML-escaped, and in plain output the template's
 * doubled braces become single ones. */
static gchar *words(Render *r, const gchar *value)
{
	g_autofree gchar *escaped = g_markup_escape_text(value != NULL ? value : "", -1);
	if (r->template_syntax)
		return g_steal_pointer(&escaped);
	{
		g_auto(GStrv) parts = g_strsplit(escaped, "{{", -1);
		return g_strjoinv("{", parts);
	}
}
static gchar *plain(Render *r, const gchar *value)
{
	if (value == NULL) return g_strdup("");
	if (r->template_syntax) return g_strdup(value);
	{
		g_auto(GStrv) parts = g_strsplit(value, "{{", -1);
		return g_strjoinv("{", parts);
	}
}
static void text_paragraph(Render *r, const gchar *value)
{
	g_autofree gchar *line = plain(r, value);
	if (venture_string_is_empty(line)) return;
	if (r->text->len > 0) g_string_append(r->text, "\n\n");
	g_string_append(r->text, line);
}
static void text_link(Render *r, const gchar *label, const gchar *url)
{
	g_autofree gchar *line = plain(r, label);
	if (r->text->len > 0) g_string_append(r->text, "\n\n");
	g_string_append_printf(r->text, "%s: %s", line, url);
}
static const gchar *font(Render *r) { return r->look->font != NULL ? r->look->font : venture_email_font_stack("sans"); }

/* A body paragraph set: blank lines part paragraphs, a single newline
 * breaks a line. */
static void paragraphs(Render *r, GString *out, const gchar *body, const gchar *align, gint size, gint line)
{
	g_autofree gchar *normal = NULL;
	g_auto(GStrv) parts = NULL;
	guint i;
	{
		g_auto(GStrv) crlf = g_strsplit(body, "\r\n", -1);
		normal = g_strjoinv("\n", crlf);
	}
	parts = g_strsplit(normal, "\n\n", -1);
	for (i = 0; parts[i]; i++) {
		g_autofree gchar *escaped = NULL, *trimmed = g_strstrip(g_strdup(parts[i]));
		g_auto(GStrv) lines = NULL;
		if (trimmed[0] == '\0') continue;
		escaped = words(r, trimmed);
		lines = g_strsplit(escaped, "\n", -1);
		g_free(escaped);
		escaped = g_strjoinv("<br>", lines);
		g_string_append_printf(out, "<p class=\"ve-text\" style=\"margin:0 0 14px;font-family:%s;font-size:%dpx;line-height:%dpx;color:%s;text-align:%s\">%s</p>",
			font(r), size, line, r->look->text, align, escaped);
		text_paragraph(r, trimmed);
	}
}
static gboolean image_source(Render *r, JsonObject *block, const gchar *src_name, const gchar *id_name,
	gchar **url, gint *width, gint *height, GError **error)
{
	const gchar *src = string_member(block, src_name);
	*width = 0; *height = 0;
	if (src != NULL) {
		*url = g_strdup(src);
		return TRUE;
	}
	if (r->resolver == NULL || r->resolver->image == NULL)
		return refuse(error, "The design names hosted image %" G_GINT64_FORMAT ", which is not available here", int_member(block, id_name, 0));
	return r->resolver->image(int_member(block, id_name, 0), r->resolver->data, url, width, height, error);
}
static gboolean render_image(Render *r, GString *out, JsonObject *block, gint available, GError **error)
{
	g_autofree gchar *url = NULL, *alt = NULL, *src = NULL, *link = NULL;
	gint natural_w, natural_h, width, height = 0;
	if (!image_source(r, block, "src", "image_id", &url, &natural_w, &natural_h, error))
		return FALSE;
	width = (gint)int_member(block, "width", natural_w > 0 ? MIN(natural_w, available) : available);
	width = MIN(width, available);
	if (natural_w > 0 && natural_h > 0)
		height = (gint)((gint64)natural_h * width / natural_w);
	alt = g_markup_escape_text(string_member(block, "alt"), -1);
	src = g_markup_escape_text(url, -1);
	if (string_member(block, "link") != NULL) {
		link = g_markup_escape_text(string_member(block, "link"), -1);
		g_string_append_printf(out, "<a href=\"%s\" target=\"_blank\" style=\"text-decoration:none\">", link);
	}
	g_string_append_printf(out, "<img src=\"%s\" alt=\"%s\" width=\"%d\"", src, alt, width);
	if (height > 0)
		g_string_append_printf(out, " height=\"%d\"", height);
	g_string_append_printf(out, " style=\"display:block;width:100%%;max-width:%dpx;height:auto;margin:0 auto;border:0;outline:none;text-decoration:none;border-radius:6px\">", width);
	if (link != NULL) {
		g_string_append(out, "</a>");
		text_link(r, string_member(block, "alt"), string_member(block, "link"));
	}
	return TRUE;
}
static void render_button(Render *r, GString *out, JsonObject *block)
{
	const gchar *size = string_member(block, "size"), *align = string_member(block, "align");
	gboolean accent = g_strcmp0(string_member(block, "color"), "accent") == 0;
	const gchar *fill = accent ? r->look->accent : r->look->primary;
	const gchar *ink = accent ? ink_on(r->look->accent) : r->button_ink;
	g_autofree gchar *label = words(r, string_member(block, "label")), *url = g_markup_escape_text(string_member(block, "url"), -1);
	gint vertical = 14, horizontal = 28, points = 16;
	if (g_strcmp0(size, "small") == 0) { vertical = 10; horizontal = 18; points = 14; }
	if (g_strcmp0(size, "large") == 0) { vertical = 18; horizontal = 40; points = 19; }
	if (align == NULL) align = "center";
	/* Bulletproof: the link is the padded, coloured box, so it works
	 * with images off and in every client; the <i> spacers give Outlook,
	 * which ignores padding on a link, the same size. */
	g_string_append_printf(out,
		"<table role=\"presentation\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" align=\"%s\" style=\"margin:%s\"><tr>"
		"<td class=\"ve-btn\" align=\"center\" bgcolor=\"%s\" style=\"border-radius:6px;background:%s\">"
		"<a class=\"ve-btn-a\" href=\"%s\" target=\"_blank\" style=\"display:inline-block;padding:%dpx %dpx;font-family:%s;font-size:%dpx;line-height:%dpx;"
		"font-weight:bold;color:%s;text-decoration:none;border-radius:6px;background:%s;mso-padding-alt:0\">"
		"<!--[if mso]><i style=\"mso-font-width:%d%%;mso-text-raise:%dpt\" hidden>&#8195;</i><![endif]-->"
		"<span style=\"mso-text-raise:%dpt\">%s</span>"
		"<!--[if mso]><i style=\"mso-font-width:%d%%\" hidden>&#8195;&#8203;</i><![endif]--></a></td></tr></table>",
		align, g_str_equal(align, "center") ? "0 auto" : "0", fill, fill, url, vertical, horizontal, font(r), points, points + 4,
		ink, fill, horizontal * 10, vertical * 2 + 6, vertical, label, horizontal * 10);
	text_link(r, string_member(block, "label"), string_member(block, "url"));
}
static void render_divider(Render *r, GString *out)
{
	g_string_append_printf(out, "<table role=\"presentation\" width=\"100%%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\"><tr>"
		"<td class=\"ve-divider\" style=\"border-top:1px solid %s;font-size:1px;line-height:1px;height:1px\">&nbsp;</td></tr></table>", r->divider);
	if (r->text->len > 0) g_string_append(r->text, "\n\n");
	g_string_append(r->text, "----------");
}
static void render_text(Render *r, GString *out, JsonObject *block, gboolean nested)
{
	const gchar *heading = string_member(block, "heading"), *align = string_member(block, "align");
	if (align == NULL) align = "left";
	if (heading != NULL && heading[0] != '\0') {
		g_autofree gchar *escaped = words(r, heading);
		g_string_append_printf(out, "<h%d class=\"ve-text\" style=\"margin:0 0 12px;font-family:%s;font-size:%dpx;line-height:%dpx;font-weight:bold;color:%s;text-align:%s\">%s</h%d>",
			nested ? 3 : 2, font(r), nested ? 18 : 24, nested ? 24 : 31, r->look->text, align, escaped, nested ? 3 : 2);
		text_paragraph(r, heading);
	}
	paragraphs(r, out, string_member(block, "body"), align, nested ? 15 : 16, nested ? 23 : 26);
}
static void render_header(Render *r, GString *out, JsonObject *block)
{
	const gchar *align = string_member(block, "align");
	const gchar *tagline = string_member(block, "tagline");
	g_autofree gchar *name = g_markup_escape_text(r->look->name != NULL ? r->look->name : "", -1);
	if (align == NULL) align = "center";
	if (r->look->logo_url != NULL && bool_member(block, "show_logo", TRUE)) {
		g_autofree gchar *src = g_markup_escape_text(r->look->logo_url, -1);
		gint width = r->look->logo_width > 0 ? MIN(r->look->logo_width, 240) : 180, height = 0;
		if (r->look->logo_width > 0 && r->look->logo_height > 0)
			height = (gint)((gint64)r->look->logo_height * width / r->look->logo_width);
		/* A logo is usually dark ink on nothing; in dark mode it sits on a
		 * light chip rather than vanishing into the dark card. */
		g_string_append_printf(out, "<table role=\"presentation\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" align=\"%s\" style=\"margin:%s\"><tr><td class=\"ve-logo\" style=\"border-radius:8px\">",
			align, g_str_equal(align, "center") ? "0 auto" : "0");
		if (r->look->website != NULL) {
			g_autofree gchar *site = g_markup_escape_text(r->look->website, -1);
			g_string_append_printf(out, "<a href=\"%s\" target=\"_blank\" style=\"text-decoration:none\">", site);
		}
		g_string_append_printf(out, "<img src=\"%s\" alt=\"%s\" width=\"%d\"", src, name, width);
		if (height > 0) g_string_append_printf(out, " height=\"%d\"", height);
		g_string_append_printf(out, " style=\"display:block;width:%dpx;max-width:100%%;height:auto;border:0;outline:none;text-decoration:none\">", width);
		if (r->look->website != NULL) g_string_append(out, "</a>");
		g_string_append(out, "</td></tr></table>");
	} else if (name[0] != '\0') {
		g_string_append_printf(out, "<p class=\"ve-heading\" style=\"margin:0;font-family:%s;font-size:24px;line-height:30px;font-weight:bold;color:%s;text-align:%s;letter-spacing:-0.2px\">%s</p>",
			font(r), r->look->primary, align, name);
	}
	text_paragraph(r, r->look->name);
	if (tagline != NULL && tagline[0] != '\0') {
		g_autofree gchar *escaped = words(r, tagline);
		g_string_append_printf(out, "<p class=\"ve-muted\" style=\"margin:8px 0 0;font-family:%s;font-size:14px;line-height:20px;color:%s;text-align:%s\">%s</p>",
			font(r), r->muted, align, escaped);
		text_paragraph(r, tagline);
	}
}
static void render_social(Render *r, GString *out, JsonObject *block)
{
	JsonArray *links = json_node_get_array(json_object_get_member(block, "links"));
	guint i, n;
	g_string_append_printf(out, "<p class=\"ve-text\" style=\"margin:0;font-family:%s;font-size:15px;line-height:22px;text-align:center;color:%s\">", font(r), r->look->text);
	for (i = 0; i < json_array_get_length(links); i++) {
		JsonObject *link = json_array_get_object_element(links, i);
		const gchar *network = string_member(link, "network"), *label = network;
		g_autofree gchar *url = g_markup_escape_text(string_member(link, "url"), -1);
		for (n = 0; networks[n]; n++)
			if (g_str_equal(networks[n], network)) label = network_labels[n];
		if (i) g_string_append(out, " &nbsp;&middot;&nbsp; ");
		g_string_append_printf(out, "<a class=\"ve-link\" href=\"%s\" target=\"_blank\" style=\"color:%s;font-weight:bold;text-decoration:underline\">%s</a>",
			url, r->look->primary, label);
		text_link(r, label, string_member(link, "url"));
	}
	g_string_append(out, "</p>");
}
static void render_footer(Render *r, GString *out, JsonObject *block)
{
	const gchar *text = string_member(block, "text");
	const gchar *address = string_member(block, "address");
	g_autofree gchar *name = g_markup_escape_text(r->look->name != NULL ? r->look->name : "", -1);
	if (address == NULL) address = r->look->address;
	if (text != NULL && text[0] != '\0') {
		g_autofree gchar *escaped = words(r, text);
		g_string_append_printf(out, "<p class=\"ve-muted\" style=\"margin:0 0 8px;font-family:%s;font-size:13px;line-height:19px;color:%s;text-align:center\">%s</p>",
			font(r), r->muted, escaped);
		text_paragraph(r, text);
	}
	if (name[0] != '\0' || address != NULL) {
		g_autofree gchar *where = words(r, address != NULL ? address : "");
		g_string_append_printf(out, "<p class=\"ve-muted\" style=\"margin:0;font-family:%s;font-size:13px;line-height:19px;color:%s;text-align:center\"><strong>%s</strong>%s%s</p>",
			font(r), r->muted, name, address != NULL ? "<br>" : "", where);
		text_paragraph(r, r->look->name);
		text_paragraph(r, address);
	}
	if (r->look->website != NULL) {
		g_autofree gchar *site = g_markup_escape_text(r->look->website, -1);
		g_string_append_printf(out, "<p style=\"margin:8px 0 0;font-family:%s;font-size:13px;line-height:19px;text-align:center\">"
			"<a class=\"ve-link\" href=\"%s\" target=\"_blank\" style=\"color:%s;text-decoration:underline\">%s</a></p>",
			font(r), site, r->look->primary, site);
		text_paragraph(r, r->look->website);
	}
}
/*
 * No mail app plays video inside a message. A video block is therefore a
 * poster -- the picture, with a play button drawn over it -- that links
 * to the page where the video plays. Outlook for Windows cannot draw a
 * background picture, so it gets the plain picture, linked.
 */
static gboolean render_video(Render *r, GString *out, JsonObject *block, GError **error)
{
	g_autofree gchar *poster = NULL, *src = NULL, *alt = NULL, *url = NULL, *caption = NULL, *label = NULL;
	gint natural_w, natural_h, height;
	if (!image_source(r, block, "poster_src", "poster_image_id", &poster, &natural_w, &natural_h, error))
		return FALSE;
	height = natural_w > 0 && natural_h > 0 ? (gint)((gint64)natural_h * INNER / natural_w) : 300;
	src = g_markup_escape_text(poster, -1);
	alt = g_markup_escape_text(string_member(block, "alt"), -1);
	url = g_markup_escape_text(string_member(block, "url"), -1);
	caption = words(r, string_member(block, "caption") != NULL ? string_member(block, "caption") : "Watch the video");
	g_string_append_printf(out,
		"<!--[if mso]><a href=\"%s\" target=\"_blank\"><img src=\"%s\" alt=\"%s\" width=\"%d\" height=\"%d\" style=\"display:block;border:0\"></a><![endif]-->"
		"<!--[if !mso]><!--><table role=\"presentation\" width=\"100%%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\"><tr>"
		"<td background=\"%s\" bgcolor=\"%s\" align=\"center\" valign=\"middle\" height=\"%d\" role=\"img\" aria-label=\"%s\" "
		"style=\"height:%dpx;background-color:%s;background-image:url('%s');background-size:cover;background-position:center;border-radius:6px\">"
		"<a href=\"%s\" target=\"_blank\" aria-label=\"Play: %s\" style=\"display:inline-block;width:72px;height:72px;border-radius:36px;"
		"background:rgba(0,0,0,0.62);border:3px solid #ffffff;color:#ffffff;font-family:Arial,sans-serif;font-size:0;line-height:66px;"
		"text-align:center;text-decoration:none\"><span class=\"ve-play\" style=\"display:inline-block;width:0;height:0;margin-left:6px;vertical-align:middle;border-style:solid;border-width:13px 0 13px 22px;border-color:transparent transparent transparent #ffffff\"></span></a></td></tr></table><!--<![endif]-->"
		"<p class=\"ve-muted\" style=\"margin:10px 0 0;font-family:%s;font-size:14px;line-height:20px;text-align:center;color:%s\">"
		"<a class=\"ve-link\" href=\"%s\" target=\"_blank\" style=\"color:%s;text-decoration:underline\">%s</a></p>",
		url, src, alt, INNER, height, src, r->look->text, height, alt, height, r->look->text, src,
		url, alt, font(r), r->muted, url, r->look->primary, caption);
	label = g_strdup_printf("Watch the video (%s)", string_member(block, "alt"));
	text_link(r, label, string_member(block, "url"));
	if (string_member(block, "caption") != NULL)
		text_paragraph(r, string_member(block, "caption"));
	return TRUE;
}
static gboolean render_inner(Render *r, GString *out, JsonObject *block, gint available, gboolean nested, GError **error)
{
	const gchar *type = string_member(block, "type");
	if (g_str_equal(type, "text")) render_text(r, out, block, nested);
	else if (g_str_equal(type, "image")) return render_image(r, out, block, available, error);
	else if (g_str_equal(type, "button")) render_button(r, out, block);
	else if (g_str_equal(type, "divider")) render_divider(r, out);
	else if (g_str_equal(type, "header")) render_header(r, out, block);
	else if (g_str_equal(type, "social")) render_social(r, out, block);
	else if (g_str_equal(type, "footer")) render_footer(r, out, block);
	else if (g_str_equal(type, "video")) return render_video(r, out, block, error);
	return TRUE;
}
static gboolean render_blocks(Render *r, JsonArray *blocks, GError **error);
static gboolean render_columns(Render *r, JsonObject *block, GError **error)
{
	JsonArray *columns = json_node_get_array(json_object_get_member(block, "columns"));
	guint n = json_array_get_length(columns), c, i;
	gint gutter = 16, width = (INNER - gutter * ((gint)n - 1)) / (gint)n, percent = 100 / (gint)n;
	g_string_append_printf(r->html, "<tr><td class=\"ve-pad\" style=\"padding:8px %dpx\">"
		"<table role=\"presentation\" width=\"100%%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\"><tr>", PAD - gutter / 2);
	for (c = 0; c < n; c++) {
		JsonArray *inner = json_object_get_array_member(json_array_get_object_element(columns, c), "blocks");
		g_string_append_printf(r->html, "<td class=\"ve-col\" width=\"%d%%\" valign=\"top\" style=\"width:%d%%;padding:0 %dpx;vertical-align:top\">",
			percent, percent, gutter / 2);
		for (i = 0; i < json_array_get_length(inner); i++) {
			g_string_append(r->html, "<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\"><tr><td style=\"padding:6px 0\">");
			if (!render_inner(r, r->html, json_array_get_object_element(inner, i), width, TRUE, error))
				return FALSE;
			g_string_append(r->html, "</td></tr></table>");
		}
		g_string_append(r->html, "</td>");
	}
	g_string_append(r->html, "</tr></table></td></tr>");
	return TRUE;
}
static gboolean render_section(Render *r, JsonObject *block, GError **error)
{
	gint64 id = int_member(block, "section_id", 0);
	g_autofree gchar *json = NULL;
	g_autoptr(JsonNode) node = NULL;
	gboolean ok;
	if (r->resolver == NULL || r->resolver->section == NULL)
		return refuse(error, "The design names section %" G_GINT64_FORMAT ", which is not available here", id);
	if (g_hash_table_contains(r->visiting, &id))
		return refuse(error, "Section %" G_GINT64_FORMAT " includes itself, through the sections it names", id);
	if (r->depth >= VENTURE_EMAIL_DESIGN_MAX_SECTION_DEPTH)
		return refuse(error, "Sections nest more than %d deep", VENTURE_EMAIL_DESIGN_MAX_SECTION_DEPTH);
	json = r->resolver->section(id, r->resolver->data, error);
	if (json == NULL)
		return FALSE;
	node = parse(json, error);
	if (node == NULL)
		return FALSE;
	g_hash_table_add(r->visiting, g_memdup2(&id, sizeof id));
	r->depth++;
	ok = render_blocks(r, json_object_get_array_member(json_node_get_object(node), "blocks"), error);
	r->depth--;
	g_hash_table_remove(r->visiting, &id);
	return ok;
}
static gboolean render_blocks(Render *r, JsonArray *blocks, GError **error)
{
	guint i;
	for (i = 0; i < json_array_get_length(blocks); i++) {
		JsonObject *block = json_array_get_object_element(blocks, i);
		const gchar *type = string_member(block, "type");
		if (g_str_equal(type, "section")) {
			if (!render_section(r, block, error))
				return FALSE;
			continue;
		}
		if (++r->top > VENTURE_EMAIL_DESIGN_MAX_BLOCKS)
			return refuse(error, "With its sections in place, the design has more than %d blocks", VENTURE_EMAIL_DESIGN_MAX_BLOCKS);
		if (g_str_equal(type, "columns")) {
			if (!render_columns(r, block, error))
				return FALSE;
			continue;
		}
		if (g_str_equal(type, "footer"))
			g_string_append_printf(r->html, "<tr><td class=\"ve-pad\" style=\"padding:16px %dpx 28px\">", PAD);
		else if (g_str_equal(type, "header"))
			g_string_append_printf(r->html, "<tr><td class=\"ve-pad\" style=\"padding:28px %dpx 16px\">", PAD);
		else
			g_string_append_printf(r->html, "<tr><td class=\"ve-pad\" style=\"padding:10px %dpx\">", PAD);
		if (!render_inner(r, r->html, block, INNER, FALSE, error))
			return FALSE;
		g_string_append(r->html, "</td></tr>");
	}
	return TRUE;
}
static void style_block(Render *r, GString *out)
{
	const VentureEmailLook *l = r->look;
	g_autoptr(GString) css = g_string_new(NULL);
	g_autofree gchar *dark_bg = g_strdup(l->dark_background != NULL ? l->dark_background : "#111418");
	g_autofree gchar *dark_card = g_strdup(l->dark_card != NULL ? l->dark_card : "#1b1f24");
	g_autofree gchar *dark_text = g_strdup(l->dark_text != NULL ? l->dark_text : "#e5e7eb");
	static const gchar *const scopes[] = { "@media (prefers-color-scheme: dark) {\n", "[data-ogsc] ", NULL };
	guint i;
	g_string_append(css, ":root { color-scheme: light dark; supported-color-schemes: light dark; }\n"
		"body { margin: 0; padding: 0; width: 100% !important; -webkit-text-size-adjust: 100%; -ms-text-size-adjust: 100%; }\n"
		"img { border: 0; outline: none; -ms-interpolation-mode: bicubic; }\n"
		"table, td { mso-table-lspace: 0pt; mso-table-rspace: 0pt; }\n"
		"@media (max-width: 620px) {\n"
		"  .ve-wrap { width: 100% !important; }\n"
		"  .ve-pad { padding-left: 20px !important; padding-right: 20px !important; }\n"
		"  .ve-col { display: block !important; width: 100% !important; padding: 0 !important; }\n"
		"}\n");
	/* Dark mode for the apps that honour it, and the same colours under
	 * the attribute Outlook.com puts on a message it darkens itself. */
	for (i = 0; scopes[i]; i++) {
		gboolean media = i == 0;
		const gchar *p = media ? "  " : scopes[i];
		if (media) g_string_append(css, scopes[i]);
		g_string_append_printf(css, "%s.ve-body, %s.ve-bg { background: %s !important; background-color: %s !important; }\n", p, p, dark_bg, dark_bg);
		g_string_append_printf(css, "%s.ve-card { background: %s !important; background-color: %s !important; }\n", p, dark_card, dark_card);
		g_string_append_printf(css, "%s.ve-text { color: %s !important; }\n", p, dark_text);
		g_string_append_printf(css, "%s.ve-heading, %s.ve-link { color: %s !important; }\n", p, p, r->dark_primary);
		g_string_append_printf(css, "%s.ve-muted { color: %s !important; }\n", p, r->dark_muted);
		g_string_append_printf(css, "%s.ve-divider { border-top-color: %s !important; }\n", p, r->dark_divider);
		g_string_append_printf(css, "%s.ve-btn, %s.ve-btn-a { background: %s !important; background-color: %s !important; }\n", p, p, r->dark_primary, r->dark_primary);
		g_string_append_printf(css, "%s.ve-btn-a { color: %s !important; }\n", p, r->dark_button_ink);
		g_string_append_printf(css, "%s.ve-logo { background: #ffffff !important; padding: 8px 12px !important; }\n", p);
		if (media) g_string_append(css, "}\n");
	}
	if (r->template_syntax) {
		g_auto(GStrv) parts = g_strsplit(css->str, "{", -1);
		g_autofree gchar *doubled = g_strjoinv("{{", parts);
		g_string_append(out, doubled);
	} else
		g_string_append(out, css->str);
}
gboolean venture_email_design_render(const gchar *json, const VentureEmailLook *look, const VentureEmailResolver *resolver,
	const gchar *title, gboolean template_syntax, gchar **html, gchar **text, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) body = g_string_new(NULL), out = g_string_new(NULL);
	g_autoptr(GHashTable) visiting = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *escaped_title = NULL, *preheader = NULL;
	Render r;
	gboolean ok;
	g_return_val_if_fail(look != NULL && html != NULL && text != NULL, FALSE);
	*html = NULL; *text = NULL;
	node = parse(json, error);
	if (node == NULL)
		return FALSE;
	memset(&r, 0, sizeof r);
	r.look = look;
	r.resolver = resolver;
	r.template_syntax = template_syntax;
	r.html = body;
	r.text = g_string_new(NULL);
	r.visiting = visiting;
	r.muted = mix(look->text, look->card, 0.32);
	r.divider = mix(look->text, look->card, 0.86);
	r.dark_primary = look->dark_primary != NULL ? g_strdup(look->dark_primary)
		: lift(look->primary, look->dark_card != NULL ? look->dark_card : "#1b1f24");
	r.dark_muted = mix(look->dark_text != NULL ? look->dark_text : "#e5e7eb", look->dark_card != NULL ? look->dark_card : "#1b1f24", 0.3);
	r.dark_divider = mix(look->dark_text != NULL ? look->dark_text : "#e5e7eb", look->dark_card != NULL ? look->dark_card : "#1b1f24", 0.8);
	r.button_ink = g_strdup(ink_on(look->primary));
	r.dark_button_ink = g_strdup(ink_on(r.dark_primary));
	ok = render_blocks(&r, json_object_get_array_member(json_node_get_object(node), "blocks"), error);
	if (ok) {
		escaped_title = words(&r, title != NULL ? title : (look->name != NULL ? look->name : ""));
		g_string_append_printf(out, "<!DOCTYPE html>\n<html lang=\"en\" xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:o=\"urn:schemas-microsoft-com:office:office\">\n"
			"<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
			"<meta name=\"x-apple-disable-message-reformatting\">\n<meta name=\"color-scheme\" content=\"light dark\">\n"
			"<meta name=\"supported-color-schemes\" content=\"light dark\">\n<title>%s</title>\n"
			"<!--[if mso]><noscript><xml><o:OfficeDocumentSettings><o:PixelsPerInch>96</o:PixelsPerInch></o:OfficeDocumentSettings></xml></noscript><![endif]-->\n"
			"<style>\n", escaped_title);
		style_block(&r, out);
		g_string_append_printf(out, "</style>\n</head>\n<body class=\"ve-body\" style=\"margin:0;padding:0;width:100%%;background:%s;-webkit-text-size-adjust:100%%\">\n",
			look->background);
		if (json_object_has_member(json_node_get_object(node), "preheader")) {
			preheader = words(&r, string_member(json_node_get_object(node), "preheader"));
			/* The line a mail app shows beside the subject, kept out of the
			 * message itself; the spacer stops the body running on after it. */
			g_string_append_printf(out, "<div style=\"display:none;font-size:1px;line-height:1px;max-height:0;max-width:0;opacity:0;overflow:hidden;mso-hide:all\" aria-hidden=\"true\">%s"
				"&#8204;&nbsp;&#8204;&nbsp;&#8204;&nbsp;&#8204;&nbsp;&#8204;&nbsp;&#8204;&nbsp;</div>\n", preheader);
		}
		g_string_append_printf(out, "<table role=\"presentation\" class=\"ve-bg\" width=\"100%%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" bgcolor=\"%s\" style=\"background:%s\">\n"
			"<tr><td align=\"center\" style=\"padding:24px 12px\">\n"
			"<!--[if mso]><table role=\"presentation\" width=\"%d\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" align=\"center\"><tr><td><![endif]-->\n"
			"<table role=\"presentation\" class=\"ve-wrap ve-card\" width=\"100%%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" bgcolor=\"%s\" "
			"style=\"width:100%%;max-width:%dpx;background:%s;border-radius:10px\">\n%s\n</table>\n"
			"<!--[if mso]></td></tr></table><![endif]-->\n</td></tr>\n</table>\n</body>\n</html>\n",
			look->background, look->background, VENTURE_EMAIL_DESIGN_WIDTH, look->card, VENTURE_EMAIL_DESIGN_WIDTH, look->card, body->str);
		*html = g_string_free(g_steal_pointer(&out), FALSE);
		g_string_append_c(r.text, '\n');
		*text = g_string_free(r.text, FALSE);
		r.text = NULL;
	}
	if (r.text != NULL) g_string_free(r.text, TRUE);
	g_free(r.muted); g_free(r.divider); g_free(r.dark_primary); g_free(r.dark_muted);
	g_free(r.dark_divider); g_free(r.button_ink); g_free(r.dark_button_ink);
	return ok;
}
