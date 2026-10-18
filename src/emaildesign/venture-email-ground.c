/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The grounding rule, in code: a designed email may repeat a price, an
 * amount, a percentage or a date only if the person's brief or the
 * organization's records say it. A model told so in its prompt usually
 * complies; "usually" is the wrong word for a discount a business would
 * then have to honour, so every number in the visible text is checked
 * against the numbers in the sources, and so is the word "free".
 *
 * The check is deliberately crude. It does no arithmetic -- "10% off $129"
 * written as "$116.10" is a number nobody gave, and is removed -- and it
 * cannot tell a year from a price, so a year in the copy must be in the
 * brief too. Removing a sentence that was true is the cheap way to be
 * wrong; sending one that was invented is not.
 */
#include "venture-email-design-private.h"
#include <string.h>

/* "129.00", "129" and "1,29" all name one number: commas out, trailing
 * fraction zeros out, leading zeros out. */
static gchar *normal_number(const gchar *start, gsize length)
{
	GString *out = g_string_new(NULL);
	gsize i;
	gchar *dot;
	for (i = 0; i < length; i++)
		if (start[i] != ',')
			g_string_append_c(out, start[i]);
	dot = strchr(out->str, '.');
	if (dot != NULL) {
		while (out->len > 0 && out->str[out->len - 1] == '0')
			g_string_truncate(out, out->len - 1);
		if (out->len > 0 && out->str[out->len - 1] == '.')
			g_string_truncate(out, out->len - 1);
	}
	while (out->len > 1 && out->str[0] == '0' && out->str[1] != '.')
		g_string_erase(out, 0, 1);
	return g_string_free(out, FALSE);
}
static const struct { const gchar *word; const gchar *number; } spelled[] = {
	{ "one", "1" }, { "two", "2" }, { "three", "3" }, { "four", "4" }, { "five", "5" }, { "six", "6" },
	{ "seven", "7" }, { "eight", "8" }, { "nine", "9" }, { "ten", "10" }, { "eleven", "11" }, { "twelve", "12" },
	{ "fifteen", "15" }, { "twenty", "20" }, { "twenty-five", "25" }, { "thirty", "30" }, { "forty", "40" },
	{ "fifty", "50" }, { "hundred", "100" }, { "half", "50" }
};
/* A spelled number that names an amount: "ten percent", "half off",
 * "fifty dollars". A spelled number elsewhere ("one visit") is a word. */
static const gchar *spelled_amount(const gchar *text, const gchar *p, gsize *length)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(spelled); i++) {
		gsize n = strlen(spelled[i].word);
		const gchar *after = p + n;
		if (g_ascii_strncasecmp(p, spelled[i].word, n) != 0 || g_ascii_isalpha(*after) || *after == '-')
			continue;
		if (p > text && (g_ascii_isalpha(p[-1]) || p[-1] == '-'))
			continue;
		while (*after == ' ') after++;
		if (*after == '%' || g_ascii_strncasecmp(after, "percent", 7) == 0 || g_ascii_strncasecmp(after, "per cent", 8) == 0 ||
		    g_ascii_strncasecmp(after, "off", 3) == 0 || g_ascii_strncasecmp(after, "dollar", 6) == 0) {
			*length = n;
			return spelled[i].number;
		}
	}
	return NULL;
}
static gboolean is_free(const gchar *text, const gchar *p)
{
	return g_ascii_strncasecmp(p, "free", 4) == 0 && !g_ascii_isalpha(p[4]) &&
		(p == text || !g_ascii_isalpha(p[-1]));
}
/* Each number in @text, normalised, to @each; stops at the first FALSE. */
typedef gboolean (*NumberFunc)(const gchar *number, const gchar *start, gsize length, gpointer data);
static void numbers(const gchar *text, NumberFunc each, gpointer data)
{
	const gchar *p = text;
	while (*p) {
		gsize length = 0;
		const gchar *word;
		if (g_ascii_isdigit(*p)) {
			g_autofree gchar *number = NULL;
			while (g_ascii_isdigit(p[length]) || ((p[length] == ',' || p[length] == '.') && g_ascii_isdigit(p[length + 1])))
				length++;
			number = normal_number(p, length);
			if (!each(number, p, length, data))
				return;
			p += length;
			continue;
		}
		word = g_ascii_isalpha(*p) ? spelled_amount(text, p, &length) : NULL;
		if (word != NULL) {
			if (!each(word, p, length, data))
				return;
			p += length;
			continue;
		}
		p++;
	}
}
static gboolean add_number(const gchar *number, const gchar *start, gsize length, gpointer data)
{
	(void)start; (void)length;
	g_hash_table_add(data, g_strdup(number));
	return TRUE;
}
void venture_email_grounds_init(VentureEmailGrounds *grounds, const gchar *const *sources)
{
	guint i;
	const gchar *p;
	grounds->numbers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	grounds->free_ok = FALSE;
	for (i = 0; sources != NULL && sources[i] != NULL; i++) {
		numbers(sources[i], add_number, grounds->numbers);
		for (p = sources[i]; *p; p++)
			if (is_free(sources[i], p))
				grounds->free_ok = TRUE;
	}
}
void venture_email_grounds_clear(VentureEmailGrounds *grounds)
{
	g_clear_pointer(&grounds->numbers, g_hash_table_unref);
}
typedef struct { const VentureEmailGrounds *grounds; const gchar *text; gchar *claim; } Finding;
static gboolean find_claim(const gchar *number, const gchar *start, gsize length, gpointer data)
{
	Finding *finding = data;
	const gchar *from = start, *to = start + length;
	if (g_hash_table_contains(finding->grounds->numbers, number))
		return TRUE;
	/* Report it as written, with its currency sign or percent. */
	if (from > finding->text && (from[-1] == '$' || from[-1] == '#')) from--;
	while (*to == '%') to++;
	finding->claim = g_strndup(from, to - from);
	return FALSE;
}
gchar *venture_email_grounds_claim(const VentureEmailGrounds *grounds, const gchar *text)
{
	Finding finding;
	const gchar *p;
	if (text == NULL)
		return NULL;
	finding.grounds = grounds;
	finding.text = text;
	finding.claim = NULL;
	numbers(text, find_claim, &finding);
	if (finding.claim != NULL)
		return finding.claim;
	for (p = text; *p && !grounds->free_ok; p++)
		if (is_free(text, p))
			return g_strndup(p, 4);
	return NULL;
}

