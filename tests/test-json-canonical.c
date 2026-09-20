/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* Existing pending accounting approvals must survive helper extraction. */
static void test_legacy_bytes(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) input = json_from_string("{\"z\":[{\"b\":false,\"a\":\"x\\ny\"},null,1],\"a\":2}", &error);
	g_autoptr(GString) material = g_string_new(":invoice:");
	g_autoptr(JsonNode) reordered = json_from_string("{\"a\":2,\"z\":[{\"a\":\"x\\ny\",\"b\":false},null,1]}", &error);
	g_autoptr(GString) other = g_string_new(":invoice:");
	g_assert_no_error(error);
	venture_json_append_canonical(material, input);
	venture_json_append_canonical(other, reordered);
	g_assert_cmpstr(material->str, ==, ":invoice:{\"a\":2,\"z\":[{\"a\":\"x\\ny\",\"b\":false,},null,1,],}");
	g_assert_cmpstr(other->str, ==, material->str);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/json-canonical/legacy-accounting-bytes", test_legacy_bytes);
	return g_test_run();
}
