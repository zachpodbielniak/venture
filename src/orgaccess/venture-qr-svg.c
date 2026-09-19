/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * A small QR encoder: byte mode, error-correction level M, versions 1-10.
 * That covers an otpauth:// URI with a long username and a long install
 * name, which is the only thing it is asked to draw. The structure follows
 * ISO/IEC 18004 section by section: encode, split into blocks, append
 * Reed-Solomon codewords, interleave, place function patterns, place data,
 * pick the mask with the lowest penalty, write the format bits.
 */
#include "venture.h"
#include <string.h>

#define QR_MAX_VERSION 10
#define QR_QUIET_ZONE 4

/* Per version, index 1..10: total codewords, EC codewords per block, then
 * the two block groups (count, data codewords each). Level M throughout. */
static const guint16 qr_total_codewords[QR_MAX_VERSION + 1] = { 0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346 };
static const guint8 qr_ec_per_block[QR_MAX_VERSION + 1] = { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
static const guint8 qr_group1_blocks[QR_MAX_VERSION + 1] = { 0, 1, 1, 1, 2, 2, 4, 4, 2, 3, 4 };
static const guint8 qr_group1_data[QR_MAX_VERSION + 1] = { 0, 16, 28, 44, 32, 43, 27, 31, 38, 36, 43 };
static const guint8 qr_group2_blocks[QR_MAX_VERSION + 1] = { 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 1 };
static const guint8 qr_group2_data[QR_MAX_VERSION + 1] = { 0, 0, 0, 0, 0, 0, 0, 0, 39, 37, 44 };
static const guint8 qr_alignment[QR_MAX_VERSION + 1][3] = {
	{ 0, 0, 0 }, { 0, 0, 0 }, { 6, 18, 0 }, { 6, 22, 0 }, { 6, 26, 0 }, { 6, 30, 0 },
	{ 6, 34, 0 }, { 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 }, { 6, 28, 50 }
};

typedef struct
{
	guint	 size;
	guint8	*modules;	/* 1 = dark */
	guint8	*function;	/* 1 = function pattern, not data */
} QrMatrix;

static guint
qr_data_codewords(guint version)
{
	return (guint)qr_group1_blocks[version] * qr_group1_data[version] +
	       (guint)qr_group2_blocks[version] * qr_group2_data[version];
}

/* Bytes of payload a version holds in byte mode after the mode and count
 * header: 12 bits of header up to version 9, 20 from version 10. */
static guint
qr_byte_capacity(guint version)
{
	guint bits = qr_data_codewords(version) * 8 - ((version >= 10) ? 20 : 12);
	return bits / 8;
}

/* --- GF(256) and Reed-Solomon ------------------------------------------- */

static guint8
gf_multiply(guint8 x, guint8 y)
{
	guint z = 0;
	gint i;
	for (i = 7; i >= 0; i--)
	{
		z = (z << 1) ^ ((z >> 7) * 0x11D);
		z ^= ((y >> i) & 1) * x;
	}
	return (guint8)z;
}

/* Monic generator polynomial of the given degree, leading term omitted. */
static void
rs_generator(guint degree, guint8 *out)
{
	guint8 root = 1;
	guint i, j;
	memset(out, 0, degree);
	out[degree - 1] = 1;
	for (i = 0; i < degree; i++)
	{
		for (j = 0; j < degree; j++)
		{
			out[j] = gf_multiply(out[j], root);
			if (j + 1 < degree)
				out[j] ^= out[j + 1];
		}
		root = gf_multiply(root, 0x02);
	}
}

static void
rs_remainder(const guint8 *data, guint length, const guint8 *generator, guint degree, guint8 *out)
{
	guint i, j;
	memset(out, 0, degree);
	for (i = 0; i < length; i++)
	{
		guint8 factor = data[i] ^ out[0];
		memmove(out, out + 1, degree - 1);
		out[degree - 1] = 0;
		for (j = 0; j < degree; j++)
			out[j] ^= gf_multiply(generator[j], factor);
	}
}

/* --- Bit stream ---------------------------------------------------------- */

typedef struct
{
	guint8	*bytes;
	guint	 bit_length;
} BitBuffer;

static void
bits_append(BitBuffer *buffer, guint32 value, guint count)
{
	gint i;
	for (i = (gint)count - 1; i >= 0; i--)
	{
		if ((value >> i) & 1)
			buffer->bytes[buffer->bit_length >> 3] |= (guint8)(0x80 >> (buffer->bit_length & 7));
		buffer->bit_length++;
	}
}

/* Encodes @text and returns the final interleaved codeword sequence. */
static guint8 *
qr_codewords(const gchar *text, gsize length, guint version)
{
	guint data_length = qr_data_codewords(version);
	guint total = qr_total_codewords[version];
	guint ec_length = qr_ec_per_block[version];
	guint blocks = (guint)qr_group1_blocks[version] + qr_group2_blocks[version];
	guint short_length = qr_group1_data[version];
	g_autofree guint8 *data = g_malloc0(data_length);
	g_autofree guint8 *generator = g_malloc0(ec_length);
	g_autofree guint8 *ec = g_malloc0((gsize)blocks * ec_length);
	guint8 *result = g_malloc0(total);
	BitBuffer buffer = { data, 0 };
	guint i, b, offset, out;
	gsize k;

	bits_append(&buffer, 0x4, 4);
	bits_append(&buffer, (guint32)length, (version >= 10) ? 16 : 8);
	for (k = 0; k < length; k++)
		bits_append(&buffer, (guchar)text[k], 8);
	/* Terminator, then whole bytes of the alternating pad pattern. */
	bits_append(&buffer, 0, MIN(4, data_length * 8 - buffer.bit_length));
	bits_append(&buffer, 0, (8 - buffer.bit_length % 8) % 8);
	for (i = 0xEC; buffer.bit_length < data_length * 8; i ^= 0xEC ^ 0x11)
		bits_append(&buffer, i, 8);

	rs_generator(ec_length, generator);
	offset = 0;
	for (b = 0; b < blocks; b++)
	{
		guint block_length = short_length + ((b >= qr_group1_blocks[version]) ? 1 : 0);
		rs_remainder(data + offset, block_length, generator, ec_length, ec + (gsize)b * ec_length);
		offset += block_length;
	}

	/* Interleave: the i-th data byte of every block, then the i-th EC
	 * byte of every block. Longer blocks contribute their last byte after
	 * the short ones run out. */
	out = 0;
	for (i = 0; i < short_length + 1; i++)
	{
		offset = 0;
		for (b = 0; b < blocks; b++)
		{
			guint block_length = short_length + ((b >= qr_group1_blocks[version]) ? 1 : 0);
			if (i < block_length)
				result[out++] = data[offset + i];
			offset += block_length;
		}
	}
	for (i = 0; i < ec_length; i++)
		for (b = 0; b < blocks; b++)
			result[out++] = ec[(gsize)b * ec_length + i];
	g_assert(out == total);
	return result;
}

/* --- Matrix ---------------------------------------------------------------- */

static void
qr_set_function(QrMatrix *m, gint x, gint y, gboolean dark)
{
	if (x < 0 || y < 0 || x >= (gint)m->size || y >= (gint)m->size)
		return;
	m->modules[y * m->size + x] = dark ? 1 : 0;
	m->function[y * m->size + x] = 1;
}

static void
qr_draw_finder(QrMatrix *m, gint x, gint y)
{
	gint dx, dy;
	for (dy = -4; dy <= 4; dy++)
		for (dx = -4; dx <= 4; dx++)
		{
			gint distance = MAX(ABS(dx), ABS(dy));
			qr_set_function(m, x + dx, y + dy, distance != 2 && distance != 4);
		}
}

static void
qr_draw_alignment(QrMatrix *m, gint x, gint y)
{
	gint dx, dy;
	for (dy = -2; dy <= 2; dy++)
		for (dx = -2; dx <= 2; dx++)
			qr_set_function(m, x + dx, y + dy, MAX(ABS(dx), ABS(dy)) != 1);
}

/* Format bits: level M is 00, then the mask, BCH(15,5) protected and
 * XORed with the fixed pattern so no mask yields all zeros. */
static void
qr_draw_format(QrMatrix *m, guint mask)
{
	guint data = (0 << 3) | mask;
	guint remainder = data;
	guint bits;
	gint i, size = (gint)m->size;
	for (i = 0; i < 10; i++)
		remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537);
	bits = ((data << 10) | remainder) ^ 0x5412;
	for (i = 0; i <= 5; i++)
		qr_set_function(m, 8, i, (bits >> i) & 1);
	qr_set_function(m, 8, 7, (bits >> 6) & 1);
	qr_set_function(m, 8, 8, (bits >> 7) & 1);
	qr_set_function(m, 7, 8, (bits >> 8) & 1);
	for (i = 9; i < 15; i++)
		qr_set_function(m, 14 - i, 8, (bits >> i) & 1);
	for (i = 0; i < 8; i++)
		qr_set_function(m, size - 1 - i, 8, (bits >> i) & 1);
	for (i = 8; i < 15; i++)
		qr_set_function(m, 8, size - 15 + i, (bits >> i) & 1);
	/* The module that is always dark. */
	qr_set_function(m, 8, size - 8, TRUE);
}

