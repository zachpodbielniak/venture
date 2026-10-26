/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A browser import belongs to the business selected in the sidebar, even
 * when its owner has no membership in the installation's default business. */
static guint
csv_contact_post(ServerFixture *fixture, const gchar *cookie, const gchar *csv, gchar **body)
{
	g_autofree gchar *multipart = g_strdup_printf(
		"--CONTACTCSV\r\nContent-Disposition: form-data; name=\"file\"; filename=\"contacts.csv\"\r\n"
		"Content-Type: text/csv\r\n\r\n%s\r\n--CONTACTCSV--\r\n", csv);
	g_autoptr(GBytes) bytes = g_bytes_new(multipart, strlen(multipart));
	return server_fixture_post_raw(fixture, "/e/contact/import", cookie,
		"multipart/form-data; boundary=CONTACTCSV", bytes, body);
}

static void
test_auth_csv_selected_business(ServerFixture *fixture, gconstpointer unused)
{
	g_autoptr(VentureEntity) business = g_object_new(VENTURE_TYPE_ORGANIZATION,
		"name", "Cedar Fixture", "slug", "cedar-fixture", NULL);
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GPtrArray) contacts = NULL;
	g_autofree gchar *session = NULL, *cookie = NULL, *csv = NULL, *body = NULL;
	gint64 user, selected, home = venture_context_get_default_organization_id(fixture->context);
	guint i;
	(void)unused;
	g_assert_true(venture_database_save(fixture->database, business, NULL, NULL));
	selected = venture_entity_get_id(business);
	server_fixture_create_user(fixture, "csv-owner", "fixture-password", VENTURE_USER_ROLE_EDITOR, &user);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", user,
		"organization-id", selected, "role", VENTURE_ORGANIZATION_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, member, NULL, NULL));
	session = server_fixture_login(fixture, "csv-owner", "fixture-password");
	cookie = g_strdup_printf("%s; venture_entity=%" G_GINT64_FORMAT, session, selected);
	g_assert_cmpuint(server_fixture_request(fixture, "GET", "/e/contact/import", cookie, NULL, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "multipart/form-data"));
	g_clear_pointer(&body, g_free);
	/* A CSV organization column cannot redirect the import to another business. */
	csv = g_strdup_printf("name,email,notes,organization_id\r\n"
		"Cedar Customer,cedar@example.test,First fixture,%" G_GINT64_FORMAT "\r\n"
		"Maple Customer,maple@example.test,Second fixture,%" G_GINT64_FORMAT "\r\n", home, home);
	g_assert_cmpuint(csv_contact_post(fixture, cookie, csv, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Imported 2 records"));
	contacts = venture_database_find(fixture->database, query, NULL);
	g_assert_nonnull(contacts);
	g_assert_cmpuint(contacts->len, ==, 2);
	for (i = 0; i < contacts->len; i++)
		g_assert_cmpint(venture_entity_get_organization_id(g_ptr_array_index(contacts, i)), ==, selected);
	g_clear_pointer(&body, g_free);
	/* Validation failure rolls back the good row alongside it. */
	g_assert_cmpuint(csv_contact_post(fixture, cookie,
		"name,email\r\nBirch Customer,birch@example.test\r\n,missing-name@example.test\r\n", &body), ==, 422);
	g_assert_nonnull(strstr(body, "Nothing was imported"));
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL), ==, 2);
	g_clear_pointer(&body, g_free);
	/* Selecting a business the user does not belong to still cannot write. */
	g_clear_pointer(&cookie, g_free);
	cookie = g_strdup_printf("%s; venture_entity=%" G_GINT64_FORMAT, session, home);
	g_assert_cmpuint(csv_contact_post(fixture, cookie, csv, &body), ==, 404);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL), ==, 2);
}
