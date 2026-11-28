/*
 * test-markdown.c - The markdown a comment is written in
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture_markdown_to_html() turns a stranger's text into a page's markup,
 * so most of these tests are about what it must never do: let a tag, an
 * attribute or a javascript: address through. The rest pin the subset a
 * comment box is actually typed in, so a change to the parser shows up as
 * a changed page here before it shows up on somebody's invoice.
 */

#include <venture.h>

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static gchar *
render(const gchar *text)
{
	return venture_markdown_to_html(text, NULL);
}

static void
assert_render(
	const gchar	*text,
	const gchar	*expected
){
	g_autofree gchar *html = render(text);

	if (0 != g_strcmp0(html, expected))
		g_error("%s\n  rendered %s\n  expected %s", text, html, expected);
}

/* The page must never carry a tag or an attribute the renderer did not
 * write itself. A crude but exhaustive check: every "<" in the output
 * opens one of the renderer's own tags. */
static void
assert_only_own_tags(const gchar *html)
{
	static const gchar *const own[] = {
		"p>", "/p>", "br>", "strong>", "/strong>", "em>", "/em>", "del>",
		"/del>", "code>", "code class=\"language-", "/code>", "pre>", "/pre>",
		"ul>", "/ul>", "ol>", "ol start=\"", "/ol>", "li>", "li class=\"task\">",
		"/li>", "input type=\"checkbox\" disabled", "blockquote>",
		"/blockquote>", "h3>", "/h3>", "h4>", "/h4>", "h5>", "/h5>", "h6>",
		"/h6>", "hr>", "div class=\"md-table\">", "/div>", "table>",
		"/table>", "thead>", "/thead>", "tbody>", "/tbody>", "tr>", "/tr>",
		"th>", "th class=\"md-", "/th>", "td>", "td class=\"md-", "/td>",
		"a href=\"", "/a>", "span class=\"mention\" title=\"", "/span>", NULL
	};
	const gchar *p;

	for (p = strchr(html, '<'); NULL != p; p = strchr(p + 1, '<'))
	{
		gsize i;
		gboolean known = FALSE;

		for (i = 0; NULL != own[i]; i++)
		{
			if (g_str_has_prefix(p + 1, own[i]))
				known = TRUE;
		}

		if (!known)
			g_error("a tag the renderer did not write: %.40s", p);
	}
}

/*
 * The escape comes first. If this regresses, a comment can run script in
 * the browser of everybody who opens the record -- the owner included.
 */
static void
test_html_is_text(void)
{
	static const gchar *const attacks[] = {
		"<script>alert(1)</script>",
		"<img src=x onerror=alert(1)>",
		"<a href=\"javascript:alert(1)\">x</a>",
		"**<b onclick=alert(1)>bold</b>**",
		"# <svg onload=alert(1)>",
		"- <iframe src=//evil>",
		"> <style>body{display:none}</style>",
		"| a | b |\n|---|---|\n| <script> | </td><td onmouseover=x> |",
		"`<script>` and ``<b>``",
		"```html\n<script>alert(1)</script>\n```",
		"```\"><script>\nx\n```",
		"&lt;script&gt; typed as entities",
		"<!-- a comment --> and <![CDATA[x]]>",
		NULL
	};
	gsize i;

	for (i = 0; NULL != attacks[i]; i++)
	{
		g_autofree gchar *html = render(attacks[i]);

		assert_only_own_tags(html);
		/* The words may survive as text; the tags may not. */
		g_assert_null(strstr(html, "<script"));
		g_assert_null(strstr(html, "<img"));
		g_assert_null(strstr(html, "<iframe"));
	}

	assert_render("<b>hi</b>", "<p>&lt;b&gt;hi&lt;/b&gt;</p>");
	/* An entity somebody typed is shown as they typed it. */
	assert_render("&lt;", "<p>&amp;lt;</p>");
	/* Code inside a fence is byte for byte, escaped. */
	assert_render("```\n<b>x</b>\n\n**y**\n```",
	              "<pre><code>&lt;b&gt;x&lt;/b&gt;\n\n**y**</code></pre>");
}