/* Sentences, each with the space after it, so dropping one leaves the
 * others joined as they were written. A full stop between digits is a
 * decimal point, not an end. */
static GPtrArray *sentences(const gchar *text)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	const gchar *start = text, *p = text;
	while (*p) {
		gboolean end = *p == '\n' || ((*p == '.' || *p == '!' || *p == '?') && (p[1] == '\0' || g_ascii_isspace(p[1])));
		p++;
		if (end) {
			while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
			g_ptr_array_add(out, g_strndup(start, p - start));
			start = p;
		}
	}
	if (*start)
		g_ptr_array_add(out, g_strdup(start));
	return out;
}
/* @text without its ungrounded sentences, or NULL with nothing removed. */
static gchar *strip(const VentureEmailGrounds *grounds, const gchar *text, GPtrArray *stripped)
{
	g_autoptr(GPtrArray) parts = sentences(text);
	g_autoptr(GString) kept = g_string_new(NULL);
	gboolean changed = FALSE;
	guint i;
	for (i = 0; i < parts->len; i++) {
		const gchar *sentence = g_ptr_array_index(parts, i);
		g_autofree gchar *claim = venture_email_grounds_claim(grounds, sentence);
		if (claim == NULL) {
			g_string_append(kept, sentence);
			continue;
		}
		changed = TRUE;
		if (stripped != NULL) {
			g_autofree gchar *trimmed = g_strstrip(g_strdup(sentence));
			g_ptr_array_add(stripped, g_strdup_printf("\"%s\" -- %s is in neither the brief nor the records", trimmed, claim));
		}
	}
	if (!changed)
		return NULL;
	/* Paragraph breaks that now end the text are gone with their sentence. */
	while (kept->len > 0 && g_ascii_isspace(kept->str[kept->len - 1]))
		g_string_truncate(kept, kept->len - 1);
	return g_string_free(g_steal_pointer(&kept), FALSE);
}
static const gchar *const strippable[] = { "body", "text", NULL };
static const gchar *const whole[] = { "heading", "label", "tagline", "caption", "alt", "address", NULL };

