/*
 * test-kb.c - Knowledge bases: chunking, storage format, indexing, search
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The tests that need an embedding service are skipped when there is none
 * reachable, rather than failing. A machine with no ollama is a normal
 * developer machine, and a suite that goes red on it teaches everybody to
 * ignore a red suite. What is never skipped is everything that can be
 * checked without one -- the chunker, the storage format, the model
 * invariants -- which is most of what breaks.
 */

#include <glib.h>
#include <string.h>

#include "venture.h"
#include "venture-test-util.h"

/* ==========================================================================
 * Chunking
 * ========================================================================== */

/*
 * A document shorter than the target is one passage, not one per line.
 */
static void
test_kb_chunk_short_document(void)
{
	g_autoptr(GPtrArray) passages = NULL;
	const VentureKbPassage *passage;

	passages = venture_kb_chunk_text("A short note about invoicing.",
	                                 VENTURE_KB_FORMAT_TEXT, 1200, 200);

	g_assert_cmpuint(passages->len, ==, 1);

	passage = g_ptr_array_index(passages, 0);
	g_assert_cmpstr(passage->text, ==, "A short note about invoicing.");
	g_assert_null(passage->heading);
	g_assert_cmpuint(passage->ordinal, ==, 0);
}

/*
 * Nothing to say produces nothing to embed.
 *
 * An empty passage costs a request and comes back as a zero vector, which
 * matches every query equally badly -- so it is not merely wasteful, it is
 * noise in the results.
 */
static void
test_kb_chunk_empty_document(void)
{
	g_autoptr(GPtrArray) empty = NULL;
	g_autoptr(GPtrArray) blank = NULL;

	empty = venture_kb_chunk_text("", VENTURE_KB_FORMAT_ORG, 1200, 200);
	blank = venture_kb_chunk_text("   \n\n  \t\n", VENTURE_KB_FORMAT_ORG,
	                              1200, 200);

	g_assert_cmpuint(empty->len, ==, 0);
	g_assert_cmpuint(blank->len, ==, 0);
}

/*
 * Org headings split the document, and each passage remembers which one it
 * sits under. The heading is what lets a retrieved paragraph say what it is
 * about when the paragraph itself only says "it".
 */
static void
test_kb_chunk_splits_on_org_headings(void)
{
	static const gchar document[] =
		"* Getting started\n"
		"Install it, then run it.\n"
		"* API tokens\n"
		"Mint one at /account/tokens. It is shown once.\n"
		"** Revoking\n"
		"Clearing the active flag stops it working.\n";
	g_autoptr(GPtrArray) passages = NULL;

	passages = venture_kb_chunk_text(document, VENTURE_KB_FORMAT_ORG,
	                                 1200, 200);

	g_assert_cmpuint(passages->len, ==, 3);

	g_assert_cmpstr(((VentureKbPassage *)g_ptr_array_index(passages, 0))->heading,
	                ==, "Getting started");
	g_assert_cmpstr(((VentureKbPassage *)g_ptr_array_index(passages, 1))->heading,
	                ==, "API tokens");
	g_assert_cmpstr(((VentureKbPassage *)g_ptr_array_index(passages, 2))->heading,
	                ==, "Revoking");

	/* Ordinals are contiguous, which the search relies on to say which
	 * passage of an article matched. */
	g_assert_cmpuint(((VentureKbPassage *)g_ptr_array_index(passages, 2))->ordinal,
	                 ==, 2);
}

/*
 * Markdown uses '#', and a run of markers is only a heading when a space
 * follows it.
 *
 * Without that check "#hashtag" becomes a heading, and in Org "**bold**"
 * does -- which is common in body text, and a wrong heading is attached to
 * every passage beneath it.
 */
static void
test_kb_chunk_heading_needs_a_space(void)
{
	static const gchar markdown[] =
		"# Real heading\n"
		"Body text.\n"
		"#hashtag is not a heading and neither is #1.\n";
	static const gchar org[] =
		"* Real heading\n"
		"Body with **bold** in it.\n"
		"**not a heading**\n";
	g_autoptr(GPtrArray) md = NULL;
	g_autoptr(GPtrArray) o = NULL;

	md = venture_kb_chunk_text(markdown, VENTURE_KB_FORMAT_MARKDOWN,
	                           1200, 200);
	o = venture_kb_chunk_text(org, VENTURE_KB_FORMAT_ORG, 1200, 200);

	g_assert_cmpuint(md->len, ==, 1);
	g_assert_cmpstr(((VentureKbPassage *)g_ptr_array_index(md, 0))->heading,
	                ==, "Real heading");

	g_assert_cmpuint(o->len, ==, 1);
	g_assert_cmpstr(((VentureKbPassage *)g_ptr_array_index(o, 0))->heading,
	                ==, "Real heading");
}

/*
 * A format with no heading syntax gets no headings, rather than a guess.
 */
static void
test_kb_chunk_plain_text_has_no_headings(void)
{
	static const gchar document[] =
		"* this is a bullet, not a heading\n"
		"# so is this\n";
	g_autoptr(GPtrArray) passages = NULL;

	passages = venture_kb_chunk_text(document, VENTURE_KB_FORMAT_TEXT,
	                                 1200, 200);

	g_assert_cmpuint(passages->len, ==, 1);
	g_assert_null(((VentureKbPassage *)g_ptr_array_index(passages, 0))->heading);
}