/*
 * A link goes only to an http(s) address, a path on this site or an
 * anchor on this page. If this regresses, [click](javascript:...) is a
 * stored script one click away.
 */
static void
test_links_only_to_places(void)
{
	static const gchar *const refused[] = {
		"[x](javascript:alert(1))",
		"[x](JaVaScRiPt:alert(1))",
		"[x](  javascript:alert(1))",
		"[x](data:text/html,<script>alert(1)</script>)",
		"[x](vbscript:msgbox)",
		"[x](//evil.example/path)",
		"[x](/\\evil.example)",
		"[x](file:///etc/passwd)",
		"[x](mailto:a@b.c)",
		"javascript:alert(1)",
		NULL
	};
	gsize i;

	for (i = 0; NULL != refused[i]; i++)
	{
		g_autofree gchar *html = render(refused[i]);

		assert_only_own_tags(html);

		if (NULL != strstr(html, "<a "))
			g_error("%s became a link: %s", refused[i], html);
	}

	/* A quote cannot close the attribute: the address stops before it
	 * and the rest is text. */
	{
		g_autofree gchar *html = render("[x](https://a.example/\"onmouseover=\"alert(1))");

		assert_only_own_tags(html);
		g_assert_null(strstr(html, "\"onmouseover"));
		g_assert_nonnull(strstr(html, "&quot;onmouseover=&quot;"));
	}

	assert_render("[docs](https://example.com/a?b=1&c=2)",
		"<p><a href=\"https://example.com/a?b=1&amp;c=2\" "
		"rel=\"noopener noreferrer nofollow\">docs</a></p>");
	assert_render("[the invoice](/e/invoice/12)",
		"<p><a href=\"/e/invoice/12\">the invoice</a></p>");
	assert_render("[above](#comment-3)",
		"<p><a href=\"#comment-3\">above</a></p>");
	/* Balanced parentheses stay in the address. */
	assert_render("[w](https://en.wikipedia.org/wiki/C_(language))",
		"<p><a href=\"https://en.wikipedia.org/wiki/C_(language)\" "
		"rel=\"noopener noreferrer nofollow\">w</a></p>");
	/* A bare address is a link, less the sentence's full stop. */
	assert_render("See https://example.com/x.",
		"<p>See <a href=\"https://example.com/x\" "
		"rel=\"noopener noreferrer nofollow\">https://example.com/x</a>.</p>");
	assert_render("(https://example.com/a)",
		"<p>(<a href=\"https://example.com/a\" "
		"rel=\"noopener noreferrer nofollow\">https://example.com/a</a>)</p>");
	/* A link inside a link's text is text. */
	{
		g_autofree gchar *html = render("[see https://a.example](https://b.example)");

		g_assert_nonnull(strstr(html, ">see https://a.example</a>"));
	}
}

/* The everyday syntax, pinned exactly. */
static void
test_inline(void)
{
	assert_render("**bold** and __bold__", "<p><strong>bold</strong> and <strong>bold</strong></p>");
	assert_render("*it* and _it_", "<p><em>it</em> and <em>it</em></p>");
	assert_render("~~gone~~", "<p><del>gone</del></p>");
	assert_render("`a * b`", "<p><code>a * b</code></p>");
	assert_render("``code with ` tick``", "<p><code>code with ` tick</code></p>");
	assert_render("**bold *and it***", "<p><strong>bold <em>and it</em></strong></p>");
	assert_render("***both***", "<p><strong><em>both</em></strong></p>");
	/* snake_case is a name, not emphasis. */
	assert_render("set max_age_hours and min_profit",
		"<p>set max_age_hours and min_profit</p>");
	assert_render("2 * 3 * 4", "<p>2 * 3 * 4</p>");
	assert_render("\\*not emphasis\\*", "<p>*not emphasis*</p>");
	/* A comment box is typed in lines. */
	assert_render("one\ntwo", "<p>one<br>\ntwo</p>");
	assert_render("one\n\ntwo", "<p>one</p><p>two</p>");
	assert_render("", "");
	assert_render("   \n\n  ", "");
}

