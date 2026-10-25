/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <cairo.h>
#include <glib/gstdio.h>
#include "venture-test-util.h"

/* A page deadline shorter than helper startup must not fail identification.
 * The hung page is what that deadline stops. */
static void test_version_probe_outruns_page_deadline(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-ocr-probe-XXXXXX", NULL);
	g_autofree gchar *binary = g_build_filename(directory, "fake-tesseract", NULL);
	g_autofree gchar *png = g_build_filename(directory, "page.png", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOcrEngine) engine = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *contents = NULL, *text = NULL;
	gsize length;
	gint64 start;
	cairo_surface_t *surface;
	g_assert_true(g_file_set_contents(binary,
		"#!/bin/sh\n"
		"if [ \"$1\" = --version ]; then sleep 0.2; printf 'fixture OCR 1\\n'; exit 0; fi\n"
		"while :; do :; done\n", -1, NULL));
	g_assert_cmpint(g_chmod(binary, 0700), ==, 0);
	surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 40, 40);
	g_assert_cmpint(cairo_surface_write_to_png(surface, png), ==, CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(surface);
	engine = venture_ocr_local_new(binary, 50, &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(venture_ocr_engine_describe(engine), "fixture OCR 1"));
	g_assert_true(g_file_get_contents(png, &contents, &length, NULL));
	bytes = g_bytes_new(contents, length);
	start = g_get_monotonic_time();
	text = venture_ocr_engine_extract(engine, bytes, "image/png", 0, "eng", NULL, &error);
	g_assert_null(text);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 5 * G_USEC_PER_SEC);
	venture_test_remove_tree(directory);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ocr-local/version-probe-outruns-page-deadline", test_version_probe_outruns_page_deadline);
	return g_test_run();
}