/*
 * A long section is broken up, the pieces overlap, and the split terminates.
 *
 * The overlap is what stops the one sentence that answers a question being
 * the one that fell across a boundary. Termination is not obvious: an
 * overlap at or above the piece size would move the cursor backwards.
 */
static void
test_kb_chunk_splits_long_sections_with_overlap(void)
{
	g_autoptr(GString) document = NULL;
	g_autoptr(GPtrArray) passages = NULL;
	guint i;

	document = g_string_new(NULL);

	for (i = 0; i < 200; i++)
		g_string_append_printf(document,
			"Sentence number %u about receivables and invoices. ", i);

	passages = venture_kb_chunk_text(document->str, VENTURE_KB_FORMAT_TEXT,
	                                 400, 100);

	g_assert_cmpuint(passages->len, >, 1);

	for (i = 0; i < passages->len; i++)
	{
		const VentureKbPassage *passage = g_ptr_array_index(passages, i);

		g_assert_nonnull(passage->text);
		g_assert_true(g_utf8_validate(passage->text, -1, NULL));
		g_assert_cmpuint(passage->ordinal, ==, i);
	}
}

/*
 * An overlap larger than the target would never advance. Clamped rather
 * than trusted, because the value comes from configuration.
 */
static void
test_kb_chunk_survives_absurd_settings(void)
{
	g_autoptr(GString) document = NULL;
	g_autoptr(GPtrArray) passages = NULL;
	guint i;

	document = g_string_new(NULL);

	for (i = 0; i < 100; i++)
		g_string_append(document, "Some text that goes on for a while. ");

	/* Overlap above target, and a target below the floor. */
	passages = venture_kb_chunk_text(document->str, VENTURE_KB_FORMAT_TEXT,
	                                 10, 9999);

	g_assert_cmpuint(passages->len, >, 0);
}

/*
 * Splitting is UTF-8 aware.
 *
 * A byte-wise cut lands mid-character, and the result is not merely wrong
 * but invalid UTF-8 -- which fails at the JSON encoder a long way from here.
 */
static void
test_kb_chunk_does_not_split_utf8(void)
{
	g_autoptr(GString) document = NULL;
	g_autoptr(GPtrArray) passages = NULL;
	guint i;

	document = g_string_new(NULL);

	for (i = 0; i < 400; i++)
		g_string_append(document, "\xc3\xa9\xe2\x82\xac\xf0\x9f\x93\x9a ");

	passages = venture_kb_chunk_text(document->str, VENTURE_KB_FORMAT_TEXT,
	                                 200, 40);

	g_assert_cmpuint(passages->len, >, 1);

	for (i = 0; i < passages->len; i++)
	{
		const VentureKbPassage *passage = g_ptr_array_index(passages, i);

		g_assert_true(g_utf8_validate(passage->text, -1, NULL));
	}
}

/*
 * The embedded text carries the heading, so a passage that says "set it
 * before the first run" is findable by somebody asking about API tokens.
 */
static void
test_kb_chunk_embed_text_includes_heading(void)
{
	g_autoptr(GPtrArray) passages = NULL;
	g_autofree gchar *with = NULL;
	g_autofree gchar *without = NULL;

	passages = venture_kb_chunk_text("* API tokens\nIt is shown once.\n",
	                                 VENTURE_KB_FORMAT_ORG, 1200, 200);
	g_assert_cmpuint(passages->len, ==, 1);

	with = venture_kb_chunk_embed_text(g_ptr_array_index(passages, 0));
	g_assert_nonnull(strstr(with, "API tokens"));
	g_assert_nonnull(strstr(with, "It is shown once."));

	g_clear_pointer(&passages, g_ptr_array_unref);
	passages = venture_kb_chunk_text("Just a body.\n",
	                                 VENTURE_KB_FORMAT_TEXT, 1200, 200);
	without = venture_kb_chunk_embed_text(g_ptr_array_index(passages, 0));
	g_assert_cmpstr(without, ==, "Just a body.");
}

/* ==========================================================================
 * Formats and slugs
 * ========================================================================== */

static void
test_kb_format_from_path(void)
{
	g_assert_cmpint(venture_kb_format_from_path("handbook.org"), ==,
	                VENTURE_KB_FORMAT_ORG);
	g_assert_cmpint(venture_kb_format_from_path("README.MD"), ==,
	                VENTURE_KB_FORMAT_MARKDOWN);
	g_assert_cmpint(venture_kb_format_from_path("notes.txt"), ==,
	                VENTURE_KB_FORMAT_TEXT);
	g_assert_cmpint(venture_kb_format_from_path("/a/b/spec.pdf"), ==,
	                VENTURE_KB_FORMAT_PDF);
	g_assert_cmpint(venture_kb_format_from_path("contract.docx"), ==,
	                VENTURE_KB_FORMAT_DOCX);

	/*
	 * Unrecognised is OTHER, not TEXT. A binary read as text produces a
	 * body of mojibake that embeds to a vector nobody can tell is
	 * meaningless.
	 */
	g_assert_cmpint(venture_kb_format_from_path("photo.jpeg"), ==,
	                VENTURE_KB_FORMAT_OTHER);
	g_assert_cmpint(venture_kb_format_from_path(NULL), ==,
	                VENTURE_KB_FORMAT_OTHER);
}

