/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PHONE_H
#define VENTURE_PHONE_H
#include <glib.h>
G_BEGIN_DECLS
/**
 * venture_phone_normalize:
 * @raw: (nullable): a phone number as people write it
 * @default_country: (nullable): two-letter country for a number written
 *   without its country code; %NULL or empty is US
 *
 * Reads "+44 20 7946 0123", "(555) 010-0123", "00 49 30 1234567" or
 * "011 44 ..." into E.164: a plus and 8 to 15 digits. A national number
 * loses its trunk 0 (or the leading 1 in North America).
 *
 * Returns: (transfer full) (nullable): the E.164 number, or %NULL when it
 *   cannot be read as one
 */
gchar *venture_phone_normalize(const gchar *raw, const gchar *default_country);
/**
 * venture_phone_country_code:
 * @country: two-letter country
 *
 * Returns: (nullable): its calling code without the plus, or %NULL when the
 *   table does not know it
 */
const gchar *venture_phone_country_code(const gchar *country);
G_END_DECLS
#endif