static void
qr_draw_version(QrMatrix *m, guint version)
{
	guint remainder = version;
	guint bits;
	gint i, size = (gint)m->size;
	if (version < 7)
		return;
	for (i = 0; i < 12; i++)
		remainder = (remainder << 1) ^ ((remainder >> 11) * 0x1F25);
	bits = (version << 12) | remainder;
	for (i = 0; i < 18; i++)
	{
		gboolean dark = (bits >> i) & 1;
		gint a = size - 11 + i % 3;
		gint b = i / 3;
		qr_set_function(m, a, b, dark);
		qr_set_function(m, b, a, dark);
	}
}

static void
qr_draw_function_patterns(QrMatrix *m, guint version)
{
	gint i, j, size = (gint)m->size;
	guint count = 0;
	for (i = 0; i < size; i++)
	{
		qr_set_function(m, 6, i, i % 2 == 0);
		qr_set_function(m, i, 6, i % 2 == 0);
	}
	qr_draw_finder(m, 3, 3);
	qr_draw_finder(m, size - 4, 3);
	qr_draw_finder(m, 3, size - 4);
	while (count < 3 && 0 != qr_alignment[version][count])
		count++;
	for (i = 0; i < (gint)count; i++)
		for (j = 0; j < (gint)count; j++)
		{
			gboolean corner = (i == 0 && j == 0) || (i == 0 && j == (gint)count - 1) || (i == (gint)count - 1 && j == 0);
			if (!corner)
				qr_draw_alignment(m, qr_alignment[version][i], qr_alignment[version][j]);
		}
	qr_draw_format(m, 0);
	qr_draw_version(m, version);
}

