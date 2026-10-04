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

	return g_test_run();
}