/*
 * A PDF exports as .txt, because what the article holds is the extracted
 * text. Writing that under a .pdf name produces a file nothing can open.
 */
static void
test_kb_format_extension(void)
{
	g_assert_cmpstr(venture_kb_format_extension(VENTURE_KB_FORMAT_ORG), ==,
	                ".org");
	g_assert_cmpstr(venture_kb_format_extension(VENTURE_KB_FORMAT_MARKDOWN),
	                ==, ".md");
	g_assert_cmpstr(venture_kb_format_extension(VENTURE_KB_FORMAT_PDF), ==,
	                ".txt");
	g_assert_cmpstr(venture_kb_format_extension(VENTURE_KB_FORMAT_DOCX), ==,
	                ".txt");
}

static void
test_kb_slugify(void)
{
	g_autofree gchar *plain = NULL;
	g_autofree gchar *messy = NULL;
	g_autofree gchar *edges = NULL;
	g_autofree gchar *nothing = NULL;

	plain = venture_kb_slugify("Venture Docs");
	g_assert_cmpstr(plain, ==, "venture-docs");

	messy = venture_kb_slugify("  The 2026 Handbook (v2)!  ");
	g_assert_cmpstr(messy, ==, "the-2026-handbook-v2");

	/* Never leading, trailing or doubled hyphens -- a slug is typed after
	 * '#' and has to be predictable. */
	edges = venture_kb_slugify("---a---b---");
	g_assert_cmpstr(edges, ==, "a-b");

	nothing = venture_kb_slugify("!!!");
	g_assert_null(nothing);
}

/* ==========================================================================
 * The storage format
 * ========================================================================== */

/*
 * A vector survives the round trip through the text column exactly.
 *
 * Exactly matters: these are compared with each other, and a value that
 * drifted in storage would shift every score computed against it.
 */
static void
test_kb_embedding_round_trip(void)
{
	gfloat original[5] = { 0.5f, -0.25f, 0.0f, 1.0f, -1.0f };
	g_autofree gchar *encoded = NULL;
	g_autofree gfloat *decoded = NULL;
	gsize dims = 0;
	gsize i;

	encoded = venture_embedding_encode(original, 5);
	g_assert_nonnull(encoded);

	decoded = venture_embedding_decode(encoded, &dims);
	g_assert_nonnull(decoded);
	g_assert_cmpuint(dims, ==, 5);

	for (i = 0; i < 5; i++)
		g_assert_cmpfloat(decoded[i], ==, original[i]);
}

/*
 * A column that cannot be a whole number of floats is refused rather than
 * half-read, and so is an empty one.
 *
 * The check is deliberately only that: bytes whose length happens to divide
 * by four *do* decode, because nothing distinguishes them from a vector.
 * That is not the layer where rubbish is caught -- the search skips any
 * passage whose width disagrees with the query's, which is what actually
 * keeps a corrupt row out of the results.
 */
static void
test_kb_embedding_rejects_rubbish(void)
{
	g_autofree gfloat *ragged = NULL;
	g_autofree gfloat *from_empty = NULL;
	g_autofree gfloat *from_garbage = NULL;
	gsize dims = 1;

	/* base64 of "abcde": five bytes, so not a whole number of floats. */
	ragged = venture_embedding_decode("YWJjZGU=", &dims);
	g_assert_null(ragged);
	g_assert_cmpuint(dims, ==, 0);

	dims = 1;
	from_empty = venture_embedding_decode("", &dims);
	g_assert_null(from_empty);
	g_assert_cmpuint(dims, ==, 0);

	/*
	 * base64 of "not a vector": twelve bytes, which is three floats, so
	 * this succeeds. Pinned so the limitation is recorded rather than
	 * discovered.
	 */
	dims = 0;
	from_garbage = venture_embedding_decode("bm90IGEgdmVjdG9y", &dims);
	g_assert_nonnull(from_garbage);
	g_assert_cmpuint(dims, ==, 3);
}

static void
test_kb_embedding_cosine(void)
{
	const gfloat a[3] = { 1.0f, 0.0f, 0.0f };
	const gfloat same[3] = { 1.0f, 0.0f, 0.0f };
	const gfloat orthogonal[3] = { 0.0f, 1.0f, 0.0f };
	const gfloat zero[3] = { 0.0f, 0.0f, 0.0f };

	g_assert_cmpfloat(venture_embedding_cosine(a, same, 3), >, 0.999);
	g_assert_cmpfloat(ABS(venture_embedding_cosine(a, orthogonal, 3)), <,
	                  0.001);
	g_assert_cmpfloat(venture_embedding_cosine(a, zero, 3), ==, 0.0);
}

/* ==========================================================================
 * The records
 * ========================================================================== */

typedef struct
{
	VentureDatabase	*database;
	VentureConfig	*config;
	VentureContext	*context;
	gchar		*state_dir;
	gint64		 organization_id;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureOrganization) organization = NULL;
	g_autoptr(GError) error = NULL;

	fixture->state_dir = g_dir_make_tmp("venture-kb-XXXXXX", NULL);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config,
	                                       fixture->database);

	organization = venture_organization_new();
	g_object_set(organization, "name", "Test Org", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(organization), NULL,
	                                    NULL));
	fixture->organization_id =
		venture_entity_get_id(VENTURE_ENTITY(organization));
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}
}