/* Data goes in two-module columns, zigzagging up then down from the right
 * edge, skipping the vertical timing column entirely. */
static void
qr_draw_codewords(QrMatrix *m, const guint8 *codewords, guint length)
{
	guint i = 0;
	gint size = (gint)m->size;
	gint right, vertical, j;
	for (right = size - 1; right >= 1; right -= 2)
	{
		if (right == 6)
			right = 5;
		for (vertical = 0; vertical < size; vertical++)
			for (j = 0; j < 2; j++)
			{
				gint x = right - j;
				gboolean upward = ((right + 1) & 2) == 0;
				gint y = upward ? (size - 1 - vertical) : vertical;
				if (!m->function[y * size + x] && i < length * 8)
				{
					m->modules[y * size + x] = (codewords[i >> 3] >> (7 - (i & 7))) & 1;
					i++;
				}
			}
	}
}

static gboolean
qr_mask_bit(guint mask, gint x, gint y)
{
	switch (mask)
	{
	case 0: return (x + y) % 2 == 0;
	case 1: return y % 2 == 0;
	case 2: return x % 3 == 0;
	case 3: return (x + y) % 3 == 0;
	case 4: return (x / 3 + y / 2) % 2 == 0;
	case 5: return x * y % 2 + x * y % 3 == 0;
	case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
	default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
	}
}

static void
qr_apply_mask(QrMatrix *m, guint mask)
{
	gint x, y, size = (gint)m->size;
	for (y = 0; y < size; y++)
		for (x = 0; x < size; x++)
			if (!m->function[y * size + x] && qr_mask_bit(mask, x, y))
				m->modules[y * size + x] ^= 1;
}

/* Adds the finder-like penalty for a run history, ISO 18004 rule 3. */
static guint
qr_finder_penalty(const guint *history)
{
	guint n = history[1];
	gboolean core = n > 0 && history[2] == n && history[3] == n * 3 && history[4] == n && history[5] == n;
	guint penalty = 0;
	if (core && history[0] >= n * 4 && history[6] >= n)
		penalty += 40;
	if (core && history[6] >= n * 4 && history[0] >= n)
		penalty += 40;
	return penalty;
}

static void
qr_push_run(guint *history, guint run)
{
	memmove(history + 1, history, 6 * sizeof(guint));
	history[0] = run;
}

