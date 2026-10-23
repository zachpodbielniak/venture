/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Designed email: brand settings, hosted images, the block model and its
 * renderer, reusable sections, versions and undo, and the AI designer with
 * its grounding rule.
 *
 * What matters here is what reaches a customer's mail app. The renderer is
 * held to a structural check -- tables, inline styles, nothing a mail app
 * strips, nothing wider than 600 pixels, alt text and a width on every
 * image, a dark-mode block and a real text alternative -- because no test
 * can run the mail apps themselves. The designer is held to the rule that
 * a price, an amount, a percentage or a date it writes must be in the brief
 * or the records; a model that invents a discount is caught in code, not
 * trusted to follow its prompt. No test calls a real AI provider or the
 * network: the provider is a canned fake.
 */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

/* --- Image bytes ------------------------------------------------------------ */

static GBytes *png_bytes(guint width, guint height)
{
	guint8 data[40] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
	data[16] = (width >> 24) & 0xff; data[17] = (width >> 16) & 0xff; data[18] = (width >> 8) & 0xff; data[19] = width & 0xff;
	data[20] = (height >> 24) & 0xff; data[21] = (height >> 16) & 0xff; data[22] = (height >> 8) & 0xff; data[23] = height & 0xff;
	data[24] = 8; data[25] = 6;
	return g_bytes_new(data, sizeof data);
}
static GBytes *jpeg_bytes(guint width, guint height)
{
	guint8 data[48] = { 0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
		0xff, 0xc0, 0x00, 0x11, 0x08 };
	data[25] = (height >> 8) & 0xff; data[26] = height & 0xff;
	data[27] = (width >> 8) & 0xff; data[28] = width & 0xff;
	data[29] = 3;
	return g_bytes_new(data, sizeof data);
}
static GBytes *gif_bytes(guint width, guint height)
{
	guint8 data[24] = { 'G', 'I', 'F', '8', '9', 'a' };
	data[6] = width & 0xff; data[7] = (width >> 8) & 0xff;
	data[8] = height & 0xff; data[9] = (height >> 8) & 0xff;
	return g_bytes_new(data, sizeof data);
}
static GBytes *webp_bytes(const gchar *chunk, guint width, guint height)
{
	guint8 data[40] = { 'R', 'I', 'F', 'F', 32, 0, 0, 0, 'W', 'E', 'B', 'P' };
	memcpy(data + 12, chunk, 4);
	data[16] = 20;
	if (g_str_equal(chunk, "VP8X")) {
		data[24] = (width - 1) & 0xff; data[25] = ((width - 1) >> 8) & 0xff; data[26] = ((width - 1) >> 16) & 0xff;
		data[27] = (height - 1) & 0xff; data[28] = ((height - 1) >> 8) & 0xff; data[29] = ((height - 1) >> 16) & 0xff;
	} else if (g_str_equal(chunk, "VP8L")) {
		guint32 bits = ((width - 1) & 0x3fff) | (((height - 1) & 0x3fff) << 14);
		data[20] = 0x2f;
		data[21] = bits & 0xff; data[22] = (bits >> 8) & 0xff; data[23] = (bits >> 16) & 0xff; data[24] = (bits >> 24) & 0xff;
	} else {
		data[23] = 0x9d; data[24] = 0x01; data[25] = 0x2a;
		data[26] = width & 0xff; data[27] = (width >> 8) & 0x3f;
		data[28] = height & 0xff; data[29] = (height >> 8) & 0x3f;
	}
	return g_bytes_new(data, sizeof data);
}
/* A real picture when the screenshot run hands one in, so the pictures
 * show a logo; otherwise a header-only PNG, which is all a test reads. */
static GBytes *picture(const gchar *variable, guint width, guint height)
{
	const gchar *path = g_getenv(variable);
	gchar *contents = NULL;
	gsize length = 0;
	if (path != NULL && g_file_get_contents(path, &contents, &length, NULL))
		return g_bytes_new_take(contents, length);
	return png_bytes(width, height);
}

/* --- A canned AI provider ---------------------------------------------------- */

#define TYPE_CANNED_PROVIDER (canned_provider_get_type())
G_DECLARE_FINAL_TYPE(CannedProvider, canned_provider, CANNED, PROVIDER, GObject)
struct _CannedProvider {
	GObject parent_instance;
	GQueue *answers;
	gchar *prompt;
	gchar *system;
	guint calls;
};
static void canned_iface(AiProviderInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(CannedProvider, canned_provider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, canned_iface))
static void canned_finalize(GObject *object)
{
	CannedProvider *self = CANNED_PROVIDER(object);
	g_queue_free_full(self->answers, g_free);
	g_free(self->prompt);
	g_free(self->system);
	G_OBJECT_CLASS(canned_provider_parent_class)->finalize(object);
}
static void canned_provider_class_init(CannedProviderClass *klass) { G_OBJECT_CLASS(klass)->finalize = canned_finalize; }
static void canned_provider_init(CannedProvider *self) { self->answers = g_queue_new(); }
static void canned_chat(AiProvider *provider, GList *messages, const gchar *system_prompt, gint max_tokens,
	GList *offered_tools, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
	CannedProvider *self = CANNED_PROVIDER(provider);
	g_autoptr(GTask) task = g_task_new(provider, cancellable, callback, data);
	g_autofree gchar *answer = g_queue_pop_head(self->answers);
	g_autoptr(AiTextContent) text = NULL;
	AiResponse *response = ai_response_new("fixture", "fixture");
	(void)max_tokens;
	/* The designer reads the organization's own records and a person's
	 * brief; it is offered no tool to act with. */
	g_assert_null(offered_tools);
	g_assert_nonnull(answer);
	g_free(self->prompt); g_free(self->system);
	self->prompt = ai_message_get_text(messages->data);
	self->system = g_strdup(system_prompt);
	self->calls++;
	text = ai_text_content_new(answer);
	ai_response_add_content_block(response, AI_CONTENT_BLOCK(g_steal_pointer(&text)));
	g_task_return_pointer(task, response, g_object_unref);
}
static AiResponse *canned_finish(AiProvider *provider, GAsyncResult *result, GError **error)
{
	(void)provider;
	return g_task_propagate_pointer(G_TASK(result), error);
}
static const gchar *canned_name(AiProvider *provider) { (void)provider; return "fixture"; }
static AiProviderType canned_type(AiProvider *provider) { (void)provider; return AI_PROVIDER_OPENAI; }
static void canned_iface(AiProviderInterface *iface)
{
	iface->chat_async = canned_chat;
	iface->chat_finish = canned_finish;
	iface->get_name = canned_name;
	iface->get_default_model = canned_name;
	iface->get_provider_type = canned_type;
}

/* --- Structural rendering assertions ------------------------------------------ */

/*
 * The check set that stands in for a client farm. It cannot say that a
 * message looks right in a given mail app; it says the message is built
 * from the things every major one renders: tables, inline styles, a fixed
 * 600-pixel body, images that keep their place when blocked, and nothing a
 * client strips or refuses (script, forms, linked stylesheets).
 */