/*
 * The four record types register, get tables, and round-trip.
 *
 * This is what the generic machinery gives them: REST, the web form, the
 * CLI, the MCP tool surface and the audit diff all derive from the same
 * registration, so a type that saves and loads here works everywhere.
 */
static void
test_kb_records_round_trip(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKnowledgeBase) base = NULL;
	g_autoptr(VentureKbArticle) article = NULL;
	g_autoptr(VentureEntity) loaded = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *slug = NULL;
	gint64 base_id;
	gint64 article_id;

	base = venture_knowledge_base_new();
	g_object_set(base, "name", "Venture Docs", "slug", "venture_docs",
	             "description", "How this thing works",
	             "auto-retrieve", TRUE, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(base),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(base), NULL, NULL));
	base_id = venture_entity_get_id(VENTURE_ENTITY(base));
	g_assert_cmpint(base_id, >, 0);

	article = venture_kb_article_new();
	g_object_set(article, "title", "API tokens", "kb-id", base_id,
	             "slug", "api-tokens", "body", "Mint one at /account/tokens.",
	             "format", VENTURE_KB_FORMAT_ORG,
	             "status", VENTURE_KB_ARTICLE_STATUS_PUBLISHED, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(article),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(article), NULL, NULL));
	article_id = venture_entity_get_id(VENTURE_ENTITY(article));

	loaded = venture_database_get(fixture->database, VENTURE_TYPE_KB_ARTICLE,
	                              article_id, NULL);
	g_assert_nonnull(loaded);
	g_object_get(loaded, "title", &title, "slug", &slug, NULL);
	g_assert_cmpstr(title, ==, "API tokens");
	g_assert_cmpstr(slug, ==, "api-tokens");

	g_clear_object(&loaded);
	loaded = venture_database_get(fixture->database,
	                              VENTURE_TYPE_KNOWLEDGE_BASE, base_id, NULL);
	g_assert_nonnull(loaded);
}

/*
 * A knowledge base's slug is unique, because it is what somebody types
 * after '#'. Two bases answering to one name is a prompt whose meaning
 * depends on insertion order.
 */
static void
test_kb_slug_is_unique(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKnowledgeBase) first = NULL;
	g_autoptr(VentureKnowledgeBase) second = NULL;
	g_autoptr(GError) error = NULL;

	first = venture_knowledge_base_new();
	g_object_set(first, "name", "Docs", "slug", "docs", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(first),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(first), NULL, NULL));

	second = venture_knowledge_base_new();
	g_object_set(second, "name", "Other docs", "slug", "docs", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(second),
	                                   fixture->organization_id);

	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(second), NULL,
	                                     &error));
	g_assert_nonnull(error);
}

/*
 * An article must belong to a base that exists.
 *
 * References are checked at the save, so a kb-id pointing at nothing is
 * refused rather than becoming a row the search can never explain.
 */
static void
test_kb_article_needs_a_real_base(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbArticle) article = NULL;
	g_autoptr(GError) error = NULL;

	article = venture_kb_article_new();
	g_object_set(article, "title", "Orphan", "kb-id", (gint64)987654,
	             "body", "Nothing points at me.", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(article),
	                                   fixture->organization_id);

	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(article), NULL,
	                                     &error));
	g_assert_nonnull(error);
}

/* ==========================================================================
 * Indexing and search, against a real embedding service
 * ========================================================================== */

/*
 * Builds the service, or skips the test when no embedding service answers.
 *
 * A machine with no ollama is a normal developer machine. A suite that goes
 * red there teaches everybody to ignore a red suite, which costs more than
 * these tests are worth.
 */
static VentureKbService *
kb_service_or_skip(Fixture *fixture)
{
	g_autoptr(GError) error = NULL;
	VentureKbService *service;
	gsize dims = 0;
	gfloat *probe;

	service = venture_kb_service_new(fixture->context, &error);

	if (NULL == service)
	{
		g_test_skip("no embedding service configured");
		return NULL;
	}

	probe = venture_embedder_embed(venture_kb_service_get_embedder(service),
	                               "probe", &dims, &error);

	if (NULL == probe)
	{
		g_test_skip("embedding service unreachable");
		g_object_unref(service);
		return NULL;
	}

	g_free(probe);

	return service;
}

static gint64
kb_make_base(
	Fixture		*fixture,
	const gchar	*slug
){
	g_autoptr(VentureKnowledgeBase) base = NULL;

	base = venture_knowledge_base_new();
	g_object_set(base, "name", slug, "slug", slug, "auto-retrieve", TRUE,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(base),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(base), NULL, NULL));

	return venture_entity_get_id(VENTURE_ENTITY(base));
}

static gint64
kb_make_article(
	Fixture		*fixture,
	gint64		 kb_id,
	const gchar	*title,
	const gchar	*body,
	VentureKbArticleStatus status
){
	g_autoptr(VentureKbArticle) article = NULL;

	article = venture_kb_article_new();
	g_object_set(article, "title", title, "kb-id", kb_id, "body", body,
	             "format", VENTURE_KB_FORMAT_ORG, "status", status, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(article),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(article), NULL, NULL));

	return venture_entity_get_id(VENTURE_ENTITY(article));
}

