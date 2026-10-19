/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

/* Calling codes for the countries an organization is likely to name as
 * its own. A number written with its country code ("+33 ...", "00 33
 * ...") needs no entry here; the table only reads national numbers. */
static const struct {
	const gchar *country;
	const gchar *code;
	gboolean keeps_zero;	/* Italy dials its leading 0 from abroad too */
} countries[] = {
	{ "US", "1", FALSE }, { "CA", "1", FALSE }, { "PR", "1", FALSE }, { "GB", "44", FALSE },
	{ "IE", "353", FALSE }, { "DE", "49", FALSE }, { "FR", "33", FALSE }, { "ES", "34", FALSE },
	{ "IT", "39", TRUE }, { "NL", "31", FALSE }, { "BE", "32", FALSE }, { "CH", "41", FALSE },
	{ "AT", "43", FALSE }, { "SE", "46", FALSE }, { "NO", "47", FALSE }, { "DK", "45", FALSE },
	{ "FI", "358", FALSE }, { "PT", "351", FALSE }, { "PL", "48", FALSE }, { "AU", "61", FALSE },
	{ "NZ", "64", FALSE }, { "MX", "52", FALSE }, { "BR", "55", FALSE }, { "IN", "91", FALSE },
	{ "JP", "81", FALSE }, { "ZA", "27", FALSE }, { "SG", "65", FALSE }
};

static gsize
country_index(const gchar *country)
{
	gsize i;
	for (i = 0; country != NULL && i < G_N_ELEMENTS(countries); i++)
		if (g_ascii_strcasecmp(countries[i].country, country) == 0)
			return i;
	return G_N_ELEMENTS(countries);
}

const gchar *
venture_phone_country_code(const gchar *country)
{
	gsize i = country_index(country);
	return i < G_N_ELEMENTS(countries) ? countries[i].code : NULL;
}

gchar *
venture_phone_normalize(const gchar *raw, const gchar *default_country)
{
	g_autoptr(GString) digits = g_string_new(NULL);
	g_autofree gchar *text = NULL;
	const gchar *p, *code;
	gboolean plus = FALSE, seen = FALSE, nanp;
	gsize index;
	if (raw == NULL)
		return NULL;
	text = g_strstrip(g_strdup(raw));
	p = text;
	if (g_ascii_strncasecmp(p, "tel:", 4) == 0)
		p += 4;
	for (; *p != '\0'; p++)
	{
		/* "+44 (0)20 ..." writes the trunk 0 a caller from abroad drops. */
		if (strncmp(p, "(0)", 3) == 0)
		{
			p += 2;
			continue;
		}
		if (g_ascii_isdigit(*p))
		{
			g_string_append_c(digits, *p);
			seen = TRUE;
		}
		else if (*p == '+' && !seen && !plus)
			plus = TRUE;
		else if (g_ascii_isalpha(*p))
			return NULL;
		else if (!strchr(" -.()/\t", *p))
			return NULL;
	}
	if (digits->len == 0)
		return NULL;
	index = country_index(venture_string_is_empty(default_country) ? "US" : default_country);
	if (index >= G_N_ELEMENTS(countries))
		index = 0;
	code = countries[index].code;
	nanp = g_strcmp0(code, "1") == 0;
	if (!plus)
	{
		if (g_str_has_prefix(digits->str, "00"))
			g_string_erase(digits, 0, 2);
		else if (nanp && g_str_has_prefix(digits->str, "011"))
			g_string_erase(digits, 0, 3);
		else if (nanp)
		{
			if (digits->len == 11 && digits->str[0] == '1')
				g_string_erase(digits, 0, 1);
			if (digits->len != 10)
				return NULL;
			g_string_prepend(digits, code);
		}
		else
		{
			if (digits->str[0] == '0' && !countries[index].keeps_zero)
				g_string_erase(digits, 0, 1);
			g_string_prepend(digits, code);
		}
	}
	/* North American numbers are always eleven digits with the 1. */
	if (digits->str[0] == '1' && digits->len != 11)
		return NULL;
	if (digits->len < 8 || digits->len > 15 || digits->str[0] == '0')
		return NULL;
	g_string_prepend_c(digits, '+');
	return g_string_free(g_steal_pointer(&digits), FALSE);
}