static void
test_blocks(void)
{
	assert_render("# Title\n## Sub\n###### Six",
		"<h3>Title</h3><h4>Sub</h4><h6>Six</h6>");
	assert_render("#not a heading", "<p>#not a heading</p>");
	assert_render("---", "<hr>");
	assert_render("> quoted\n> more\n\nafter",
		"<blockquote><p>quoted<br>\nmore</p></blockquote><p>after</p>");
	assert_render("> outer\n> > inner",
		"<blockquote><p>outer</p><blockquote><p>inner</p></blockquote></blockquote>");
	assert_render("```c\nint x;\n```",
		"<pre><code class=\"language-c\">int x;</code></pre>");
	/* An unterminated fence ends with the comment. */
	assert_render("```\nopen", "<pre><code>open</code></pre>");
	assert_render("| a | b |\n|:--|--:|\n| 1 | 2 |",
		"<div class=\"md-table\"><table><thead><tr><th class=\"md-left\">a</th>"
		"<th class=\"md-right\">b</th></tr></thead><tbody><tr>"
		"<td class=\"md-left\">1</td><td class=\"md-right\">2</td></tr>"
		"</tbody></table></div>");
	/* A paragraph ends where a list begins. */
	assert_render("Notes:\n- a\n- b",
		"<p>Notes:</p><ul><li>a</li><li>b</li></ul>");
}

static void
test_lists(void)
{
	assert_render("- a\n- b", "<ul><li>a</li><li>b</li></ul>");
	assert_render("1. a\n2. b", "<ol><li>a</li><li>b</li></ol>");
	assert_render("3. c\n4. d", "<ol start=\"3\"><li>c</li><li>d</li></ol>");
	assert_render("- a\n  - nested\n- b",
		"<ul><li>a<ul><li>nested</li></ul></li><li>b</li></ul>");
	assert_render("1. a\n   - x\n2. b",
		"<ol><li>a<ul><li>x</li></ul></li><li>b</li></ol>");
	/* A blank line between items makes them paragraphs. */
	assert_render("- a\n\n- b", "<ul><li><p>a</p></li><li><p>b</p></li></ul>");
	assert_render("- [ ] todo\n- [x] done",
		"<ul><li class=\"task\"><input type=\"checkbox\" disabled "
		"aria-label=\"Not done\"> todo</li><li class=\"task\"><input "
		"type=\"checkbox\" disabled checked aria-label=\"Done\"> done</li></ul>");
	/* A lazy continuation line belongs to its item. */
	assert_render("- first\nstill first", "<ul><li>first<br>\nstill first</li></ul>");
}

static gchar *
mention_known(
	const gchar	*username,
	gpointer	 user_data
){
	(void)user_data;

	if (0 == g_strcmp0(username, "bob"))
		return g_strdup("Bob <Marsh> \"B\"");

	return NULL;
}

static gchar *
reference_known(
	const gchar	*type_name,
	gint64		 id,
	gpointer	 user_data
){
	(void)user_data;

	if ((0 == g_strcmp0(type_name, "invoice")) && (12 == id))
		return g_strdup("INV-0012 <draft>");

	return NULL;
}

/*
 * Mentions and references are the caller's decision, escaped like
 * everything else. If this regresses, a display name with markup in it
 * becomes markup, or somebody nobody can mention renders as a chip.
 */