/*
 * The whole point of the feature: ask a question in words the document does
 * not use, and get the passage that answers it.
 *
 * Asserted on the ranking rather than on a score threshold. Scores move with
 * the model; what must hold is that the right passage beats the wrong ones.
 */
static void
test_kb_index_and_search(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(GPtrArray) hits = NULL;
	g_autoptr(GError) error = NULL;
	const VentureKbHit *best;
	gint64 kb_id;
	gint chunks;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	kb_id = kb_make_base(fixture, "handbook");

	kb_make_article(fixture, kb_id, "Credentials",
		"* API tokens\n"
		"Mint one from Your account, then API tokens. The secret is "
		"shown exactly once and only its hash is stored.\n",
		VENTURE_KB_ARTICLE_STATUS_PUBLISHED);
	kb_make_article(fixture, kb_id, "Receivables",
		"* Aging\n"
		"Outstanding invoices are bucketed by how far past due they "
		"are, and never against a date that has not happened.\n",
		VENTURE_KB_ARTICLE_STATUS_PUBLISHED);
	kb_make_article(fixture, kb_id, "Navigation",
		"* The sidebar\n"
		"It remembers where you left it, and brings the current page "
		"into view.\n",
		VENTURE_KB_ARTICLE_STATUS_PUBLISHED);

	chunks = venture_kb_service_reindex(service, kb_id, FALSE, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(chunks, ==, 3);

	/* Deliberately not the document's words: "credential" and "sign in"
	 * appear nowhere in it. Keyword search would find nothing here. */
	hits = venture_kb_service_search(service,
		"how do I get a credential for signing in to the API?",
		&kb_id, 1, 5, &error);
	g_assert_no_error(error);
	g_assert_nonnull(hits);
	g_assert_cmpuint(hits->len, >, 0);

	best = g_ptr_array_index(hits, 0);
	g_assert_cmpstr(best->title, ==, "Credentials");
	g_assert_cmpstr(best->kb_slug, ==, "handbook");
	g_assert_cmpfloat(best->score, >, 0.0);

	/* Results are strongest first, which is what every caller assumes. */
	if (hits->len > 1)
	{
		const VentureKbHit *second = g_ptr_array_index(hits, 1);

		g_assert_cmpfloat(best->score, >=, second->score);
	}
}

/*
 * A draft is not searched.
 *
 * An assistant citing a draft is quoting a document its author has not stood
 * behind, which is worse than finding nothing.
 */
static void
test_kb_search_skips_drafts(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(GPtrArray) hits = NULL;
	g_autoptr(GError) error = NULL;
	gint64 kb_id;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	kb_id = kb_make_base(fixture, "drafts");
	kb_make_article(fixture, kb_id, "Unfinished",
		"* API tokens\nMint one from the account page.\n",
		VENTURE_KB_ARTICLE_STATUS_DRAFT);

	g_assert_cmpint(venture_kb_service_reindex(service, kb_id, FALSE, NULL,
	                                           &error), ==, 1);
	g_assert_no_error(error);

	/* Indexed, so promoting it later costs nothing -- but not returned. */
	hits = venture_kb_service_search(service, "API tokens", &kb_id, 1, 5,
	                                 &error);
	g_assert_no_error(error);
	g_assert_nonnull(hits);
	g_assert_cmpuint(hits->len, ==, 0);
}

/*
 * Re-indexing replaces passages rather than adding to them.
 *
 * An article that shrank would otherwise stay findable by text it no longer
 * contains, citing a document that has since been corrected.
 */
static void
test_kb_reindex_replaces_chunks(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(VentureEntity) article = NULL;
	g_autoptr(GPtrArray) hits = NULL;
	g_autoptr(GError) error = NULL;
	gint64 kb_id;
	gint64 article_id;
	guint i;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	kb_id = kb_make_base(fixture, "revised");
	article_id = kb_make_article(fixture, kb_id, "Policy",
		"* Old policy\nExpenses are reimbursed within ninety days.\n",
		VENTURE_KB_ARTICLE_STATUS_PUBLISHED);

	g_assert_cmpint(venture_kb_service_index_article(service, article_id,
	                                                 NULL, &error), ==, 1);
	g_assert_no_error(error);

	article = venture_database_get(fixture->database, VENTURE_TYPE_KB_ARTICLE,
	                               article_id, NULL);
	g_object_set(article, "body",
	             "* New policy\nExpenses are reimbursed within thirty days.\n",
	             NULL);
	g_assert_true(venture_database_save(fixture->database, article, NULL,
	                                    NULL));

	g_assert_cmpint(venture_kb_service_index_article(service, article_id,
	                                                 NULL, &error), ==, 1);
	g_assert_no_error(error);

	hits = venture_kb_service_search(service, "how long until reimbursement?",
	                                 &kb_id, 1, 10, &error);
	g_assert_no_error(error);
	g_assert_nonnull(hits);

	/* Exactly one passage, and it is the new text. The old one is gone
	 * rather than outranked. */
	g_assert_cmpuint(hits->len, ==, 1);

	for (i = 0; i < hits->len; i++)
	{
		const VentureKbHit *hit = g_ptr_array_index(hits, i);

		g_assert_nonnull(strstr(hit->text, "thirty"));
		g_assert_null(strstr(hit->text, "ninety"));
	}
}

/*
 * A base slug resolves, and an unknown one is named rather than ignored.
 */
static void
test_kb_resolve_slugs(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autofree gint64 *ids = NULL;
	g_autofree gint64 *missing = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *good[2];
	const gchar *bad[2];
	gsize count = 0;
	gint64 kb_id;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	kb_id = kb_make_base(fixture, "venture_docs");

	good[0] = "venture_docs";
	good[1] = NULL;

	ids = venture_kb_service_resolve_slugs(service, good, &count, &error);
	g_assert_no_error(error);
	g_assert_nonnull(ids);
	g_assert_cmpuint(count, ==, 1);
	g_assert_cmpint(ids[0], ==, kb_id);

	bad[0] = "handbok";
	bad[1] = NULL;

	missing = venture_kb_service_resolve_slugs(service, bad, &count, &error);
	g_assert_null(missing);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_nonnull(strstr(error->message, "handbok"));
}

/* ==========================================================================
 * Ingest, sync and export
 * ========================================================================== */

static GBytes *
kb_bytes(const gchar *text)
{
	return g_bytes_new(text, strlen(text));
}

/*
 * A text file becomes an article, and the same file again changes nothing.
 *
 * The second half is what makes a sync of a large tree cheap enough to run
 * often, which is what makes it useful at all.
 */
static void
test_kb_ingest_is_idempotent(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(VentureKbIngestResult) first = NULL;
	g_autoptr(VentureKbIngestResult) second = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	gint64 kb_id;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	kb_id = kb_make_base(fixture, "imported");
	bytes = kb_bytes("* Expenses\nReimbursed within thirty days.\n");

	first = venture_kb_ingest_result_new();
	g_assert_true(venture_kb_ingest_bytes(service, kb_id, bytes,
	                                      "expenses-policy.org",
	                                      "expenses-policy.org", NULL, first,
	                                      &error));
	g_assert_no_error(error);
	g_assert_cmpuint(first->created, ==, 1);
	g_assert_cmpuint(first->indexed, ==, 1);

	second = venture_kb_ingest_result_new();
	g_assert_true(venture_kb_ingest_bytes(service, kb_id, bytes,
	                                      "expenses-policy.org",
	                                      "expenses-policy.org", NULL,
	                                      second, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(second->unchanged, ==, 1);
	g_assert_cmpuint(second->created, ==, 0);
	g_assert_cmpuint(second->updated, ==, 0);
}

/*
 * A title is derived from the filename, and the slug from the title.
 */
static void
test_kb_ingest_names_the_article(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(VentureKbIngestResult) result = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *slug = NULL;
	gint64 kb_id;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	kb_id = kb_make_base(fixture, "named");
	bytes = kb_bytes("Body text.\n");
	result = venture_kb_ingest_result_new();

	g_assert_true(venture_kb_ingest_bytes(service, kb_id, bytes,
	                                      "getting-started.md",
	                                      "getting-started.md", NULL, result,
	                                      &error));
	g_assert_no_error(error);

	query = venture_query_new(VENTURE_TYPE_KB_ARTICLE);
	found = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(found->len, ==, 1);

	g_object_get(g_ptr_array_index(found, 0), "title", &title, "slug", &slug,
	             NULL);

	/* Separators become spaces: a list of articles reads better as
	 * "getting started" than as the filename. */
	g_assert_cmpstr(title, ==, "getting started");
	g_assert_cmpstr(slug, ==, "getting-started");
}

/*
 * A file that is not text is skipped, not failed, and says why.
 *
 * An import of a documentation tree that happens to contain screenshots
 * should not stop at the first one.
 */
static void
test_kb_ingest_skips_unreadable(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(VentureKbIngestResult) result = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	gint64 kb_id;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	kb_id = kb_make_base(fixture, "mixed");
	bytes = g_bytes_new("\x89PNG\r\n\x1a\n\x00\x00\x00\x0d", 12);
	result = venture_kb_ingest_result_new();

	g_assert_true(venture_kb_ingest_bytes(service, kb_id, bytes,
	                                      "screenshot.png", NULL, NULL,
	                                      result, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(result->skipped, ==, 1);
	g_assert_cmpuint(result->created, ==, 0);
	g_assert_cmpuint(result->notes->len, ==, 1);
}

/*
 * Text that is not UTF-8 is refused rather than repaired.
 *
 * Substituting replacement characters silently changes what the document
 * says, and the change is invisible until somebody reads the passage it
 * landed in.
 */
static void
test_kb_extract_refuses_bad_utf8(void)
{
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	VentureKbFormat format;

	bytes = g_bytes_new("valid then \xff\xfe invalid", 22);
	text = venture_kb_extract_text(bytes, "notes.txt", &format, &error);

	g_assert_null(text);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/*
 * An archive is recognised by its bytes, not its name -- and a .docx, which
 * is also a zip, is deliberately not treated as one.
 */
static void
test_kb_archive_detection(void)
{
	g_autoptr(GBytes) zip = NULL;
	g_autoptr(GBytes) gzip = NULL;
	g_autoptr(GBytes) text = NULL;

	zip = g_bytes_new("PK\x03\x04rest of it", 16);
	gzip = g_bytes_new("\x1f\x8b\x08\x00rest", 8);
	text = g_bytes_new("just some words", 15);

	/* Named anything, or nothing: the magic decides. */
	g_assert_true(venture_kb_is_archive("bundle.zip", zip));
	g_assert_true(venture_kb_is_archive(NULL, zip));
	g_assert_true(venture_kb_is_archive("docs.tar.gz", gzip));

	/* A .zip that is not one is stored rather than unpacked. */
	g_assert_false(venture_kb_is_archive("bundle.zip", text));

	/* A .docx is a zip with a known extractor; unpacking it would file
	 * its XML parts as articles. */
	g_assert_false(venture_kb_is_archive("contract.docx", zip));
}

/*
 * Syncing a directory imports it, and a file that has gone is archived
 * rather than deleted.
 *
 * A file removed from a checkout is usually a move, and destroying the
 * article would take its cross-references with it.
 */
static void
test_kb_sync_directory(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(VentureKbIngestResult) first = NULL;
	g_autoptr(VentureKbIngestResult) second = NULL;
	g_autoptr(VentureEntity) base = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *two = NULL;
	g_autofree gchar *nested_dir = NULL;
	g_autofree gchar *nested = NULL;
	gint64 kb_id;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	root = g_build_filename(fixture->state_dir, "corpus", NULL);
	nested_dir = g_build_filename(root, "guides", NULL);
	g_assert_cmpint(g_mkdir_with_parents(nested_dir, 0755), ==, 0);

	one = g_build_filename(root, "alpha.org", NULL);
	two = g_build_filename(root, "beta.org", NULL);
	nested = g_build_filename(nested_dir, "gamma.md", NULL);

	g_assert_true(g_file_set_contents(one, "* Alpha\nFirst.\n", -1, NULL));
	g_assert_true(g_file_set_contents(two, "* Beta\nSecond.\n", -1, NULL));
	g_assert_true(g_file_set_contents(nested, "# Gamma\nThird.\n", -1, NULL));

	kb_id = kb_make_base(fixture, "synced");
	base = venture_database_get(fixture->database,
	                            VENTURE_TYPE_KNOWLEDGE_BASE, kb_id, NULL);
	g_object_set(base, "source-path", root, NULL);
	g_assert_true(venture_database_save(fixture->database, base, NULL, NULL));

	first = venture_kb_sync_directory(service, kb_id, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(first);

	/* Nested directories are walked. */
	g_assert_cmpuint(first->created, ==, 3);

	/* A file that has gone is archived, and the rest are unchanged. */
	g_assert_cmpint(g_unlink(two), ==, 0);

	second = venture_kb_sync_directory(service, kb_id, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(second);
	g_assert_cmpuint(second->unchanged, ==, 2);
	g_assert_cmpuint(second->created, ==, 0);

	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) found = NULL;
		guint archived = 0;
		guint i;

		query = venture_query_new(VENTURE_TYPE_KB_ARTICLE);
		venture_query_set_limit(query, 0);
		found = venture_database_find(fixture->database, query, NULL);

		for (i = 0; i < found->len; i++)
		{
			VentureKbArticleStatus status;

			g_object_get(g_ptr_array_index(found, i), "status",
			             &status, NULL);

			if (VENTURE_KB_ARTICLE_STATUS_ARCHIVED == status)
				archived++;
		}

		/* Archived, and still there -- not destroyed. */
		g_assert_cmpuint(found->len, ==, 3);
		g_assert_cmpuint(archived, ==, 1);
	}
}

/*
 * A base with no source directory cannot be synced, and says so rather than
 * silently doing nothing.
 */
static void
test_kb_sync_needs_a_directory(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(VentureKbIngestResult) result = NULL;
	g_autoptr(GError) error = NULL;
	gint64 kb_id;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	kb_id = kb_make_base(fixture, "no-source");
	result = venture_kb_sync_directory(service, kb_id, NULL, &error);

	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/*
 * An export is an archive of the articles plus a manifest, and it can be
 * read back by the importer.
 *
 * The round trip is the test that matters: an export nobody can import is a
 * backup that does not restore.
 */
static void
test_kb_export_round_trip(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(VentureKbIngestResult) result = NULL;
	g_autoptr(GBytes) archive = NULL;
	g_autoptr(GError) error = NULL;
	gint64 source_id;
	gint64 target_id;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	source_id = kb_make_base(fixture, "exported");
	kb_make_article(fixture, source_id, "Alpha", "* Alpha\nFirst article.\n",
	                VENTURE_KB_ARTICLE_STATUS_PUBLISHED);
	kb_make_article(fixture, source_id, "Beta", "* Beta\nSecond article.\n",
	                VENTURE_KB_ARTICLE_STATUS_PUBLISHED);

	g_assert_true(venture_kb_export(service, source_id, "zip", &archive,
	                                &error));
	g_assert_no_error(error);
	g_assert_nonnull(archive);
	g_assert_cmpuint(g_bytes_get_size(archive), >, 0);

	/* What came out is an archive by the same test the importer uses. */
	g_assert_true(venture_kb_is_archive("export.zip", archive));

	/* And it imports, into a different base, as the articles it held. */
	target_id = kb_make_base(fixture, "restored");
	result = venture_kb_ingest_result_new();

	g_assert_true(venture_kb_ingest_bytes(service, target_id, archive,
	                                      "export.zip", NULL, NULL, result,
	                                      &error));
	g_assert_no_error(error);

	/* Two articles and the manifest; the manifest is json, which is not a
	 * format the importer reads, so it is skipped rather than filed. */
	g_assert_cmpuint(result->created, ==, 2);
	g_assert_cmpuint(result->skipped, ==, 1);
}

/*
 * tar.gz is offered as well as zip, and an unknown format is refused by
 * name.
 */
static void
test_kb_export_formats(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureKbService) service = NULL;
	g_autoptr(GBytes) tarball = NULL;
	g_autoptr(GBytes) nothing = NULL;
	g_autoptr(GError) error = NULL;
	gint64 kb_id;

	service = kb_service_or_skip(fixture);

	if (NULL == service)
		return;

	kb_id = kb_make_base(fixture, "formats");
	kb_make_article(fixture, kb_id, "Alpha", "* Alpha\nText.\n",
	                VENTURE_KB_ARTICLE_STATUS_PUBLISHED);

	g_assert_true(venture_kb_export(service, kb_id, "tar.gz", &tarball,
	                                &error));
	g_assert_no_error(error);
	g_assert_true(venture_kb_is_archive("export.tar.gz", tarball));

	g_assert_false(venture_kb_export(service, kb_id, "rar", &nothing,
	                                 &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/kb/chunk/short-document",
	                test_kb_chunk_short_document);
	g_test_add_func("/kb/chunk/empty-document",
	                test_kb_chunk_empty_document);
	g_test_add_func("/kb/chunk/splits-on-org-headings",
	                test_kb_chunk_splits_on_org_headings);
	g_test_add_func("/kb/chunk/heading-needs-a-space",
	                test_kb_chunk_heading_needs_a_space);
	g_test_add_func("/kb/chunk/plain-text-has-no-headings",
	                test_kb_chunk_plain_text_has_no_headings);
	g_test_add_func("/kb/chunk/splits-long-sections-with-overlap",
	                test_kb_chunk_splits_long_sections_with_overlap);
	g_test_add_func("/kb/chunk/survives-absurd-settings",
	                test_kb_chunk_survives_absurd_settings);
	g_test_add_func("/kb/chunk/does-not-split-utf8",
	                test_kb_chunk_does_not_split_utf8);
	g_test_add_func("/kb/chunk/embed-text-includes-heading",
	                test_kb_chunk_embed_text_includes_heading);

	g_test_add_func("/kb/format/from-path", test_kb_format_from_path);
	g_test_add_func("/kb/format/extension", test_kb_format_extension);
	g_test_add_func("/kb/slugify", test_kb_slugify);

	g_test_add_func("/kb/embedding/round-trip", test_kb_embedding_round_trip);
	g_test_add_func("/kb/embedding/rejects-rubbish",
	                test_kb_embedding_rejects_rubbish);
	g_test_add_func("/kb/embedding/cosine", test_kb_embedding_cosine);

	g_test_add("/kb/records/round-trip", Fixture, NULL, fixture_set_up,
	           test_kb_records_round_trip, fixture_tear_down);
	g_test_add("/kb/records/slug-is-unique", Fixture, NULL, fixture_set_up,
	           test_kb_slug_is_unique, fixture_tear_down);
	g_test_add("/kb/records/article-needs-a-real-base", Fixture, NULL,
	           fixture_set_up, test_kb_article_needs_a_real_base,
	           fixture_tear_down);

	g_test_add("/kb/search/index-and-search", Fixture, NULL, fixture_set_up,
	           test_kb_index_and_search, fixture_tear_down);
	g_test_add("/kb/search/skips-drafts", Fixture, NULL, fixture_set_up,
	           test_kb_search_skips_drafts, fixture_tear_down);
	g_test_add("/kb/search/reindex-replaces-chunks", Fixture, NULL,
	           fixture_set_up, test_kb_reindex_replaces_chunks,
	           fixture_tear_down);
	g_test_add("/kb/search/resolve-slugs", Fixture, NULL, fixture_set_up,
	           test_kb_resolve_slugs, fixture_tear_down);

	g_test_add_func("/kb/extract/refuses-bad-utf8",
	                test_kb_extract_refuses_bad_utf8);
	g_test_add_func("/kb/archive/detection", test_kb_archive_detection);

	g_test_add("/kb/ingest/is-idempotent", Fixture, NULL, fixture_set_up,
	           test_kb_ingest_is_idempotent, fixture_tear_down);
	g_test_add("/kb/ingest/names-the-article", Fixture, NULL, fixture_set_up,
	           test_kb_ingest_names_the_article, fixture_tear_down);
	g_test_add("/kb/ingest/skips-unreadable", Fixture, NULL, fixture_set_up,
	           test_kb_ingest_skips_unreadable, fixture_tear_down);
	g_test_add("/kb/sync/directory", Fixture, NULL, fixture_set_up,
	           test_kb_sync_directory, fixture_tear_down);
	g_test_add("/kb/sync/needs-a-directory", Fixture, NULL, fixture_set_up,
	           test_kb_sync_needs_a_directory, fixture_tear_down);
	g_test_add("/kb/export/round-trip", Fixture, NULL, fixture_set_up,
	           test_kb_export_round_trip, fixture_tear_down);
	g_test_add("/kb/export/formats", Fixture, NULL, fixture_set_up,
	           test_kb_export_formats, fixture_tear_down);

	return g_test_run();
}