static gboolean ground_blocks(const VentureEmailGrounds *grounds, JsonArray *blocks, GPtrArray *stripped, gboolean *changed, GError **error)
{
	guint i, k;
	for (i = 0; i < json_array_get_length(blocks); ) {
		JsonObject *block = json_array_get_object_element(blocks, i);
		gboolean drop = FALSE;
		for (k = 0; whole[k]; k++) {
			const gchar *value = json_object_has_member(block, whole[k]) ? json_object_get_string_member(block, whole[k]) : NULL;
			g_autofree gchar *claim = value != NULL ? venture_email_grounds_claim(grounds, value) : NULL;
			if (claim != NULL) {
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					"The design says \"%s\": %s is in neither the brief nor the records, and this %s cannot lose a sentence",
					value, claim, whole[k]);
				return FALSE;
			}
		}
		for (k = 0; strippable[k]; k++) {
			const gchar *value = json_object_has_member(block, strippable[k]) ? json_object_get_string_member(block, strippable[k]) : NULL;
			g_autofree gchar *kept = value != NULL ? strip(grounds, value, stripped) : NULL;
			if (kept == NULL)
				continue;
			*changed = TRUE;
			if (kept[0] != '\0')
				json_object_set_string_member(block, strippable[k], kept);
			else if (g_str_equal(strippable[k], "text"))
				json_object_remove_member(block, "text");
			else
				drop = TRUE;
		}
		if (json_object_has_member(block, "columns")) {
			JsonArray *columns = json_object_get_array_member(block, "columns");
			for (k = 0; k < json_array_get_length(columns); k++)
				if (!ground_blocks(grounds, json_object_get_array_member(json_array_get_object_element(columns, k), "blocks"), stripped, changed, error))
					return FALSE;
		}
		if (json_object_has_member(block, "links")) {
			/* Link labels are fixed network names; nothing to ground. */
		}
		if (drop)
			json_array_remove_element(blocks, i);
		else
			i++;
	}
	return TRUE;
}
gchar *venture_email_design_ground(const gchar *design, const gchar *const *sources, GPtrArray *stripped, GError **error)
{
	g_autoptr(GError) invalid = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureEmailGrounds grounds;
	JsonObject *object;
	gboolean changed = FALSE, ok;
	if (!venture_email_design_validate(design, &invalid)) {
		g_propagate_error(error, g_steal_pointer(&invalid));
		return NULL;
	}
	node = json_from_string(design, NULL);
	object = json_node_get_object(node);
	venture_email_grounds_init(&grounds, sources);
	ok = ground_blocks(&grounds, json_object_get_array_member(object, "blocks"), stripped, &changed, error);
	if (ok && json_object_has_member(object, "preheader")) {
		g_autofree gchar *kept = strip(&grounds, json_object_get_string_member(object, "preheader"), stripped);
		if (kept != NULL) {
			changed = TRUE;
			if (kept[0] != '\0') json_object_set_string_member(object, "preheader", kept);
			else json_object_remove_member(object, "preheader");
		}
	}
	venture_email_grounds_clear(&grounds);
	if (!ok)
		return NULL;
	if (!changed)
		return g_strdup(design);
	return json_to_string(node, FALSE);
}

/* --- Addresses and images a design uses ------------------------------------------- */

void venture_email_collect_urls(GHashTable *into, const gchar *text)
{
	static GRegex *regex;
	g_autoptr(GMatchInfo) info = NULL;
	if (g_once_init_enter(&regex))
		g_once_init_leave(&regex, g_regex_new("https?://[^\\s\"'<>{}\\\\]+", G_REGEX_CASELESS, 0, NULL));
	if (text == NULL)
		return;
	g_regex_match(regex, text, 0, &info);
	while (g_match_info_matches(info)) {
		gchar *url = g_match_info_fetch(info, 0);
		gsize n = strlen(url);
		/* A sentence's full stop or a closing bracket is not the address. */
		while (n > 0 && strchr(".,;:!?)]", url[n - 1]) != NULL)
			url[--n] = '\0';
		g_hash_table_add(into, url);
		g_match_info_next(info, NULL);
	}
}
static void walk(JsonArray *blocks, GHashTable *urls, GHashTable *ids)
{
	static const gchar *const url_members[] = { "src", "link", "url", "poster_src", NULL };
	static const gchar *const id_members[] = { "image_id", "poster_image_id", NULL };
	guint i, k;
	for (i = 0; blocks != NULL && i < json_array_get_length(blocks); i++) {
		JsonObject *block = json_array_get_object_element(blocks, i);
		for (k = 0; urls != NULL && url_members[k]; k++)
			if (json_object_has_member(block, url_members[k]))
				g_hash_table_add(urls, g_strdup(json_object_get_string_member(block, url_members[k])));
		for (k = 0; ids != NULL && id_members[k]; k++)
			if (json_object_has_member(block, id_members[k])) {
				gint64 *id = g_new(gint64, 1);
				*id = json_object_get_int_member(block, id_members[k]);
				g_hash_table_add(ids, id);
			}
		if (urls != NULL && json_object_has_member(block, "links")) {
			JsonArray *links = json_object_get_array_member(block, "links");
			for (k = 0; k < json_array_get_length(links); k++)
				g_hash_table_add(urls, g_strdup(json_object_get_string_member(json_array_get_object_element(links, k), "url")));
		}
		if (json_object_has_member(block, "columns")) {
			JsonArray *columns = json_object_get_array_member(block, "columns");
			for (k = 0; k < json_array_get_length(columns); k++)
				walk(json_object_get_array_member(json_array_get_object_element(columns, k), "blocks"), urls, ids);
		}
	}
}
void venture_email_design_urls(const gchar *design, GHashTable *into)
{
	g_autoptr(JsonNode) node = json_from_string(design, NULL);
	if (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
		walk(json_object_get_array_member(json_node_get_object(node), "blocks"), into, NULL);
}
void venture_email_design_image_ids(const gchar *design, GHashTable *into)
{
	g_autoptr(JsonNode) node = json_from_string(design, NULL);
	if (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
		walk(json_object_get_array_member(json_node_get_object(node), "blocks"), NULL, into);
}