static guint
qr_penalty(const QrMatrix *m)
{
	gint x, y, size = (gint)m->size;
	guint penalty = 0;
	guint dark = 0;
	guint total = (guint)size * size;
	guint k;
	for (y = 0; y < size; y++)
	{
		guint history[7] = { 0 };
		guint run = 0;
		gboolean colour = FALSE;
		for (x = 0; x < size; x++)
		{
			gboolean module = m->modules[y * size + x];
			if (module == colour)
			{
				run++;
				if (run == 5) penalty += 3;
				else if (run > 5) penalty += 1;
			}
			else
			{
				qr_push_run(history, run);
				if (!colour)
					penalty += qr_finder_penalty(history);
				colour = module;
				run = 1;
			}
			if (module) dark++;
		}
		qr_push_run(history, run);
		if (colour)
			qr_push_run(history, 0);
		penalty += qr_finder_penalty(history);
	}
	for (x = 0; x < size; x++)
	{
		guint history[7] = { 0 };
		guint run = 0;
		gboolean colour = FALSE;
		for (y = 0; y < size; y++)
		{
			gboolean module = m->modules[y * size + x];
			if (module == colour)
			{
				run++;
				if (run == 5) penalty += 3;
				else if (run > 5) penalty += 1;
			}
			else
			{
				qr_push_run(history, run);
				if (!colour)
					penalty += qr_finder_penalty(history);
				colour = module;
				run = 1;
			}
		}
		qr_push_run(history, run);
		if (colour)
			qr_push_run(history, 0);
		penalty += qr_finder_penalty(history);
	}
	for (y = 0; y + 1 < size; y++)
		for (x = 0; x + 1 < size; x++)
		{
			guint8 a = m->modules[y * size + x];
			if (a == m->modules[y * size + x + 1] && a == m->modules[(y + 1) * size + x] && a == m->modules[(y + 1) * size + x + 1])
				penalty += 3;
		}
	k = (guint)((ABS((gint)(dark * 20) - (gint)(total * 10)) + (gint)total - 1) / total);
	penalty += (k > 0 ? k - 1 : 0) * 10;
	return penalty;
}

guint8 *
venture_qr_matrix(const gchar *text, guint *out_size, GError **error)
{
	QrMatrix m;
	g_autofree guint8 *codewords = NULL;
	g_autofree guint8 *function = NULL;
	guint version;
	guint mask;
	guint best_mask = 0;
	guint best_penalty = G_MAXUINT;
	gsize length;
	g_return_val_if_fail(NULL != out_size, NULL);
	*out_size = 0;
	if (venture_string_is_empty(text))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Nothing to encode");
		return NULL;
	}
	length = strlen(text);
	for (version = 1; version <= QR_MAX_VERSION; version++)
		if (length <= qr_byte_capacity(version))
			break;
	if (version > QR_MAX_VERSION)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Too long for a QR code: %" G_GSIZE_FORMAT " bytes, at most %u", length, qr_byte_capacity(QR_MAX_VERSION));
		return NULL;
	}
	m.size = 17 + 4 * version;
	m.modules = g_malloc0((gsize)m.size * m.size);
	function = g_malloc0((gsize)m.size * m.size);
	m.function = function;
	qr_draw_function_patterns(&m, version);
	codewords = qr_codewords(text, length, version);
	qr_draw_codewords(&m, codewords, qr_total_codewords[version]);
	for (mask = 0; mask < 8; mask++)
	{
		guint penalty;
		qr_apply_mask(&m, mask);
		qr_draw_format(&m, mask);
		penalty = qr_penalty(&m);
		qr_apply_mask(&m, mask);
		if (penalty < best_penalty)
		{
			best_penalty = penalty;
			best_mask = mask;
		}
	}
	qr_apply_mask(&m, best_mask);
	qr_draw_format(&m, best_mask);
	*out_size = m.size;
	return m.modules;
}

gchar *
venture_qr_svg_render(const gchar *text, guint module_size, GError **error)
{
	g_autofree guint8 *modules = NULL;
	g_autoptr(GString) svg = NULL;
	guint size = 0;
	guint x, y, span;
	modules = venture_qr_matrix(text, &size, error);
	if (NULL == modules)
		return NULL;
	if (0 == module_size)
		module_size = 6;
	span = size + 2 * QR_QUIET_ZONE;
	svg = g_string_new(NULL);
	g_string_append_printf(svg,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %u %u\" width=\"%u\" height=\"%u\" "
		"shape-rendering=\"crispEdges\" role=\"img\" aria-label=\"QR code\">"
		"<rect width=\"%u\" height=\"%u\" fill=\"#ffffff\"/><path fill=\"#000000\" d=\"",
		span, span, span * module_size, span * module_size, span, span);
	for (y = 0; y < size; y++)
		for (x = 0; x < size; x++)
			if (modules[y * size + x])
				g_string_append_printf(svg, "M%u %uh1v1h-1z", x + QR_QUIET_ZONE, y + QR_QUIET_ZONE);
	g_string_append(svg, "\"/></svg>");
	return g_string_free(g_steal_pointer(&svg), FALSE);
}