static void
test_mentions_and_references(void)
{
	VentureMarkdownOptions options;
	g_autofree gchar *html = NULL;
	g_autofree gchar *code = NULL;
	g_autofree gchar *email = NULL;
	g_autofree gchar *refs = NULL;

	options.mention = mention_known;
	options.reference = reference_known;
	options.user_data = NULL;

	html = venture_markdown_to_html("hi @bob and @nobody.", &options);
	g_assert_cmpstr(html, ==,
		"<p>hi <span class=\"mention\" title=\"Bob &lt;Marsh&gt; &quot;B&quot;\">"
		"@bob</span> and @nobody.</p>");

	/* Inside code, or in an address, it names nobody. */
	code = venture_markdown_to_html("`@bob` and\n```\n@bob\n```", &options);
	g_assert_null(strstr(code, "mention"));
	email = venture_markdown_to_html("mail bob@bob.example", &options);
	g_assert_null(strstr(email, "mention"));

	refs = venture_markdown_to_html("see #invoice/12, not #invoice/13 or #Invoice/12",
	                                &options);
	g_assert_cmpstr(refs, ==,
		"<p>see <a href=\"/e/invoice/12\" class=\"record-ref\">INV-0012 "
		"&lt;draft&gt;</a>, not #invoice/13 or #Invoice/12</p>");
}

/*
 * The excerpt an inbox line and an activity entry show: the words, on one
 * line, with nothing of the syntax left.
 */
static void
test_text(void)
{
	g_autofree gchar *plain = NULL;
	g_autofree gchar *cut = NULL;

	plain = venture_markdown_to_text(
		"# Hello\n\n**Bold** and [a link](https://x.example) &amp; `code`\n\n- one\n- two",
		0);
	g_assert_cmpstr(plain, ==, "Hello Bold and a link &amp; code one two");

	cut = venture_markdown_to_text("one two three four", 7);
	g_assert_cmpstr(cut, ==, "one two\xe2\x80\xa6");
}

/*
 * The input is a stranger's and the server is single-threaded: neither a
 * wall of delimiters nor a tower of quotes may take long or overflow the
 * stack. If this regresses, one comment stalls every request.
 */
static void
test_bounded(void)
{
	g_autoptr(GString) stars = g_string_new(NULL);
	g_autoptr(GString) quotes = g_string_new(NULL);
	g_autoptr(GString) lists = g_string_new(NULL);
	g_autoptr(GTimer) timer = g_timer_new();
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	guint i;

	for (i = 0; i < 20000; i++)
		g_string_append(stars, (0 == i % 3) ? "**a" : "_*`[");

	for (i = 0; i < 500; i++)
		g_string_append_c(quotes, '>');

	g_string_append(quotes, " deep");

	for (i = 0; i < 200; i++)
	{
		guint j;

		for (j = 0; j < i; j++)
			g_string_append(lists, "  ");

		g_string_append(lists, "- item\n");
	}

	a = render(stars->str);
	b = render(quotes->str);
	c = render(lists->str);

	assert_only_own_tags(a);
	assert_only_own_tags(b);
	assert_only_own_tags(c);
	g_assert_nonnull(strstr(b, "deep"));
	g_assert_cmpfloat(g_timer_elapsed(timer, NULL), <, 5.0);
}

/*
 * An unclosed link must not search to the end of the body from every
 * opener. These are the scans in venture-markdown.c that walk forward
 * from a character: the two that were quadratic (a wall of '[', and
 * "[a]("), the title scan, and the ones that stay linear (emphasis,
 * code, a bare address, a mention, a #type/id, a pipe row, a fence).
 * Each body is the renderer's own input cap. Under two seconds, and the
 * text comes back escaped rather than as a link, except the bare address
 * which is a link on purpose.
 */
static gchar *
repeat_to(
	const gchar	*unit,
	gsize		 total
){
	gsize unit_len;
	gsize filled;
	gchar *text;

	unit_len = strlen(unit);
	text = g_malloc(total + 1);
	filled = 0;

	while (filled + unit_len <= total)
	{
		memcpy(text + filled, unit, unit_len);
		filled += unit_len;
	}

	if (filled < total)
		memcpy(text + filled, unit, total - filled);

	text[total] = '\0';

	return text;
}

static gchar *
render_within(
	const gchar	*name,
	const gchar	*text,
	const VentureMarkdownOptions *options,
	gdouble		 limit
){
	g_autoptr(GTimer) timer = g_timer_new();
	gchar *html;
	gdouble elapsed;

	html = venture_markdown_to_html(text, options);
	elapsed = g_timer_elapsed(timer, NULL);

	if (elapsed >= limit)
		g_error("%s rendered in %.3fs (limit %.1fs)", name, elapsed, limit);

	assert_only_own_tags(html);

	return html;
}