static void assert_email_safe(const gchar *html, const gchar *text)
{
	g_autoptr(GRegex) widths = g_regex_new("width=\"([0-9]+)\"", 0, 0, NULL);
	g_autoptr(GRegex) px = g_regex_new("[^-]width:\\s*([0-9]+)px", 0, 0, NULL);
	g_autoptr(GRegex) images = g_regex_new("<img\\b[^>]*>", G_REGEX_CASELESS, 0, NULL);
	g_autoptr(GRegex) alt = g_regex_new("\\balt=\"[^\"]+\"", 0, 0, NULL);
	g_autoptr(GRegex) image_width = g_regex_new("\\bwidth=\"[0-9]+\"", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	guint styles = 0, found = 0;
	const gchar *p;
	g_assert_nonnull(html);
	g_assert_nonnull(strstr(html, "<table role=\"presentation\""));
	for (p = html; (p = strstr(p, "style=\"")) != NULL; p++) styles++;
	g_assert_cmpuint(styles, >=, 10);
	g_assert_null(strstr(html, "<script"));
	g_assert_null(strstr(html, "<form"));
	g_assert_null(strstr(html, "<link"));
	g_assert_null(strstr(html, "@import"));
	g_assert_null(strstr(html, "javascript:"));
	g_assert_nonnull(strstr(html, "max-width:600px"));
	g_assert_nonnull(strstr(html, "<meta name=\"color-scheme\" content=\"light dark\">"));
	g_assert_nonnull(strstr(html, "@media (prefers-color-scheme: dark)"));
	g_assert_nonnull(strstr(html, "<!--[if mso]>"));
	g_regex_match(widths, html, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *n = g_match_info_fetch(info, 1);
		g_assert_cmpint(g_ascii_strtoll(n, NULL, 10), <=, 600);
		g_match_info_next(info, NULL);
	}
	g_clear_pointer(&info, g_match_info_free);
	g_regex_match(px, html, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *n = g_match_info_fetch(info, 1);
		g_assert_cmpint(g_ascii_strtoll(n, NULL, 10), <=, 600);
		g_match_info_next(info, NULL);
	}
	g_clear_pointer(&info, g_match_info_free);
	g_regex_match(images, html, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *tag = g_match_info_fetch(info, 0);
		g_assert_true(g_regex_match(alt, tag, 0, NULL));
		g_assert_true(g_regex_match(image_width, tag, 0, NULL));
		found++;
		g_match_info_next(info, NULL);
	}
	(void)found;
	g_assert_nonnull(text);
	g_assert_cmpuint(strlen(text), >=, 20);
	g_assert_null(strstr(text, "<td"));
	g_assert_null(strstr(text, "<table"));
}

static const gchar every_block[] =
	"{\"version\":1,\"preheader\":\"Fall is here.\",\"blocks\":["
	"{\"type\":\"header\",\"tagline\":\"Warm homes since forever\"},"
	"{\"type\":\"text\",\"heading\":\"Ready for the cold?\",\"body\":\"A tune-up keeps you warm.\\n\\nBook by the end of the month.\",\"align\":\"left\"},"
	"{\"type\":\"image\",\"src\":\"https://cdn.example.com/furnace.jpg\",\"alt\":\"A clean furnace\",\"width\":560,\"link\":\"https://shop.example.com/\"},"
	"{\"type\":\"button\",\"label\":\"Book now\",\"url\":\"https://shop.example.com/book\",\"size\":\"large\",\"align\":\"center\"},"
	"{\"type\":\"columns\",\"columns\":[{\"blocks\":[{\"type\":\"text\",\"heading\":\"Tune-up\",\"body\":\"Inspection and cleaning.\"},"
	"{\"type\":\"image\",\"src\":\"https://cdn.example.com/a.png\",\"alt\":\"Tune-up\"}]},"
	"{\"blocks\":[{\"type\":\"text\",\"heading\":\"Filters\",\"body\":\"Cleaner air.\"},{\"type\":\"button\",\"label\":\"Order\",\"url\":\"https://shop.example.com/filters\",\"size\":\"small\"}]}]},"
	"{\"type\":\"divider\"},"
	"{\"type\":\"social\",\"links\":[{\"network\":\"facebook\",\"url\":\"https://facebook.example.com/heat\"},{\"network\":\"website\",\"url\":\"https://shop.example.com/\"}]},"
	"{\"type\":\"video\",\"url\":\"https://video.example.com/watch/1\",\"poster_src\":\"https://cdn.example.com/poster.jpg\",\"alt\":\"How a tune-up works\",\"caption\":\"Two minutes on what we check\"},"
	"{\"type\":\"footer\",\"text\":\"You get this because you asked us to.\",\"address\":\"1 Example Road\"}"
	"]}";

/* A look a test can recognise in the output. */
static VentureEmailLook *heating_look(void)
{
	VentureEmailLook *look = venture_email_look_new_default("Brightside Heating & Air");
	g_free(look->primary); look->primary = g_strdup("#c2410c");
	g_free(look->accent); look->accent = g_strdup("#0f766e");
	g_free(look->dark_background); look->dark_background = g_strdup("#101820");
	g_free(look->font); look->font = g_strdup(venture_email_font_stack("serif"));
	look->logo_url = g_strdup("https://books.example.com/i/1/logo.png");
	look->logo_width = 180; look->logo_height = 60;
	return look;
}

/* --- Validation ---------------------------------------------------------------- */

/* One valid design per block type, and the whole set together. */
static void test_validate_pass(void)
{
	const gchar *designs[] = {
		"{\"blocks\":[{\"type\":\"header\"}]}",
		"{\"blocks\":[{\"type\":\"header\",\"show_logo\":false,\"tagline\":\"Hello\",\"align\":\"left\"}]}",
		"{\"blocks\":[{\"type\":\"text\",\"body\":\"Hello {first_name}, a {{literal}} brace.\"}]}",
		"{\"blocks\":[{\"type\":\"image\",\"src\":\"http://cdn.example.com/a.gif\",\"alt\":\"A picture\"}]}",
		"{\"blocks\":[{\"type\":\"image\",\"image_id\":3,\"alt\":\"A picture\",\"width\":300}]}",
		"{\"blocks\":[{\"type\":\"button\",\"label\":\"Go\",\"url\":\"https://example.com/go\",\"color\":\"accent\"}]}",
		"{\"blocks\":[{\"type\":\"columns\",\"columns\":[{\"blocks\":[]},{\"blocks\":[{\"type\":\"divider\"}]}]}]}",
		"{\"blocks\":[{\"type\":\"columns\",\"columns\":[{\"blocks\":[]},{\"blocks\":[]},{\"blocks\":[{\"type\":\"text\",\"body\":\"Three\"}]}]}]}",
		"{\"blocks\":[{\"type\":\"divider\"}]}",
		"{\"blocks\":[{\"type\":\"social\",\"links\":[{\"network\":\"instagram\",\"url\":\"https://instagram.example.com/a\"}]}]}",
		"{\"blocks\":[{\"type\":\"footer\"}]}",
		"{\"blocks\":[{\"type\":\"video\",\"url\":\"https://video.example.com/1\",\"poster_image_id\":4,\"alt\":\"Our video\"}]}",
		"{\"blocks\":[{\"type\":\"section\",\"section_id\":7}]}",
		every_block,
		NULL
	};
	guint i;
	for (i = 0; designs[i]; i++) {
		g_autoptr(GError) error = NULL;
		if (!venture_email_design_validate(designs[i], &error))
			g_error("design %u refused: %s", i, error ? error->message : "(no reason)");
	}
}

/* Strict: anything unknown, unbounded or unsafe is refused with a reason. */
static void test_validate_fail(void)
{
	const gchar *designs[] = {
		"",
		"[]",
		"{\"blocks\":{}}",
		"{\"blocks\":[],\"style\":\"x\"}",
		"{\"version\":2,\"blocks\":[]}",
		"{\"blocks\":[{\"type\":\"marquee\"}]}",
		"{\"blocks\":[{\"body\":\"no type\"}]}",
		"{\"blocks\":[{\"type\":\"text\",\"body\":\"x\",\"onclick\":\"x\"}]}",
		"{\"blocks\":[{\"type\":\"text\"}]}",
		"{\"blocks\":[{\"type\":\"text\",\"body\":7}]}",
		"{\"blocks\":[{\"type\":\"text\",\"body\":\"Hello { first_name }\"}]}",
		"{\"blocks\":[{\"type\":\"text\",\"body\":\"x\",\"align\":\"justify\"}]}",
		"{\"blocks\":[{\"type\":\"image\",\"src\":\"https://cdn.example.com/a.png\"}]}",
		"{\"blocks\":[{\"type\":\"image\",\"src\":\"https://cdn.example.com/a.png\",\"alt\":\"\"}]}",
		"{\"blocks\":[{\"type\":\"image\",\"src\":\"https://cdn.example.com/a.png\",\"image_id\":2,\"alt\":\"x\"}]}",
		"{\"blocks\":[{\"type\":\"image\",\"alt\":\"x\"}]}",
		"{\"blocks\":[{\"type\":\"image\",\"src\":\"javascript:alert(1)\",\"alt\":\"x\"}]}",
		"{\"blocks\":[{\"type\":\"image\",\"src\":\"data:image/png;base64,AAAA\",\"alt\":\"x\"}]}",
		"{\"blocks\":[{\"type\":\"image\",\"src\":\"https://cdn.example.com/a.png\",\"alt\":\"x\",\"width\":700}]}",
		"{\"blocks\":[{\"type\":\"button\",\"label\":\"Go\"}]}",
		"{\"blocks\":[{\"type\":\"button\",\"label\":\"Go\",\"url\":\"ftp://example.com/\"}]}",
		"{\"blocks\":[{\"type\":\"button\",\"label\":\"Go\",\"url\":\"/relative\"}]}",
		"{\"blocks\":[{\"type\":\"button\",\"label\":\"Go\",\"url\":\"https://example.com/\",\"size\":\"huge\"}]}",
		"{\"blocks\":[{\"type\":\"button\",\"label\":\"An extremely long button label that goes on and on and on past every sensible length of a label\",\"url\":\"https://example.com/\"}]}",
		"{\"blocks\":[{\"type\":\"columns\",\"columns\":[{\"blocks\":[]}]}]}",
		"{\"blocks\":[{\"type\":\"columns\",\"columns\":[{\"blocks\":[]},{\"blocks\":[]},{\"blocks\":[]},{\"blocks\":[]}]}]}",
		"{\"blocks\":[{\"type\":\"columns\",\"columns\":[{\"blocks\":[{\"type\":\"columns\",\"columns\":[{\"blocks\":[]},{\"blocks\":[]}]}]},{\"blocks\":[]}]}]}",
		"{\"blocks\":[{\"type\":\"columns\",\"columns\":[{\"blocks\":[{\"type\":\"section\",\"section_id\":1}]},{\"blocks\":[]}]}]}",
		"{\"blocks\":[{\"type\":\"columns\",\"columns\":[{\"blocks\":[],\"width\":3},{\"blocks\":[]}]}]}",
		"{\"blocks\":[{\"type\":\"divider\",\"color\":\"red\"}]}",
		"{\"blocks\":[{\"type\":\"social\",\"links\":[]}]}",
		"{\"blocks\":[{\"type\":\"social\",\"links\":[{\"network\":\"myspace\",\"url\":\"https://example.com/\"}]}]}",
		"{\"blocks\":[{\"type\":\"video\",\"url\":\"https://video.example.com/1\",\"alt\":\"x\"}]}",
		"{\"blocks\":[{\"type\":\"video\",\"url\":\"https://video.example.com/1\",\"poster_src\":\"https://cdn.example.com/p.jpg\"}]}",
		"{\"blocks\":[{\"type\":\"section\"}]}",
		"{\"blocks\":[{\"type\":\"section\",\"section_id\":0}]}",
		"{\"blocks\":[{\"type\":\"header\",\"tagline\":\"x\",\"tagline\":\"y\",\"extra\":1}]}",
		NULL
	};
	g_autoptr(GString) many = g_string_new("{\"blocks\":[");
	g_autoptr(GError) error = NULL;
	guint i;
	for (i = 0; designs[i]; i++) {
		g_autoptr(GError) refused = NULL;
		if (venture_email_design_validate(designs[i], &refused))
			g_error("design %u accepted: %s", i, designs[i]);
		g_assert_nonnull(refused);
		g_assert_false(venture_string_is_empty(refused->message));
	}
	for (i = 0; i <= VENTURE_EMAIL_DESIGN_MAX_BLOCKS; i++)
		g_string_append(many, i ? ",{\"type\":\"divider\"}" : "{\"type\":\"divider\"}");
	g_string_append(many, "]}");
	g_assert_false(venture_email_design_validate(many->str, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* --- Rendering ------------------------------------------------------------------- */

static void render(const gchar *design, const VentureEmailLook *look, gchar **html, gchar **text)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_email_design_render(design, look, NULL, "Fall tune-up", FALSE, html, text, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

/* Every block renders, as tables with inline styles, and the text
 * alternative carries the words and every link. */
static void test_render_structure(void)
{
	g_autoptr(VentureEmailLook) look = heating_look();
	g_autofree gchar *html = NULL, *text = NULL;
	render(every_block, look, &html, &text);
	assert_email_safe(html, text);
	g_assert_nonnull(strstr(html, "<title>Fall tune-up</title>"));
	g_assert_nonnull(strstr(html, "Fall is here."));
	/* Bulletproof button: the link is the padded box, so it is clickable
	 * everywhere and needs no image. */
	g_assert_nonnull(strstr(html, "href=\"https://shop.example.com/book\""));
	g_assert_nonnull(strstr(html, "display:inline-block"));
	g_assert_nonnull(strstr(html, "class=\"ve-col\""));
	g_assert_nonnull(strstr(html, "Inspection and cleaning."));
	g_assert_nonnull(strstr(html, "border-top:1px solid"));
	g_assert_nonnull(strstr(html, ">Facebook</a>"));
	g_assert_nonnull(strstr(html, "1 Example Road"));
	g_assert_nonnull(strstr(text, "Ready for the cold?"));
	g_assert_nonnull(strstr(text, "A tune-up keeps you warm."));
	g_assert_nonnull(strstr(text, "Book now: https://shop.example.com/book"));
	g_assert_nonnull(strstr(text, "Facebook: https://facebook.example.com/heat"));
	g_assert_nonnull(strstr(text, "1 Example Road"));
	g_assert_null(strstr(text, "<"));
}

/* Dark mode: a prefers-color-scheme block with the brand's dark colours,
 * or ones derived from the light colours when the brand gives none. */
static void test_render_dark(void)
{
	g_autoptr(VentureEmailLook) look = heating_look();
	g_autoptr(VentureEmailLook) plain = venture_email_look_new_default(NULL);
	g_autofree gchar *html = NULL, *text = NULL, *plain_html = NULL, *plain_text = NULL;
	const gchar *dark;
	render(every_block, look, &html, &text);
	dark = strstr(html, "@media (prefers-color-scheme: dark)");
	g_assert_nonnull(dark);
	g_assert_nonnull(strstr(dark, "#101820"));
	g_assert_nonnull(strstr(dark, ".ve-body"));
	g_assert_nonnull(strstr(dark, ".ve-text"));
	g_assert_nonnull(strstr(html, "[data-ogsc]"));
	render(every_block, plain, &plain_html, &plain_text);
	dark = strstr(plain_html, "@media (prefers-color-scheme: dark)");
	g_assert_nonnull(dark);
	g_assert_nonnull(strstr(dark, plain->dark_background));
	g_assert_nonnull(strstr(dark, plain->dark_text));
}

/* The brand reaches the message: colour on the button, the font stack,
 * the logo with the business name as its alt text. */
static void test_render_brand(void)
{
	g_autoptr(VentureEmailLook) look = heating_look();
	g_autoptr(VentureEmailLook) plain = venture_email_look_new_default("Plain Co");
	g_autofree gchar *html = NULL, *text = NULL, *plain_html = NULL, *plain_text = NULL;
	render(every_block, look, &html, &text);
	g_assert_nonnull(strstr(html, "background:#c2410c"));
	g_assert_nonnull(strstr(html, "Georgia"));
	g_assert_nonnull(strstr(html, "src=\"https://books.example.com/i/1/logo.png\""));
	g_assert_nonnull(strstr(html, "alt=\"Brightside Heating &amp; Air\""));
	g_assert_nonnull(strstr(text, "Brightside Heating & Air"));
	/* No logo: the name stands in, as text. */
	render(every_block, plain, &plain_html, &plain_text);
	g_assert_null(strstr(plain_html, "logo.png"));
	g_assert_nonnull(strstr(plain_html, "Plain Co"));
	g_assert_nonnull(strstr(plain_html, "Arial"));
	g_assert_null(venture_email_font_stack("comic"));
}

/* Video: a poster with a play button over it, linking to the video's own
 * page -- no mail app plays video in the message. */
static void test_render_video(void)
{
	const gchar *design = "{\"blocks\":[{\"type\":\"video\",\"url\":\"https://video.example.com/watch/1\","
		"\"poster_src\":\"https://cdn.example.com/poster.jpg\",\"alt\":\"How a tune-up works\",\"caption\":\"Two minutes\"}]}";
	g_autoptr(VentureEmailLook) look = venture_email_look_new_default("Brightside");
	g_autofree gchar *html = NULL, *text = NULL;
	render(design, look, &html, &text);
	assert_email_safe(html, text);
	g_assert_nonnull(strstr(html, "href=\"https://video.example.com/watch/1\""));
	g_assert_nonnull(strstr(html, "https://cdn.example.com/poster.jpg"));
	/* The play button is drawn with borders, not a glyph a font may lack. */
	g_assert_nonnull(strstr(html, "class=\"ve-play\""));
	g_assert_nonnull(strstr(html, "alt=\"How a tune-up works\""));
	g_assert_nonnull(strstr(text, "Watch the video (How a tune-up works): https://video.example.com/watch/1"));
	g_assert_null(strstr(html, "<video"));
	/* Some webmail (SOGo) strips a downlevel-revealed "<!--[if !mso]><!-->"
	 * section as a comment, which lost the poster and its play button.
	 * The poster is hidden from Outlook by mso-hide instead. */
	g_assert_null(strstr(html, "<!--[if !mso]>"));
	g_assert_nonnull(strstr(html, "role=\"presentation\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\" style=\"mso-hide:all\""));
}

/* A design names a hosted image by id; with nothing to resolve it, the
 * render is refused rather than written with a hole in it. */
static void test_render_unresolved(void)
{
	g_autoptr(VentureEmailLook) look = venture_email_look_new_default(NULL);
	g_autofree gchar *html = NULL, *text = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_email_design_render("{\"blocks\":[{\"type\":\"image\",\"image_id\":5,\"alt\":\"x\"}]}",
		look, NULL, NULL, FALSE, &html, &text, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_email_design_render("{\"blocks\":[{\"type\":\"section\",\"section_id\":5}]}",
		look, NULL, NULL, FALSE, &html, &text, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* --- Image sniffing ------------------------------------------------------------------ */

static void sniffed(GBytes *bytes, const gchar *type, gint width, gint height)
{
	gint w = 0, h = 0;
	const gchar *extension = NULL;
	g_assert_cmpstr(venture_hosted_image_sniff(bytes, &w, &h, &extension), ==, type);
	g_assert_cmpint(w, ==, width);
	g_assert_cmpint(h, ==, height);
	g_assert_nonnull(extension);
	g_bytes_unref(bytes);
}
static void test_sniff(void)
{
	g_autoptr(GBytes) svg = g_bytes_new_static("<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>", 45);
	g_autoptr(GBytes) text = g_bytes_new_static("PNG but not really", 18);
	g_autoptr(GBytes) truncated = g_bytes_new_static("\x89PNG\r\n\x1a\n", 8);
	sniffed(png_bytes(640, 480), "image/png", 640, 480);
	sniffed(jpeg_bytes(1200, 800), "image/jpeg", 1200, 800);
	sniffed(gif_bytes(32, 16), "image/gif", 32, 16);
	sniffed(webp_bytes("VP8X", 3000, 2000), "image/webp", 3000, 2000);
	sniffed(webp_bytes("VP8L", 400, 300), "image/webp", 400, 300);
	sniffed(webp_bytes("VP8 ", 320, 240), "image/webp", 320, 240);
	g_assert_null(venture_hosted_image_sniff(svg, NULL, NULL, NULL));
	g_assert_null(venture_hosted_image_sniff(text, NULL, NULL, NULL));
	g_assert_null(venture_hosted_image_sniff(truncated, NULL, NULL, NULL));
}

/* --- Grounding -------------------------------------------------------------------- */

/* The rule, without a model: what the brief and the records say may be
 * repeated; a number from nowhere is removed with its sentence, and one in
 * a place that cannot lose a sentence refuses the design. */
static void test_ground(void)
{
	const gchar *sources[] = { "fall furnace tune-up, 10% off, ends Nov 15", "Furnace tune-up 129.00 USD", NULL };
	const gchar *good = "{\"blocks\":[{\"type\":\"text\",\"heading\":\"Save 10% this fall\","
		"\"body\":\"Book by November 15th. A tune-up is $129. It is the best value in town!\"}]}";
	const gchar *invented = "{\"blocks\":[{\"type\":\"text\",\"heading\":\"Fall tune-ups\","
		"\"body\":\"Book by Nov 15. Today only, just $99! Free filter with every visit. We look forward to it.\"}]}";
	const gchar *label = "{\"blocks\":[{\"type\":\"button\",\"label\":\"Save $50 now\",\"url\":\"https://example.com/\"}]}";
	g_autoptr(GPtrArray) stripped = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *kept = NULL, *cleaned = NULL, *refused = NULL;
	kept = venture_email_design_ground(good, sources, stripped, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(kept, ==, good);
	g_assert_cmpuint(stripped->len, ==, 0);
	cleaned = venture_email_design_ground(invented, sources, stripped, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cleaned);
	g_assert_nonnull(strstr(cleaned, "Book by Nov 15."));
	g_assert_nonnull(strstr(cleaned, "We look forward to it."));
	g_assert_null(strstr(cleaned, "$99"));
	g_assert_null(strstr(cleaned, "Free filter"));
	g_assert_cmpuint(stripped->len, ==, 2);
	g_assert_true(venture_email_design_validate(cleaned, NULL));
	refused = venture_email_design_ground(label, sources, stripped, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "$50"));
}

/* --- With a database ------------------------------------------------------------------ */

typedef struct {
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gchar *directory;
	gint64 org;
	CannedProvider *provider;
	VentureAiService *ai;
	gint64 logo, photo, poster;
} Fixture;

static VentureEntity *record(Fixture *f, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	return g_object_new(type, "organization-id", f->org, NULL);
}
static void save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, row, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}
static gboolean try_save(Fixture *f, VentureEntity *row, GError **error)
{
	return venture_database_save(f->db, row, NULL, error);
}
static VentureEntity *reload(Fixture *f, VentureEntity *row)
{
	return venture_database_get(f->db, G_OBJECT_TYPE(row), venture_entity_get_id(row), NULL);
}
static gchar *string(VentureEntity *row, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(row, name, &value, NULL);
	return value;
}
static GPtrArray *rows(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(f->db, query, NULL);
}
static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->directory = g_dir_make_tmp("venture-email-design-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-base-url", "https://books.example.com", NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}
static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	if (f->ai != NULL)
		venture_context_set_ai_service(f->context, NULL);
	g_clear_object(&f->ai);
	g_clear_object(&f->provider);
	g_clear_object(&f->context); g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
}
static void answer(Fixture *f, const gchar *reply)
{
	g_autoptr(GError) error = NULL;
	if (f->provider == NULL) {
		f->provider = g_object_new(TYPE_CANNED_PROVIDER, NULL);
		f->ai = venture_ai_service_new_with_provider(f->context, AI_PROVIDER(f->provider), &error);
		g_assert_no_error(error);
		venture_context_set_ai_service(f->context, f->ai);
	}
	g_queue_push_tail(f->provider->answers, g_strdup(reply));
}
static VentureEntity *store_image(Fixture *f, GBytes *bytes, const gchar *name, const gchar *alt)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *image = venture_hosted_image_store(f->context, f->org, name, bytes, alt, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(image);
	return image;
}
static VentureEntity *template_with(Fixture *f, const gchar *name, const gchar *design)
{
	g_autoptr(VentureEntity) template = record(f, "mail_template");
	g_autoptr(VentureEntity) version = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(template, "name", name, "subject", "Fall tune-up", NULL);
	version = venture_email_design_save(f->context, template, design, "manual", NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(version);
	return reload(f, template);
}

/* The example heating business: a logo, colours, a font, a website and two
 * services with prices and photos. Every name and figure is made up. */
static void seed_heating(Fixture *f)
{
	g_autoptr(VentureEntity) brand = record(f, "email_brand");
	g_autoptr(VentureEntity) venture = record(f, "venture");
	g_autoptr(VentureEntity) logo = store_image(f, picture("VENTURE_EMAIL_DESIGN_SHOTS_LOGO", 360, 120), "logo.png", "Brightside Heating & Air");
	g_autoptr(VentureEntity) photo = store_image(f, picture("VENTURE_EMAIL_DESIGN_SHOTS_PHOTO", 1200, 700), "furnace.png", "A technician servicing a furnace");
	g_autoptr(VentureEntity) poster = store_image(f, picture("VENTURE_EMAIL_DESIGN_SHOTS_POSTER", 1200, 675), "poster.png", "A furnace being cleaned");
	g_autoptr(VentureEntity) tuneup = record(f, "product");
	g_autoptr(VentureEntity) filter = record(f, "product");
	g_autofree gchar *photo_url = venture_hosted_image_url(f->context, photo);
	f->logo = venture_entity_get_id(logo);
	f->photo = venture_entity_get_id(photo);
	f->poster = venture_entity_get_id(poster);
	g_object_set(brand, "name", "Brightside Heating & Air", "logo-image-id", f->logo,
		"primary-color", "#c2410c", "accent-color", "#0f766e", "text-color", "#1f2933",
		"background-color", "#f6f1eb", "font", "rounded", "website-url", "https://brightside.example.com",
		"footer-address", "100 Example Street, Exampleton", NULL);
	save(f, brand);
	g_object_set(venture, "name", "Heating and cooling", NULL);
	save(f, venture);
	g_object_set(tuneup, "name", "Furnace tune-up", "venture-id", venture_entity_get_id(venture),
		"url", "https://brightside.example.com/tune-up", "image-url", photo_url,
		"description", "Full inspection, cleaning and safety check", "active", TRUE, NULL);
	g_assert_true(venture_entity_set_field_from_string(tuneup, "list-price", "129.00 USD", NULL));
	save(f, tuneup);
	g_object_set(filter, "name", "Filter replacement", "venture-id", venture_entity_get_id(venture),
		"url", "https://brightside.example.com/filters", "description", "A fresh filter for cleaner air", "active", TRUE, NULL);
	g_assert_true(venture_entity_set_field_from_string(filter, "list-price", "39.00 USD", NULL));
	save(f, filter);
}

/* What the canned model answers to the brief "fall furnace tune-up, 10%
 * off, ends Nov 15": every figure in it is in the brief or the records. */
static gchar *heating_reply(Fixture *f, const gchar *button_size, const gchar *extra_sentence)
{
	return g_strdup_printf(
		"{\"subject\":\"Fall furnace tune-up: 10%% off until Nov 15\",\"design\":{"
		"\"preheader\":\"Book a fall furnace tune-up by Nov 15 and save 10%%.\",\"blocks\":["
		"{\"type\":\"header\"},"
		"{\"type\":\"image\",\"image_id\":%" G_GINT64_FORMAT ",\"alt\":\"A technician servicing a furnace\",\"link\":\"https://brightside.example.com/tune-up\"},"
		"{\"type\":\"text\",\"heading\":\"Get your furnace ready for fall\",\"body\":\"Cold nights are coming. A tune-up now keeps your home warm and your heating running smoothly.%s\\n\\nBook a fall furnace tune-up by Nov 15 and save 10%%.\"},"
		"{\"type\":\"button\",\"label\":\"Book your tune-up\",\"url\":\"https://brightside.example.com/tune-up\",\"size\":\"%s\"},"
		"{\"type\":\"columns\",\"columns\":["
		"{\"blocks\":[{\"type\":\"text\",\"heading\":\"Furnace tune-up: $129.00\",\"body\":\"Full inspection, cleaning and safety check.\"}]},"
		"{\"blocks\":[{\"type\":\"text\",\"heading\":\"Filter replacement: $39.00\",\"body\":\"A fresh filter for cleaner air.\"}]}]},"
		"{\"type\":\"divider\"},"
		"{\"type\":\"social\",\"links\":[{\"network\":\"website\",\"url\":\"https://brightside.example.com\"}]},"
		"{\"type\":\"footer\",\"text\":\"You are receiving this because you are a customer of Brightside Heating & Air.\"}"
		"]}}",
		f->photo, extra_sentence != NULL ? extra_sentence : "", button_size);
}
static const gchar brief[] = "fall furnace tune-up, 10% off, ends Nov 15";

/* A template with a design gets its HTML and text bodies from it, and the
 * existing renderer turns them into the message with placeholders filled. */
static void test_template(Fixture *f, gconstpointer unused)
{
	const gchar *design = "{\"blocks\":[{\"type\":\"text\",\"heading\":\"Hello {first_name}\",\"body\":\"Your {{gift} awaits.\"},"
		"{\"type\":\"button\",\"label\":\"Open\",\"url\":\"https://shop.example.com/\"}]}";
	g_autoptr(VentureEntity) template = NULL, plain = record(f, "mail_template");
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *html = NULL, *text = NULL, *body = NULL, *stored = NULL;
	(void)unused;
	template = template_with(f, "Welcome", design);
	stored = string(template, "html-body");
	g_assert_nonnull(strstr(stored, "{first_name}"));
	/* Literal braces in the CSS are doubled, so the template renderer
	 * cannot read a style rule as a placeholder. */
	g_assert_nonnull(strstr(stored, "{{"));
	json_object_set_string_member(values, "first_name", "Dana <3");
	message = venture_mail_template_render_values(VENTURE_MAIL_TEMPLATE(template), values, &error);
	g_assert_no_error(error);
	g_object_get(message, "html-body", &html, "text-body", &text, NULL);
	assert_email_safe(html, text);
	g_assert_nonnull(strstr(html, "Hello Dana &lt;3"));
	g_assert_nonnull(strstr(html, "Your {gift} awaits."));
	g_assert_nonnull(strstr(html, "@media (prefers-color-scheme: dark) {"));
	g_assert_nonnull(strstr(text, "Hello Dana <3"));
	g_assert_nonnull(strstr(text, "Open: https://shop.example.com/"));
	/* An invalid design never reaches the bodies. */
	g_object_set(template, "design", "{\"blocks\":[{\"type\":\"blink\"}]}", NULL);
	g_assert_false(try_save(f, template, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* A template without a design is the template it always was. */
	g_object_set(plain, "name", "Plain", "subject", "Hi {name}", "text-body", "Hello {name}", "html-body", "<p>{name}</p>", NULL);
	save(f, plain);
	body = string(plain, "html-body");
	g_assert_cmpstr(body, ==, "<p>{name}</p>");
}

/* A brand is checked when written: colours are #rrggbb, the font is one
 * of the stacks, the website is a web address, one brand per organization. */
static void test_brand(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) brand = record(f, "email_brand"), second = record(f, "email_brand");
	g_autoptr(VentureEmailLook) look = NULL, defaults = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	defaults = venture_email_look_load(f->context, f->org);
	g_assert_null(defaults->logo_url);
	g_assert_nonnull(defaults->primary);
	g_object_set(brand, "name", "Brightside", "primary-color", "orange", NULL);
	g_assert_false(try_save(f, brand, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(brand, "primary-color", "#C2410C", "font", "comic", NULL);
	g_assert_false(try_save(f, brand, &error));
	g_clear_error(&error);
	g_object_set(brand, "font", "serif", "website-url", "javascript:alert(1)", NULL);
	g_assert_false(try_save(f, brand, &error));
	g_clear_error(&error);
	g_object_set(brand, "website-url", "https://brightside.example.com", NULL);
	save(f, brand);
	look = venture_email_look_load(f->context, f->org);
	g_assert_cmpstr(look->name, ==, "Brightside");
	/* Stored as typed, used lowercase. */
	g_assert_cmpstr(look->primary, ==, "#c2410c");
	g_assert_cmpstr(look->font, ==, venture_email_font_stack("serif"));
	g_assert_cmpstr(look->website, ==, "https://brightside.example.com");
	g_object_set(second, "name", "Another", NULL);
	g_assert_false(try_save(f, second, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* Hosted images: type and size from the bytes, a size cap, an unguessable
 * address under the organization, and no way to make one but the store. */
static void test_hosted_image(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) image = NULL, forged = record(f, "hosted_image");
	g_autoptr(GBytes) svg = g_bytes_new_static("<svg xmlns=\"http://www.w3.org/2000/svg\"/>", 40);
	g_autoptr(GBytes) big = NULL, read = NULL, png = png_bytes(800, 400);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL, *token = NULL, *type = NULL, *file = NULL, *prefix = NULL;
	const gchar *content_type = NULL;
	gint64 width = 0, height = 0, id;
	guint i, run = 0;
	(void)unused;
	/* Named .jpg, declared nothing: it is a PNG because its bytes say so. */
	image = store_image(f, png, "photo.jpg", "A photo");
	id = venture_entity_get_id(image);
	g_object_get(image, "token", &token, "content-type", &type, "width", &width, "height", &height, NULL);
	g_assert_cmpstr(type, ==, "image/png");
	g_assert_cmpint(width, ==, 800);
	g_assert_cmpint(height, ==, 400);
	g_assert_cmpuint(strlen(token), >=, 32);
	/* Not a 32-digit hex run: the preview withholds those as bearer links,
	 * and an image is public, not a credential. */
	for (i = 0; token[i]; i++) {
		run = g_ascii_isxdigit(token[i]) ? run + 1 : 0;
		g_assert_cmpuint(run, <, 32);
	}
	url = venture_hosted_image_url(f->context, image);
	prefix = g_strdup_printf("https://books.example.com/i/%" G_GINT64_FORMAT "/%s.png", f->org, token);
	g_assert_cmpstr(url, ==, prefix);
	file = g_strdup_printf("%s.png", token);
	read = venture_hosted_image_read(f->context, f->org, file, &content_type, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(content_type, ==, "image/png");
	g_assert_true(g_bytes_equal(read, png));
	/* Another organization's address, a wrong extension, a wrong token. */
	g_assert_null(venture_hosted_image_read(f->context, f->org + 1000, file, &content_type, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_free(file); file = g_strdup_printf("%s.gif", token);
	g_assert_null(venture_hosted_image_read(f->context, f->org, file, &content_type, &error));
	g_clear_error(&error);
	g_assert_null(venture_hosted_image_read(f->context, f->org, "ghjkmnpqrstvwxyzghjkmnpqrstvwxyz.png", &content_type, &error));
	g_clear_error(&error);
	/* Not an image, and too big. */
	g_assert_null(venture_hosted_image_store(f->context, f->org, "logo.svg", svg, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	{
		guint8 *bytes = g_malloc0(VENTURE_HOSTED_IMAGE_MAX_BYTES + 1);
		memcpy(bytes, g_bytes_get_data(png, NULL), g_bytes_get_size(png));
		big = g_bytes_new_take(bytes, VENTURE_HOSTED_IMAGE_MAX_BYTES + 1);
	}
	g_assert_null(venture_hosted_image_store(f->context, f->org, "huge.png", big, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_bytes_unref(big);
	big = png_bytes(9000, 10);
	g_assert_null(venture_hosted_image_store(f->context, f->org, "wide.png", big, NULL, NULL, &error));
	g_clear_error(&error);
	/* The generic writers cannot make one with bytes it never had. */
	g_object_set(forged, "name", "Forged", "token", "ghjkmnpqrstvwxyzghjkmnpqrstvwxyz", "content-type", "image/png", NULL);
	g_assert_false(try_save(f, forged, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* Nor change what an existing one is. */
	g_object_set(image, "width", (gint64)1, NULL);
	g_assert_false(try_save(f, image, &error));
	g_clear_error(&error);
	/* A deleted image is no longer served. */
	g_clear_object(&image);
	image = venture_database_get(f->db, VENTURE_TYPE_HOSTED_IMAGE, id, NULL);
	g_assert_nonnull(image);
	g_assert_true(venture_database_delete(f->db, image, NULL, &error));
	g_assert_no_error(error);
	g_free(file); file = g_strdup_printf("%s.png", token);
	g_assert_null(venture_hosted_image_read(f->context, f->org, file, &content_type, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* Sections: a shared header or footer saved once and rendered in place
 * wherever a design names it; a cycle or a missing one is refused. */
static void test_sections(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) header = record(f, "email_section"), footer = record(f, "email_section");
	g_autoptr(VentureEntity) template = NULL, other = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *design = NULL, *html = NULL, *text = NULL, *loop = NULL;
	(void)unused;
	g_object_set(header, "name", "Shared header", "design",
		"{\"blocks\":[{\"type\":\"header\",\"tagline\":\"Shared tagline\"},{\"type\":\"divider\"}]}", NULL);
	save(f, header);
	design = g_strdup_printf("{\"blocks\":[{\"type\":\"section\",\"section_id\":%" G_GINT64_FORMAT "},"
		"{\"type\":\"text\",\"body\":\"Signed, the team\"}]}", venture_entity_get_id(header));
	g_object_set(footer, "name", "Signature", "design", design, NULL);
	save(f, footer);
	g_free(design);
	design = g_strdup_printf("{\"blocks\":[{\"type\":\"text\",\"heading\":\"Our news\",\"body\":\"Fall is here.\"},"
		"{\"type\":\"section\",\"section_id\":%" G_GINT64_FORMAT "}]}", venture_entity_get_id(footer));
	template = template_with(f, "News", design);
	html = string(template, "html-body");
	text = string(template, "text-body");
	g_assert_nonnull(strstr(html, "Shared tagline"));
	g_assert_nonnull(strstr(html, "Signed, the team"));
	g_assert_nonnull(strstr(html, "Our news"));
	g_assert_nonnull(strstr(text, "Shared tagline"));
	/* The header now names the signature, which names the header. */
	loop = g_strdup_printf("{\"blocks\":[{\"type\":\"section\",\"section_id\":%" G_GINT64_FORMAT "}]}", venture_entity_get_id(footer));
	g_object_set(header, "design", loop, NULL);
	g_assert_false(try_save(f, header, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* A section naming itself. */
	g_free(loop);
	loop = g_strdup_printf("{\"blocks\":[{\"type\":\"section\",\"section_id\":%" G_GINT64_FORMAT "}]}", venture_entity_get_id(header));
	g_object_set(header, "design", loop, NULL);
	g_assert_false(try_save(f, header, &error));
	g_clear_error(&error);
	/* A missing section refuses the template. */
	other = record(f, "mail_template");
	g_object_set(other, "name", "Broken", "subject", "x", "design", "{\"blocks\":[{\"type\":\"section\",\"section_id\":99999}]}", NULL);
	g_assert_false(try_save(f, other, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* The designed email is a preview kind: lane 1's service shows it, and
 * every one of its checks passes. */
static void test_preview(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) template = NULL;
	g_autoptr(VentureMailPreview) preview = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *design = NULL;
	guint i;
	(void)unused;
	seed_heating(f);
	design = g_strdup_printf("{\"blocks\":[{\"type\":\"header\"},{\"type\":\"image\",\"image_id\":%" G_GINT64_FORMAT ",\"alt\":\"A furnace\"},"
		"{\"type\":\"text\",\"heading\":\"Fall tune-ups\",\"body\":\"Book a visit before the cold arrives.\"},"
		"{\"type\":\"button\",\"label\":\"Book\",\"url\":\"https://brightside.example.com/tune-up\"},{\"type\":\"footer\"}]}", f->photo);
	template = template_with(f, "Preview me", design);
	g_assert_nonnull(venture_mail_preview_lookup("email-design"));
	preview = venture_mail_preview_build(f->context, "email-design", f->org, venture_entity_get_id(template), NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(preview);
	assert_email_safe(preview->html, preview->text);
	g_assert_nonnull(strstr(preview->html, "https://books.example.com/i/"));
	for (i = 0; i < preview->checks->len; i++) {
		const VentureMailCheck *check = g_ptr_array_index(preview->checks, i);
		if (check->status != VENTURE_MAIL_CHECK_OK)
			g_error("check %s: %s", check->id, check->message);
	}
	g_assert_cmpuint(preview->checks->len, >=, 5);
}

/* The designer from a brief: brand, logo, products and photos go to the
 * model as data on the toolless path; what comes back is validated,
 * grounded and saved as version 1, and it previews clean. */
static void test_ai_brief(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEmailDesignResult) result = NULL;
	g_autoptr(VentureMailPreview) preview = NULL;
	g_autoptr(GPtrArray) versions = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reply = NULL, *design = NULL, *subject = NULL, *html = NULL, *source = NULL, *note = NULL;
	gint64 number = 0;
	guint i;
	(void)unused;
	seed_heating(f);
	reply = heating_reply(f, "medium", NULL);
	answer(f, reply);
	result = venture_email_design_from_brief(f->context, f->org, 0, NULL, brief, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(f->provider->calls, ==, 1);
	/* What the model was shown: the brief, and the records as data. */
	g_assert_nonnull(strstr(f->provider->prompt, brief));
	g_assert_nonnull(strstr(f->provider->prompt, "BEGIN RECORDS"));
	g_assert_nonnull(strstr(f->provider->prompt, "Furnace tune-up"));
	g_assert_nonnull(strstr(f->provider->prompt, "129.00"));
	g_assert_nonnull(strstr(f->provider->prompt, "Filter replacement"));
	g_assert_nonnull(strstr(f->provider->prompt, "https://brightside.example.com/tune-up"));
	g_assert_nonnull(strstr(f->provider->prompt, "#c2410c"));
	g_assert_nonnull(strstr(f->provider->system, "image_id"));
	g_assert_cmpuint(result->stripped->len, ==, 0);
	/* The brand has a logo: nothing to ask. */
	g_assert_null(result->ask);
	design = string(result->template, "design");
	subject = string(result->template, "subject");
	html = string(result->template, "html-body");
	g_assert_true(venture_email_design_validate(design, NULL));
	g_assert_cmpstr(subject, ==, "Fall furnace tune-up: 10% off until Nov 15");
	g_assert_nonnull(strstr(html, "background:#c2410c"));
	g_assert_nonnull(strstr(html, "$129.00"));
	g_object_get(result->version, "number", &number, "source", &source, "note", &note, NULL);
	g_assert_cmpint(number, ==, 1);
	g_assert_cmpstr(source, ==, "ai");
	g_assert_cmpstr(note, ==, brief);
	versions = rows(f, "email_design_version");
	g_assert_cmpuint(versions->len, ==, 1);
	preview = venture_mail_preview_build(f->context, "email-design", f->org, venture_entity_get_id(result->template), NULL, NULL, &error);
	g_assert_no_error(error);
	assert_email_safe(preview->html, preview->text);
	g_assert_nonnull(strstr(preview->text, "Book your tune-up: https://brightside.example.com/tune-up"));
	for (i = 0; i < preview->checks->len; i++) {
		const VentureMailCheck *check = g_ptr_array_index(preview->checks, i);
		if (check->status != VENTURE_MAIL_CHECK_OK)
			g_error("check %s: %s", check->id, check->message);
	}
}

/* An invented price is caught in code: a sentence carrying one is removed
 * and reported; one on the button refuses the whole design and writes
 * nothing. A design the validator refuses is refused the same way. */
static void test_ai_grounding(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEmailDesignResult) result = NULL, refused = NULL, invalid = NULL;
	g_autoptr(GPtrArray) templates = NULL, versions = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reply = NULL, *design = NULL, *label = NULL;
	(void)unused;
	seed_heating(f);
	reply = heating_reply(f, "medium", " Our lowest price ever: only $99 for a full tune-up!");
	answer(f, reply);
	result = venture_email_design_from_brief(f->context, f->org, 0, "Fall", brief, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(result->stripped->len, ==, 1);
	g_assert_nonnull(strstr(g_ptr_array_index(result->stripped, 0), "$99"));
	design = string(result->template, "design");
	g_assert_null(strstr(design, "$99"));
	g_assert_nonnull(strstr(design, "save 10%"));
	label = g_strdup_printf("{\"subject\":\"Save $50\",\"design\":{\"blocks\":[{\"type\":\"header\"},"
		"{\"type\":\"button\",\"label\":\"Save $50 today\",\"url\":\"https://brightside.example.com/tune-up\"}]}}");
	answer(f, label);
	refused = venture_email_design_from_brief(f->context, f->org, 0, "Refused", brief, NULL, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "$50"));
	g_clear_error(&error);
	answer(f, "{\"subject\":\"Hi\",\"design\":{\"blocks\":[{\"type\":\"script\",\"body\":\"alert(1)\"}]}}");
	invalid = venture_email_design_from_brief(f->context, f->org, 0, "Invalid", brief, NULL, &error);
	g_assert_null(invalid);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* An address the records never gave is refused too. */
	answer(f, "{\"subject\":\"Hi\",\"design\":{\"blocks\":[{\"type\":\"button\",\"label\":\"Book\",\"url\":\"https://elsewhere.example.net/\"}]}}");
	g_assert_null(venture_email_design_from_brief(f->context, f->org, 0, "Elsewhere", brief, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	templates = rows(f, "mail_template");
	versions = rows(f, "email_design_version");
	g_assert_cmpuint(templates->len, ==, 1);
	g_assert_cmpuint(versions->len, ==, 1);
}

static const gchar plain_reply[] =
	"{\"subject\":\"Fall furnace tune-ups are here\",\"design\":{\"blocks\":["
	"{\"type\":\"header\"},"
	"{\"type\":\"text\",\"heading\":\"Fall furnace tune-up: 10% off\",\"body\":\"Get your furnace ready before the cold sets in.\\n\\nThe offer ends Nov 15. Reply to this email to book a visit.\"},"
	"{\"type\":\"footer\"}]}}";

/* No logo, no brand: a clean plain design with the defaults, and one
 * request for a logo -- asked once per organization, never again once
 * asked, dismissed or answered. */
static void test_ai_no_assets(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEmailDesignResult) first = NULL, second = NULL, third = NULL;
	g_autoptr(VentureEmailLook) defaults = venture_email_look_new_default(NULL);
	g_autoptr(GPtrArray) brands = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) organization = NULL, brand = NULL, logo = NULL;
	g_autofree gchar *html = NULL, *asked = NULL;
	(void)unused;
	answer(f, plain_reply);
	first = venture_email_design_from_brief(f->context, f->org, 0, NULL, brief, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(first->ask);
	g_assert_nonnull(strstr(first->ask, "logo"));
	html = string(first->template, "html-body");
	g_assert_nonnull(strstr(html, defaults->primary));
	g_assert_null(strstr(html, "<img"));
	brands = rows(f, "email_brand");
	g_assert_cmpuint(brands->len, ==, 1);
	asked = string(g_ptr_array_index(brands, 0), "logo-ask");
	g_assert_cmpstr(asked, ==, "asked");
	answer(f, plain_reply);
	second = venture_email_design_from_brief(f->context, f->org, 0, NULL, brief, NULL, &error);
	g_assert_no_error(error);
	g_assert_null(second->ask);
	/* A second organization dismisses the request before it is made. */
	organization = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Second Example Co", NULL);
	save(f, organization);
	g_assert_true(venture_email_design_dismiss_logo_ask(f->context, venture_entity_get_id(organization), NULL, &error));
	g_assert_no_error(error);
	answer(f, plain_reply);
	third = venture_email_design_from_brief(f->context, venture_entity_get_id(organization), 0, NULL, brief, NULL, &error);
	g_assert_no_error(error);
	g_assert_null(third->ask);
	/* Uploading a logo answers the request. */
	brand = g_object_ref(g_ptr_array_index(brands, 0));
	logo = store_image(f, png_bytes(300, 100), "logo.png", "Logo");
	g_object_set(brand, "logo-image-id", venture_entity_get_id(logo), NULL);
	save(f, brand);
	g_clear_object(&brand);
	g_clear_pointer(&brands, g_ptr_array_unref);
	brands = rows(f, "email_brand");
	g_free(asked);
	asked = string(g_ptr_array_index(brands, 0), "logo-ask");
	g_assert_cmpstr(asked, ==, "answered");
}

/* Edits by words are versions, and undo puts back the previous version
 * byte for byte -- the design and the bodies built from it. */
static void test_ai_edit_undo(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEmailDesignResult) made = NULL, edited = NULL;
	g_autoptr(VentureEntity) current = NULL, restored = NULL, again = NULL;
	g_autoptr(GPtrArray) versions = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reply = NULL, *bigger = NULL, *design1 = NULL, *html1 = NULL, *text1 = NULL;
	g_autofree gchar *design2 = NULL, *design3 = NULL, *html3 = NULL, *text3 = NULL, *source = NULL;
	gint64 template_id, number = 0;
	(void)unused;
	seed_heating(f);
	reply = heating_reply(f, "medium", NULL);
	answer(f, reply);
	made = venture_email_design_from_brief(f->context, f->org, 0, NULL, brief, NULL, &error);
	g_assert_no_error(error);
	template_id = venture_entity_get_id(made->template);
	design1 = string(made->template, "design");
	html1 = string(made->template, "html-body");
	text1 = string(made->template, "text-body");
	bigger = heating_reply(f, "large", NULL);
	answer(f, bigger);
	edited = venture_email_design_edit(f->context, f->org, template_id, "make the button bigger", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(f->provider->prompt, "make the button bigger"));
	g_assert_nonnull(strstr(f->provider->prompt, "\"size\":\"medium\""));
	g_object_get(edited->version, "number", &number, "source", &source, NULL);
	g_assert_cmpint(number, ==, 2);
	g_assert_cmpstr(source, ==, "ai-edit");
	design2 = string(edited->template, "design");
	g_assert_nonnull(strstr(design2, "\"size\":\"large\""));
	g_assert_cmpstr(design1, !=, design2);
	restored = venture_email_design_undo(f->context, f->org, template_id, NULL, &error);
	g_assert_no_error(error);
	g_object_get(restored, "number", &number, NULL);
	g_assert_cmpint(number, ==, 1);
	current = venture_database_get(f->db, VENTURE_TYPE_MAIL_TEMPLATE, template_id, NULL);
	design3 = string(current, "design");
	html3 = string(current, "html-body");
	text3 = string(current, "text-body");
	g_assert_cmpstr(design3, ==, design1);
	g_assert_cmpstr(html3, ==, html1);
	g_assert_cmpstr(text3, ==, text1);
	/* Nothing before version 1. */
	again = venture_email_design_undo(f->context, f->org, template_id, NULL, &error);
	g_assert_null(again);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	versions = rows(f, "email_design_version");
	g_assert_cmpuint(versions->len, ==, 2);
	/* The history is the service's alone. */
	g_clear_error(&error);
	g_object_set(g_ptr_array_index(versions, 0), "design", "{\"blocks\":[]}", NULL);
	g_assert_false(try_save(f, g_ptr_array_index(versions, 0), &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* The chat assistant reaches the designer through the action registry:
 * the actions are tools it is offered, always staged, and running the
 * action is the same service call. */
static void test_ai_tool(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) tools = NULL, input = NULL;
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureEntity) template = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *listed = NULL, *body = NULL, *design = NULL;
	(void)unused;
	seed_heating(f);
	answer(f, plain_reply);
	tools = venture_ai_service_describe_tools(f->ai);
	listed = json_to_string(tools, FALSE);
	g_assert_nonnull(strstr(listed, "venture_mail_template_design_email"));
	g_assert_nonnull(strstr(listed, "venture_mail_template_edit_design"));
	g_assert_nonnull(strstr(listed, "venture_mail_template_undo_design"));
	body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"brief\":\"%s\"}", f->org, brief);
	input = json_from_string(body, &error);
	g_assert_no_error(error);
	params = venture_action_parameters_from_json(input, &error);
	g_assert_no_error(error);
	template = venture_action_registry_perform(venture_database_get_action_registry(f->db), "mail_template", 0,
		"design_email", params, NULL, VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	g_assert_nonnull(template);
	design = string(template, "design");
	g_assert_true(venture_email_design_validate(design, NULL));
	g_assert_cmpuint(f->provider->calls, ==, 1);
}

/* --- The pages, with real sign-in --------------------------------------------------- */

typedef struct {
	Fixture base;
	VentureWebServer *server;
	SoupSession *session;
	guint port;
	gint64 template;
} ServerFixture;
typedef struct { gboolean done; GBytes *body; GError *error; } RequestResult;
static void request_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RequestResult *outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &outcome->error);
	outcome->done = TRUE;
}
static guint http_full(ServerFixture *s, const gchar *method, const gchar *path, const gchar *cookie,
	const gchar *content_type, GBytes *payload, GBytes **out_body, SoupMessageHeaders **out_headers)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", s->port, path);
	RequestResult outcome = { FALSE, NULL, NULL };
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (cookie != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	if (payload != NULL)
		soup_message_set_request_body_from_bytes(message, content_type, payload);
	soup_session_send_and_read_async(s->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	if (outcome.error != NULL)
		g_error("%s %s: %s", method, path, outcome.error->message);
	if (out_body != NULL)
		*out_body = g_bytes_ref(outcome.body);
	if (out_headers != NULL)
		*out_headers = g_boxed_copy(SOUP_TYPE_MESSAGE_HEADERS, soup_message_get_response_headers(message));
	g_clear_pointer(&outcome.body, g_bytes_unref);
	return soup_message_get_status(message);
}
static guint http(ServerFixture *s, const gchar *method, const gchar *path, const gchar *cookie,
	const gchar *form, gchar **out_body, gchar **out_cookie)
{
	g_autoptr(GBytes) payload = form != NULL ? g_bytes_new(form, strlen(form)) : NULL;
	g_autoptr(GBytes) body = NULL;
	SoupMessageHeaders *headers = NULL;
	guint status = http_full(s, method, path, cookie, "application/x-www-form-urlencoded", payload, &body, &headers);
	if (out_body != NULL)
		*out_body = g_strndup(g_bytes_get_data(body, NULL), g_bytes_get_size(body));
	if (out_cookie != NULL) {
		const gchar *set = soup_message_headers_get_one(headers, "Set-Cookie");
		*out_cookie = set ? g_strndup(set, strcspn(set, ";")) : NULL;
	}
	g_boxed_free(SOUP_TYPE_MESSAGE_HEADERS, headers);
	return status;
}
static gchar *login(ServerFixture *s, const gchar *username, const gchar *password)
{
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	gchar *cookie = NULL;
	g_assert_cmpuint(http(s, "POST", "/login", NULL, form, NULL, &cookie), ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(cookie);
	return cookie;
}
static guint upload(ServerFixture *s, const gchar *cookie, GBytes *image, const gchar *extra, gchar **out_body)
{
	g_autoptr(GByteArray) body = g_byte_array_new();
	g_autoptr(GBytes) payload = NULL, answer_body = NULL;
	g_autofree gchar *head = g_strdup_printf("--XBOUNDARYX\r\nContent-Disposition: form-data; name=\"organization_id\"\r\n\r\n%" G_GINT64_FORMAT
		"\r\n--XBOUNDARYX\r\nContent-Disposition: form-data; name=\"alt\"\r\n\r\nOur logo\r\n%s"
		"--XBOUNDARYX\r\nContent-Disposition: form-data; name=\"file\"; filename=\"logo.webp\"\r\nContent-Type: image/webp\r\n\r\n",
		s->base.org, extra != NULL ? extra : "");
	guint status;
	g_byte_array_append(body, (const guint8 *)head, strlen(head));
	g_byte_array_append(body, g_bytes_get_data(image, NULL), g_bytes_get_size(image));
	g_byte_array_append(body, (const guint8 *)"\r\n--XBOUNDARYX--\r\n", 18);
	payload = g_byte_array_free_to_bytes(g_steal_pointer(&body));
	status = http_full(s, "POST", "/email-design/images", cookie, "multipart/form-data; boundary=XBOUNDARYX", payload, &answer_body, NULL);
	if (out_body != NULL)
		*out_body = g_strndup(g_bytes_get_data(answer_body, NULL), g_bytes_get_size(answer_body));
	return status;
}
static void server_setup(ServerFixture *s, gconstpointer unused)
{
	Fixture *f = &s->base;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) owner = venture_user_new();
	g_autoptr(VentureUser) editor = venture_user_new();
	g_autoptr(VentureEntity) member = NULL, template = NULL;
	gboolean ok;
	setup(f, unused);
	seed_heating(f);
	answer(f, "unused");
	g_free(g_queue_pop_head(f->provider->answers));
	g_setenv("VENTURE_TEST_SESSION_SECRET", "email-design-test-secret", TRUE);
	g_object_set(f->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
		"security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", "security-password-iterations", (gint64)100000, NULL);
	g_object_set(owner, "username", "owner", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(owner, "owner-password-1", 100000, NULL));
	ok = venture_orgaccess_bootstrap_owner(f->db, owner, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	g_object_set(editor, "username", "editor", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(editor, "editor-password-1", 100000, NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(editor), NULL, &error));
	g_assert_no_error(error);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(VENTURE_ENTITY(editor)),
		"organization-id", f->org, "role", VENTURE_ORGANIZATION_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, member, NULL, &error));
	g_assert_no_error(error);
	template = template_with(f, "Fall news", "{\"blocks\":[{\"type\":\"header\"},{\"type\":\"text\",\"heading\":\"Hello\",\"body\":\"Fall is here and so are we.\"}]}");
	s->template = venture_entity_get_id(template);
	s->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(s->server, &error));
	g_assert_no_error(error);
	s->port = venture_web_server_get_port(s->server);
	s->session = soup_session_new();
}
static void server_teardown(ServerFixture *s, gconstpointer unused)
{
	venture_web_server_stop(s->server);
	g_clear_object(&s->session);
	g_clear_object(&s->server);
	teardown(&s->base, unused);
}
static guint count(const gchar *haystack, const gchar *needle)
{
	guint n = 0;
	const gchar *p = haystack;
	while ((p = strstr(p, needle)) != NULL) { n++; p += strlen(needle); }
	return n;
}
static guint version_count(ServerFixture *s)
{
	g_autoptr(GPtrArray) versions = rows(&s->base, "email_design_version");
	return versions->len;
}

/* Upload and the public address: owner or administrator only to upload;
 * anybody, with no cookie at all, to fetch -- mail apps send none -- and
 * only under the image's own organization. */
static void test_page_images(ServerFixture *s, gconstpointer unused)
{
	g_autoptr(GBytes) image = webp_bytes("VP8X", 400, 200), served = NULL;
	g_autoptr(JsonNode) node = NULL;
	SoupMessageHeaders *headers = NULL;
	g_autofree gchar *owner = NULL, *editor = NULL, *body = NULL, *path = NULL, *wrong = NULL, *api = NULL;
	const gchar *url;
	(void)unused;
	g_assert_cmpuint(upload(s, NULL, image, NULL, NULL), ==, SOUP_STATUS_FOUND);
	editor = login(s, "editor", "editor-password-1");
	g_assert_cmpuint(upload(s, editor, image, NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	owner = login(s, "owner", "owner-password-1");
	g_assert_cmpuint(upload(s, owner, image, NULL, &body), ==, SOUP_STATUS_CREATED);
	node = json_from_string(body, NULL);
	g_assert_nonnull(node);
	g_assert_cmpstr(venture_json_object_get_string(json_node_get_object(node), "content_type", NULL), ==, "image/webp");
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(node), "width", 0), ==, 400);
	url = venture_json_object_get_string(json_node_get_object(node), "url", NULL);
	g_assert_true(g_str_has_prefix(url, "https://books.example.com/i/"));
	path = g_strdup(url + strlen("https://books.example.com"));
	g_assert_cmpuint(http_full(s, "GET", path, NULL, NULL, NULL, &served, &headers), ==, SOUP_STATUS_OK);
	g_assert_true(g_bytes_equal(served, image));
	g_assert_cmpstr(soup_message_headers_get_content_type(headers, NULL), ==, "image/webp");
	g_assert_nonnull(strstr(soup_message_headers_get_one(headers, "Cache-Control"), "public"));
	g_assert_nonnull(strstr(soup_message_headers_get_one(headers, "Cache-Control"), "max-age="));
	g_assert_cmpstr(soup_message_headers_get_one(headers, "X-Content-Type-Options"), ==, "nosniff");
	g_assert_null(soup_message_headers_get_one(headers, "Set-Cookie"));
	g_boxed_free(SOUP_TYPE_MESSAGE_HEADERS, headers);
	wrong = g_strdup_printf("/i/%" G_GINT64_FORMAT "%s", s->base.org + 77, strchr(path + 3, '/'));
	g_assert_cmpuint(http(s, "GET", wrong, NULL, NULL, NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(http(s, "GET", "/i/1/ghjkmnpqrstvwxyzghjkmnpqrstvwxyz.png", NULL, NULL, NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
	/* The brand is an owner's or administrator's to change, whichever door. */
	{
		g_autoptr(GPtrArray) brands = rows(&s->base, "email_brand");
		api = g_strdup_printf("/api/v1/email_brand/%" G_GINT64_FORMAT, venture_entity_get_id(g_ptr_array_index(brands, 0)));
	}
	{
		g_autoptr(GBytes) json = g_bytes_new_static("{\"primary_color\":\"#000000\"}", 27);
		g_assert_cmpuint(http_full(s, "PUT", api, editor, "application/json", json, NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
		g_assert_cmpuint(http_full(s, "PUT", api, owner, "application/json", json, NULL, NULL), ==, SOUP_STATUS_OK);
	}
}

/* The builder: owner or administrator only; blocks added, moved, edited
 * and removed, each a version; lane 1's frames show it; the brief box and
 * undo work from the page. */
static void test_page_builder(ServerFixture *s, gconstpointer unused)
{
	g_autofree gchar *owner = NULL, *editor = NULL, *page = NULL, *path = NULL, *blocks = NULL, *brief_path = NULL;
	g_autofree gchar *undo = NULL, *reply = NULL, *form = NULL, *design = NULL;
	g_autoptr(VentureEntity) template = NULL;
	guint before;
	(void)unused;
	path = g_strdup_printf("/email-design/%" G_GINT64_FORMAT, s->template);
	blocks = g_strdup_printf("%s/blocks", path);
	brief_path = g_strdup_printf("%s/brief", path);
	undo = g_strdup_printf("%s/undo", path);
	g_assert_cmpuint(http(s, "GET", path, NULL, NULL, NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(http(s, "GET", "/email-design", NULL, NULL, NULL, NULL), ==, SOUP_STATUS_FOUND);
	editor = login(s, "editor", "editor-password-1");
	g_assert_cmpuint(http(s, "GET", path, editor, NULL, NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(http(s, "POST", blocks, editor, "op=add&type=divider", NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	owner = login(s, "owner", "owner-password-1");
	g_assert_cmpuint(http(s, "GET", "/email-design", owner, NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "Fall news"));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(http(s, "GET", path, owner, NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "id=\"email-builder\""));
	g_assert_cmpuint(count(page, "data-block-index="), ==, 2);
	g_assert_cmpuint(count(page, "class=\"mail-preview-frame\""), >=, 2);
	g_assert_cmpuint(count(page, "sandbox=\"\""), >=, 2);
	g_assert_nonnull(strstr(page, "hx-post="));
	g_assert_nonnull(strstr(page, "name=\"brief\""));
	before = version_count(s);
	g_assert_cmpuint(http(s, "POST", blocks, owner, "op=add&type=divider", NULL, NULL), ==, SOUP_STATUS_SEE_OTHER);
	g_assert_cmpuint(http(s, "POST", blocks, owner, "op=add&type=button", NULL, NULL), ==, SOUP_STATUS_SEE_OTHER);
	g_assert_cmpuint(http(s, "POST", blocks, owner, "op=up&index=3", NULL, NULL), ==, SOUP_STATUS_SEE_OTHER);
	form = g_strdup("op=edit&index=1&block=%7B%22type%22%3A%22text%22%2C%22heading%22%3A%22Hello+again%22%2C%22body%22%3A%22Edited+by+hand.%22%7D");
	g_assert_cmpuint(http(s, "POST", blocks, owner, form, NULL, NULL), ==, SOUP_STATUS_SEE_OTHER);
	g_assert_cmpuint(http(s, "POST", blocks, owner, "op=remove&index=0", NULL, NULL), ==, SOUP_STATUS_SEE_OTHER);
	g_assert_cmpuint(version_count(s), ==, before + 5);
	/* A bad edit is refused and changes nothing. */
	g_assert_cmpuint(http(s, "POST", blocks, owner, "op=edit&index=0&block=%7B%22type%22%3A%22blink%22%7D", NULL, NULL), ==, SOUP_STATUS_UNPROCESSABLE_ENTITY);
	g_assert_cmpuint(version_count(s), ==, before + 5);
	template = venture_database_get(s->base.db, VENTURE_TYPE_MAIL_TEMPLATE, s->template, NULL);
	design = string(template, "design");
	g_assert_nonnull(strstr(design, "Edited by hand."));
	g_assert_null(strstr(design, "\"header\""));
	g_assert_true(strstr(design, "\"button\"") < strstr(design, "\"divider\""));
	/* The brief box. */
	reply = heating_reply(&s->base, "medium", NULL);
	answer(&s->base, reply);
	g_assert_cmpuint(http(s, "POST", brief_path, owner, "brief=fall+furnace+tune-up%2C+10%25+off%2C+ends+Nov+15", NULL, NULL), ==, SOUP_STATUS_SEE_OTHER);
	g_assert_cmpuint(version_count(s), ==, before + 6);
	g_clear_object(&template);
	template = venture_database_get(s->base.db, VENTURE_TYPE_MAIL_TEMPLATE, s->template, NULL);
	g_free(design); design = string(template, "design");
	g_assert_nonnull(strstr(design, "Book your tune-up"));
	g_assert_cmpuint(http(s, "POST", undo, owner, "", NULL, NULL), ==, SOUP_STATUS_SEE_OTHER);
	g_clear_object(&template);
	template = venture_database_get(s->base.db, VENTURE_TYPE_MAIL_TEMPLATE, s->template, NULL);
	g_free(design); design = string(template, "design");
	g_assert_nonnull(strstr(design, "Edited by hand."));
	/* The designed email previews through lane 1's page. */
	g_clear_pointer(&page, g_free);
	g_free(path); path = g_strdup_printf("/mail/preview/email-design/%" G_GINT64_FORMAT, s->template);
	g_assert_cmpuint(http(s, "GET", path, owner, NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_cmpuint(count(page, "<iframe"), ==, 6);
}

/* --- Pictures ------------------------------------------------------------------------- */

/* Replaces every hosted image address in @html with the image's bytes as
 * a data address, so a file written for a screenshot shows them. */
static gchar *inline_images(Fixture *f, const gchar *html)
{
	g_autoptr(GPtrArray) images = rows(f, "hosted_image");
	gchar *out = g_strdup(html);
	guint i;
	for (i = 0; i < images->len; i++) {
		VentureEntity *image = g_ptr_array_index(images, i);
		g_autofree gchar *url = venture_hosted_image_url(f->context, image), *token = string(image, "token");
		g_autofree gchar *type = string(image, "content-type"), *file = NULL, *encoded = NULL, *data = NULL;
		g_autoptr(GBytes) bytes = NULL;
		g_auto(GStrv) parts = NULL;
		const gchar *content_type = NULL;
		file = g_strdup_printf("%s", strrchr(url, '/') + 1);
		bytes = venture_hosted_image_read(f->context, venture_entity_get_organization_id(image), file, &content_type, NULL);
		if (bytes == NULL) continue;
		encoded = g_base64_encode(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
		data = g_strdup_printf("data:%s;base64,%s", type, encoded);
		parts = g_strsplit(out, url, -1);
		g_free(out);
		out = g_strjoinv(data, parts);
	}
	return out;
}
static void write_shot(const gchar *directory, const gchar *name, const gchar *contents)
{
	g_autofree gchar *path = g_build_filename(directory, name, NULL);
	g_assert_true(g_file_set_contents(path, contents, -1, NULL));
}

/* Writes the HTML the screenshots are taken from when asked to, and is
 * skipped otherwise: the designed email from the brief for the example
 * heating business, the same brief with no assets, a video email and the
 * builder page. */
static void test_shots(ServerFixture *s, gconstpointer unused)
{
	const gchar *directory = g_getenv("VENTURE_EMAIL_DESIGN_SHOTS");
	g_autoptr(VentureEmailDesignResult) designed = NULL, plain = NULL;
	g_autoptr(VentureMailPreview) preview = NULL, plain_preview = NULL, video_preview = NULL;
	g_autoptr(VentureEntity) organization = NULL, video = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reply = NULL, *owner = NULL, *page = NULL, *path = NULL, *html = NULL, *design = NULL;
	(void)unused;
	if (directory == NULL) {
		g_test_skip("VENTURE_EMAIL_DESIGN_SHOTS is not set");
		return;
	}
	g_assert_true(g_mkdir_with_parents(directory, 0755) == 0);
	reply = heating_reply(&s->base, "large", NULL);
	answer(&s->base, reply);
	designed = venture_email_design_from_brief(s->base.context, s->base.org, s->template, NULL, brief, NULL, &error);
	g_assert_no_error(error);
	preview = venture_mail_preview_build(s->base.context, "email-design", s->base.org, s->template, NULL, NULL, &error);
	g_assert_no_error(error);
	html = inline_images(&s->base, preview->html);
	write_shot(directory, "designed.html", html);
	g_free(html);
	/* The same brief for a business with nothing uploaded. */
	organization = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Brightside Heating & Air", NULL);
	save(&s->base, organization);
	answer(&s->base, plain_reply);
	plain = venture_email_design_from_brief(s->base.context, venture_entity_get_id(organization), 0, NULL, brief, NULL, &error);
	g_assert_no_error(error);
	plain_preview = venture_mail_preview_build(s->base.context, "email-design", venture_entity_get_id(organization),
		venture_entity_get_id(plain->template), NULL, NULL, &error);
	g_assert_no_error(error);
	write_shot(directory, "no-assets.html", plain_preview->html);
	write_shot(directory, "no-assets-ask.txt", plain->ask != NULL ? plain->ask : "");
	/* A video email. */
	design = g_strdup_printf("{\"preheader\":\"Two minutes on what a tune-up covers.\",\"blocks\":[{\"type\":\"header\"},"
		"{\"type\":\"text\",\"heading\":\"See what a tune-up covers\",\"body\":\"Our technicians walk through every step of a fall furnace tune-up.\"},"
		"{\"type\":\"video\",\"url\":\"https://brightside.example.com/videos/tune-up\",\"poster_image_id\":%" G_GINT64_FORMAT ","
		"\"alt\":\"A furnace being cleaned\",\"caption\":\"Watch: what we check in a tune-up (2 minutes)\"},"
		"{\"type\":\"button\",\"label\":\"Book your tune-up\",\"url\":\"https://brightside.example.com/tune-up\"},{\"type\":\"footer\"}]}", s->base.poster);
	video = template_with(&s->base, "Video", design);
	video_preview = venture_mail_preview_build(s->base.context, "email-design", s->base.org, venture_entity_get_id(video), NULL, NULL, &error);
	g_assert_no_error(error);
	html = inline_images(&s->base, video_preview->html);
	write_shot(directory, "video.html", html);
	g_free(html);
	/* The builder page, signed in. */
	owner = login(s, "owner", "owner-password-1");
	path = g_strdup_printf("/email-design/%" G_GINT64_FORMAT, s->template);
	g_assert_cmpuint(http(s, "GET", path, owner, NULL, &page, NULL), ==, SOUP_STATUS_OK);
	html = inline_images(&s->base, page);
	write_shot(directory, "builder.html", html);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/email-design/validate/pass", test_validate_pass);
	g_test_add_func("/email-design/validate/fail", test_validate_fail);
	g_test_add_func("/email-design/render/structure", test_render_structure);
	g_test_add_func("/email-design/render/dark", test_render_dark);
	g_test_add_func("/email-design/render/brand", test_render_brand);
	g_test_add_func("/email-design/render/video", test_render_video);
	g_test_add_func("/email-design/render/unresolved", test_render_unresolved);
	g_test_add_func("/email-design/sniff", test_sniff);
	g_test_add_func("/email-design/ground", test_ground);
	g_test_add("/email-design/template", Fixture, NULL, setup, test_template, teardown);
	g_test_add("/email-design/brand", Fixture, NULL, setup, test_brand, teardown);
	g_test_add("/email-design/hosted-image", Fixture, NULL, setup, test_hosted_image, teardown);
	g_test_add("/email-design/sections", Fixture, NULL, setup, test_sections, teardown);
	g_test_add("/email-design/preview", Fixture, NULL, setup, test_preview, teardown);
	g_test_add("/email-design/ai/brief", Fixture, NULL, setup, test_ai_brief, teardown);
	g_test_add("/email-design/ai/grounding", Fixture, NULL, setup, test_ai_grounding, teardown);
	g_test_add("/email-design/ai/no-assets", Fixture, NULL, setup, test_ai_no_assets, teardown);
	g_test_add("/email-design/ai/edit-undo", Fixture, NULL, setup, test_ai_edit_undo, teardown);
	g_test_add("/email-design/ai/tool", Fixture, NULL, setup, test_ai_tool, teardown);
	g_test_add("/email-design/page/images", ServerFixture, NULL, server_setup, test_page_images, server_teardown);
	g_test_add("/email-design/page/builder", ServerFixture, NULL, server_setup, test_page_builder, server_teardown);
	g_test_add("/email-design/shots", ServerFixture, NULL, server_setup, test_shots, server_teardown);
	return g_test_run();
}