static gchar *
mention_never(
	const gchar	*username,
	gpointer	 user_data
){
	(void)username;
	(void)user_data;

	return NULL;
}

static gchar *
reference_never(
	const gchar	*type_name,
	gint64		 id,
	gpointer	 user_data
){
	(void)type_name;
	(void)id;
	(void)user_data;

	return NULL;
}

static void
assert_plain_wall(
	const gchar	*name,
	const gchar	*unit
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *shown = NULL;
	g_autofree gchar *html = NULL;
	gsize total = 256 * 1024;

	text = repeat_to(unit, total);
	/* A paragraph drops leading and trailing space. The wall is still
	 * the whole 256 KiB that was rendered; the comparison is the text
	 * the paragraph keeps. */
	shown = g_strstrip(g_strdup(text));
	html = render_within(name, text, NULL, 2.0);
	g_assert_true(strlen(html) == (strlen(shown) + strlen("<p></p>")));
	g_assert_cmpint(memcmp(html + 3, shown, strlen(shown)), ==, 0);
	g_assert_true(g_str_has_suffix(html, "</p>"));
	g_assert_null(strstr(html, "<a "));
}

static void
test_link_scans_are_bounded(void)
{
	VentureMarkdownOptions options;
	g_autofree gchar *fence = NULL;
	g_autofree gchar *code = NULL;
	g_autofree gchar *titles = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *address = NULL;
	g_autofree gchar *mention = NULL;
	g_autofree gchar *reference = NULL;
	gsize total = 256 * 1024;

	assert_plain_wall("brackets", "[");
	assert_plain_wall("link-open", "[a](");
	assert_plain_wall("emphasis", "*a ");
	assert_plain_wall("strike", "~~a ");
	assert_plain_wall("pipes", "|");

	code = repeat_to("`x", total);
	html = render_within("code", code, NULL, 2.0);
	g_assert_nonnull(strstr(html, "<code>x</code>"));
	g_assert_null(strstr(html, "<a "));

	/* The title never closes onto a ')'. The '[' stays text. The address
	 * in the middle is still a bare autolink, which is the ordinary rule,
	 * and the quote is escaped. */
	titles = repeat_to("[a](https://example.com/ \"x", total);
	g_free(html);
	html = render_within("link-title", titles, NULL, 2.0);
	g_assert_true(g_str_has_prefix(html, "<p>[a]("));
	g_assert_nonnull(strstr(html, "&quot;"));
	g_assert_null(strstr(html, ">[a](https://example.com/"));

	address = repeat_to("http://x", total);
	g_free(html);
	html = render_within("autolink", address, NULL, 2.0);
	g_assert_true(g_str_has_prefix(html,
		"<p><a href=\"http://x"));
	g_assert_nonnull(strstr(html, "</a></p>"));

	options.mention = mention_never;
	options.reference = reference_never;
	options.user_data = NULL;

	mention = repeat_to("@a ", total);
	g_free(html);
	html = render_within("mention", mention, &options, 2.0);
	{
		g_autofree gchar *shown = g_strstrip(g_strdup(mention));

		g_assert_true(strlen(html) == (strlen(shown) + strlen("<p></p>")));
		g_assert_cmpint(memcmp(html + 3, shown, strlen(shown)), ==, 0);
	}
	g_assert_null(strstr(html, "class=\"mention\""));

	reference = repeat_to("#a/12 ", total);
	g_free(html);
	html = render_within("reference", reference, &options, 2.0);
	{
		g_autofree gchar *shown = g_strstrip(g_strdup(reference));

		g_assert_true(strlen(html) == (strlen(shown) + strlen("<p></p>")));
		g_assert_cmpint(memcmp(html + 3, shown, strlen(shown)), ==, 0);
	}
	g_assert_null(strstr(html, "<a "));

	g_free(html);
	html = NULL;
	fence = repeat_to("`", total);
	/* A line of backticks is a fence, one pass over the line, not a
	 * search per backtick. */
	html = render_within("fence", fence, NULL, 2.0);
	g_assert_cmpstr(html, ==, "<pre><code></code></pre>");
}

/*
 * The bounds are inclusive. Text of 1024 bytes is still a link; one more
 * byte is the bracket, typed back out. A target of 2048 bytes is still a
 * link; one more byte is not, so the word "go" is not the link text. A
 * title is read and dropped, and the link remains.
 */
static void
test_link_near_bound(void)
{
	g_autoptr(GString) text_ok = NULL;
	g_autoptr(GString) text_over = NULL;
	g_autoptr(GString) url_ok = NULL;
	g_autoptr(GString) url_over = NULL;
	g_autofree gchar *href = NULL;
	guint i;

	text_ok = g_string_new("[");

	for (i = 0; i < 1024; i++)
		g_string_append_c(text_ok, 'a');

	g_string_append(text_ok, "](https://example.com/a)");

	{
		g_autofree gchar *html = render(text_ok->str);

		g_assert_true(g_str_has_prefix(html,
			"<p><a href=\"https://example.com/a\""));
		g_assert_true(g_str_has_suffix(html, "</a></p>"));
	}

	text_over = g_string_new("[");

	for (i = 0; i < 1025; i++)
		g_string_append_c(text_over, 'a');

	g_string_append(text_over, "](https://example.com/a)");

	{
		g_autofree gchar *html = render(text_over->str);

		g_assert_true(g_str_has_prefix(html, "<p>["));
		g_assert_null(strstr(html, "</a></p>"));
	}

	url_ok = g_string_new("https://example.com/");

	while (url_ok->len < 2048)
		g_string_append_c(url_ok, 'a');

	{
		g_autoptr(GString) link = g_string_new("[go](");
		g_autofree gchar *html = NULL;

		g_string_append(link, url_ok->str);
		g_string_append(link, ")");
		html = render(link->str);
		href = g_strdup_printf("<a href=\"%s\"", url_ok->str);
		g_assert_nonnull(strstr(html, href));
		g_assert_nonnull(strstr(html, ">go</a>"));
	}

	url_over = g_string_new(url_ok->str);
	g_string_append_c(url_over, 'b');

	{
		g_autoptr(GString) link = g_string_new("[go](");
		g_autofree gchar *html = NULL;

		g_string_append(link, url_over->str);
		g_string_append(link, ")");
		html = render(link->str);
		g_assert_null(strstr(html, ">go</a>"));
		g_assert_true(g_str_has_prefix(html, "<p>[go]("));
	}

	assert_render("[docs](https://example.com/a \"The title\")",
		"<p><a href=\"https://example.com/a\" "
		"rel=\"noopener noreferrer nofollow\">docs</a></p>");
}

/*
 * The NUL is the last byte of a mapped page and the next page cannot be
 * read. A character offset of 2000 used to walk off that NUL. The prefix
 * helper may look only as far as strlen does.
 */
static void
excerpt_on_guard(const gchar *bytes, gsize n)
{
	long page;
	gchar *map;
	gchar *body;
	gchar *owned;
	const gchar *source;

	page = sysconf(_SC_PAGESIZE);
	g_assert_cmpint(page, >, (glong)(n + 1));
	map = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	g_assert_true(MAP_FAILED != map);
	g_assert_cmpint(mprotect(map + page, (size_t)page, PROT_NONE), ==, 0);

	body = map + page - (gssize)(n + 1);
	memcpy(body, bytes, n);
	body[n] = '\0';
	owned = (gchar *)0x1;
	source = venture_notify_excerpt_source(body, &owned);
	g_assert_null(owned);
	g_assert_true(source == body);
	g_assert_cmpuint(strlen(source), ==, n);
	g_assert_cmpint(memcmp(source, bytes, n), ==, 0);

	g_assert_cmpint(mprotect(map + page, (size_t)page, PROT_READ | PROT_WRITE), ==, 0);
	g_assert_cmpint(munmap(map, (size_t)page * 2), ==, 0);
}

static void
test_excerpt_guard_page(void)
{
	static const gchar eacute[] = { '\xc3', '\xa9' };

	excerpt_on_guard("hi", 2);
	excerpt_on_guard(eacute, 2);
}

/*
 * A quarter-megabyte body is still an inbox line: the renderer sees at
 * most VENTURE_NOTIFY_EXCERPT_SOURCE_BYTES, then the usual 140 characters.
 * venture_truncate keeps those 140 and appends one ellipsis, so the
 * string is 141 characters.
 */
static void
test_excerpt_long_body(void)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *owned = NULL;
	g_autofree gchar *excerpt = NULL;
	g_autofree gchar *wanted = NULL;
	const gchar *source;
	g_autoptr(GTimer) timer = NULL;
	guint i;

	body = g_new(gchar, (256 * 1024) + 1);
	memset(body, '[', 256 * 1024);
	body[256 * 1024] = '\0';

	timer = g_timer_new();
	source = venture_notify_excerpt_source(body, &owned);
	excerpt = venture_markdown_to_text(source, 140);
	g_assert_cmpfloat(g_timer_elapsed(timer, NULL), <, 2.0);
	g_assert_nonnull(owned);
	g_assert_true(source == owned);
	g_assert_cmpuint(strlen(source), <=, (gsize)VENTURE_NOTIFY_EXCERPT_SOURCE_BYTES);
	g_assert_cmpint(g_utf8_strlen(excerpt, -1), <=, 141);

	wanted = g_new(gchar, 140 + 4);
	for (i = 0; i < 140; i++)
		wanted[i] = '[';
	memcpy(wanted + 140, "\xe2\x80\xa6", 3);
	wanted[143] = '\0';
	g_assert_cmpstr(excerpt, ==, wanted);
}

/*
 * The bound falls on the third byte of a 4-byte character. The prefix
 * steps back to the lead byte and drops the whole character, so what
 * remains is valid UTF-8 and does not end on a partial sequence.
 */
static void
test_excerpt_cut_is_utf8(void)
{
	static const gchar mark[] = "\xf0\x9f\x93\x84";
	g_autofree gchar *body = NULL;
	g_autofree gchar *owned = NULL;
	const gchar *source;
	gsize n;

	n = (gsize)VENTURE_NOTIFY_EXCERPT_SOURCE_BYTES;
	body = g_new(gchar, n + 8);
	memset(body, 'a', n - 2);
	memcpy(body + (n - 2), mark, 4);
	memset(body + (n - 2) + 4, 'b', 4);
	body[(n - 2) + 4 + 4] = '\0';

	source = venture_notify_excerpt_source(body, &owned);
	g_assert_true(source == owned);
	g_assert_true(g_utf8_validate(source, -1, NULL));
	g_assert_cmpuint(strlen(source), ==, n - 2);
	g_assert_cmpint(memcmp(source, body, n - 2), ==, 0);
	g_assert_null(memchr(source, '\xf0', strlen(source)));
}


int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/markdown/html-is-text", test_html_is_text);
	g_test_add_func("/markdown/links-only-to-places", test_links_only_to_places);
	g_test_add_func("/markdown/inline", test_inline);
	g_test_add_func("/markdown/blocks", test_blocks);
	g_test_add_func("/markdown/lists", test_lists);
	g_test_add_func("/markdown/mentions-and-references", test_mentions_and_references);
	g_test_add_func("/markdown/text", test_text);
	g_test_add_func("/markdown/bounded", test_bounded);
	g_test_add_func("/markdown/link-scans-are-bounded", test_link_scans_are_bounded);
	g_test_add_func("/markdown/link-near-bound", test_link_near_bound);
	g_test_add_func("/markdown/excerpt-guard-page", test_excerpt_guard_page);
	g_test_add_func("/markdown/excerpt-long-body", test_excerpt_long_body);
	g_test_add_func("/markdown/excerpt-cut-is-utf8", test_excerpt_cut_is_utf8);

	return g_test_run();
}
