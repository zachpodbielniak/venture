/*
 * venture-forge-rules.c - Rule resolution and branch naming
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/* A slug longer than this stops being a name and starts being the title
 * again. Git's own limit is far higher; this one is about readability, and
 * about leaving room for the prefix and the id inside a sane refname. */
#define VENTURE_FORGE_SLUG_MAX (48)

/* The default when a rule names no template. The id is not decorative: two
 * tickets with the same title must not land on one branch. */
#define VENTURE_FORGE_BRANCH_DEFAULT "{type}/{id}-{slug}"

/*
 * Runs one step of the precedence ladder.
 *
 * Ordering by id ascending and taking the first is what makes a tie
 * deterministic. Two rules with identical scope is a configuration mistake
 * rather than an error -- the answer still has to be the same one every
 * time, or the same ticket behaves differently on consecutive runs.
 */
static VentureForgeRule *
venture_forge_rule_try(
	VentureDatabase	 *database,
	gint64		  repo_id,
	gint64		  forge_id,
	gboolean	  all_types,
	VentureIssueType  issue_type,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rules = NULL;

	query = venture_query_new(VENTURE_TYPE_FORGE_RULE);

	if (!venture_query_add_filter_int(query, "repo-id", VENTURE_FILTER_OP_EQ,
	                                  repo_id, error))
		return NULL;

	/* A repository-scoped step does not constrain the forge: the
	 * repository already determines it, and a rule that named a
	 * different one would be a contradiction rather than a filter. */
	if ((0 == repo_id) &&
	    !venture_query_add_filter_int(query, "forge-id",
	                                  VENTURE_FILTER_OP_EQ, forge_id, error))
		return NULL;

	if (!venture_query_add_filter_string(query, "all-issue-types",
	                                     VENTURE_FILTER_OP_EQ,
	                                     all_types ? "true" : "false", error))
		return NULL;

	if (!all_types)
	{
		const gchar *nick;

		nick = venture_enum_to_nick(VENTURE_TYPE_ISSUE_TYPE, issue_type);

		if (!venture_query_add_filter_string(query, "issue-type",
		                                     VENTURE_FILTER_OP_EQ, nick,
		                                     error))
			return NULL;
	}

	if (!venture_query_add_filter_string(query, "enabled",
	                                     VENTURE_FILTER_OP_EQ, "true", error))
		return NULL;

	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING,
	                             error))
		return NULL;

	/* Two rather than one: fetching a second row is what lets a tie be
	 * reported rather than silently resolved. */
	venture_query_set_limit(query, 2);

	rules = venture_database_find(database, query, error);

	if (NULL == rules)
		return NULL;

	if (0 == rules->len)
		return NULL;

	if (rules->len > 1)
	{
		g_warning("venture_forge_rules: rules #%" G_GINT64_FORMAT
		          " and #%" G_GINT64_FORMAT " have identical scope; "
		          "using the lower id. Delete or narrow one of them.",
		          venture_entity_get_id(g_ptr_array_index(rules, 0)),
		          venture_entity_get_id(g_ptr_array_index(rules, 1)));
	}

	return VENTURE_FORGE_RULE(g_object_ref(g_ptr_array_index(rules, 0)));
}

VentureForgeRule *
venture_forge_rule_resolve(
	VentureDatabase	 *database,
	gint64		  repo_id,
	VentureIssueType  issue_type,
	GError		**error
){
	g_autoptr(VentureEntity) repo = NULL;
	VentureForgeRule *rule;
	gint64 forge_id = 0;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	/* No repository means no rule. Every step below is scoped either to a
	 * repository or to the forge that repository belongs to, so without
	 * one there is nothing to resolve against. */
	if (0 == repo_id)
		return NULL;

	repo = venture_database_get(database, VENTURE_TYPE_FORGE_REPO, repo_id,
	                            NULL);

	if (NULL == repo)
		return NULL;

	g_object_get(repo, "forge-id", &forge_id, NULL);

	/* 1. the repository's rule for this issue type */
	rule = venture_forge_rule_try(database, repo_id, 0, FALSE, issue_type,
	                              error);

	if (NULL != rule)
		return rule;

	if ((NULL != error) && (NULL != *error))
		return NULL;

	/* 2. the repository's catch-all */
	rule = venture_forge_rule_try(database, repo_id, 0, TRUE, issue_type,
	                              error);

	if (NULL != rule)
		return rule;

	if ((NULL != error) && (NULL != *error))
		return NULL;

	if (0 == forge_id)
		return NULL;

	/* 3. the forge-wide rule for this issue type */
	rule = venture_forge_rule_try(database, 0, forge_id, FALSE, issue_type,
	                              error);

	if (NULL != rule)
		return rule;

	if ((NULL != error) && (NULL != *error))
		return NULL;

	/* 4. the forge-wide catch-all */
	return venture_forge_rule_try(database, 0, forge_id, TRUE, issue_type,
	                              error);
}

gboolean
venture_forge_refname_is_valid(const gchar *name)
{
	gsize length;
	gsize i;

	if (venture_string_is_empty(name))
		return FALSE;

	length = strlen(name);

	/* A name beginning with a hyphen is a legal-looking git argument
	 * before it is a branch, which is the difference between naming a
	 * branch and passing git an option somebody else chose. */
	if (('-' == name[0]) || ('.' == name[0]) || ('/' == name[0]))
		return FALSE;

	if (('/' == name[length - 1]) || ('.' == name[length - 1]))
		return FALSE;

	if (g_str_has_suffix(name, ".lock"))
		return FALSE;

	if (NULL != strstr(name, ".."))
		return FALSE;

	if (NULL != strstr(name, "//"))
		return FALSE;

	if (NULL != strstr(name, "@{"))
		return FALSE;

	for (i = 0; i < length; i++)
	{
		guchar c = (guchar)name[i];

		if (c <= 0x20 || 0x7f == c)
			return FALSE;

		if (NULL != strchr("~^:?*[\\", (gchar)c))
			return FALSE;
	}

	return TRUE;
}

/*
 * Truncates a slug on a hyphen boundary.
 *
 * Cutting mid-word leaves a branch named after half a sentence, which reads
 * as a mistake rather than an abbreviation. If the first word is itself
 * longer than the limit there is no boundary to find and a hard cut is the
 * only option.
 */
static gchar *
venture_forge_slug_trim(
	const gchar	*slug,
	gsize		 limit
){
	gchar *cut;
	gchar *last;

	if (strlen(slug) <= limit)
		return g_strdup(slug);

	cut = g_strndup(slug, limit);
	last = strrchr(cut, '-');

	if (NULL != last && last != cut)
		*last = '\0';

	return cut;
}

gchar *
venture_forge_branch_name(
	const gchar	 *template_text,
	const gchar	 *prefix,
	VentureIssueType  issue_type,
	gint64		  ticket_id,
	gint64		  issue_number,
	const gchar	 *title
){
	const gchar *type_nick;
	g_autofree gchar *raw_slug = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *issue_text = NULL;
	g_autofree gchar *expanded = NULL;
	g_autoptr(GString) out = NULL;
	const gchar *source;

	source = venture_string_is_empty(template_text)
		? VENTURE_FORGE_BRANCH_DEFAULT
		: template_text;

	type_nick = venture_enum_to_nick(VENTURE_TYPE_ISSUE_TYPE, issue_type);
	raw_slug = venture_slugify(title);
	slug = venture_forge_slug_trim(raw_slug, VENTURE_FORGE_SLUG_MAX);
	id_text = g_strdup_printf("%" G_GINT64_FORMAT, ticket_id);
	issue_text = g_strdup_printf("%" G_GINT64_FORMAT,
	                             (0 != issue_number) ? issue_number : ticket_id);

	out = g_string_new(source);

	g_string_replace(out, "{type}", type_nick, 0);
	g_string_replace(out, "{id}", id_text, 0);
	g_string_replace(out, "{issue}", issue_text, 0);
	g_string_replace(out, "{slug}", slug, 0);

	if (!venture_string_is_empty(prefix))
		g_string_prepend(out, prefix);

	expanded = g_string_free(g_steal_pointer(&out), FALSE);

	/*
	 * The template and the prefix are operator text, so the result can
	 * still be illegal however careful the slug was -- a prefix of "-x"
	 * or a template containing ".." are both reachable from the settings
	 * page. Falling back to a name built only from parts this file
	 * controls is better than handing git something it will refuse with
	 * an opaque message, or worse, something it will read as a flag.
	 */
	if (!venture_forge_refname_is_valid(expanded))
	{
		g_warning("venture_forge_rules: branch template produced \"%s\", "
		          "which git will not accept; using a safe name instead",
		          expanded);

		return g_strdup_printf("venture/%s/%" G_GINT64_FORMAT "-%s",
		                       type_nick, ticket_id, slug);
	}

	return g_steal_pointer(&expanded);
}

gboolean
venture_forge_repo_split(
	const gchar	 *full_name,
	gchar		**out_owner,
	gchar		**out_repo
){
	const gchar *slash;
	g_autofree gchar *owner = NULL;
	g_autofree gchar *repo = NULL;

	g_return_val_if_fail(NULL != out_owner, FALSE);
	g_return_val_if_fail(NULL != out_repo, FALSE);

	*out_owner = NULL;
	*out_repo = NULL;

	if (venture_string_is_empty(full_name))
		return FALSE;

	slash = strchr(full_name, '/');

	if (NULL == slash)
		return FALSE;

	owner = g_strndup(full_name, (gsize)(slash - full_name));
	repo = g_strdup(slash + 1);

	if (venture_string_is_empty(owner) || venture_string_is_empty(repo))
		return FALSE;

	/*
	 * These two become path segments in an API URL. Percent-encoding is
	 * applied there as well, but a name containing a slash or a traversal
	 * segment is malformed rather than merely awkward, and refusing it
	 * here means the client never has to reason about what a second slash
	 * would have meant.
	 */
	if ((NULL != strchr(repo, '/')) ||
	    (0 == g_strcmp0(owner, "..")) || (0 == g_strcmp0(repo, "..")) ||
	    (0 == g_strcmp0(owner, ".")) || (0 == g_strcmp0(repo, ".")))
		return FALSE;

	*out_owner = g_steal_pointer(&owner);
	*out_repo = g_steal_pointer(&repo);

	return TRUE;
}

gchar *
venture_forge_clone_url(
	const gchar	*repo_clone_url,
	const gchar	*forge_clone_base,
	const gchar	*forge_base_url,
	const gchar	*full_name
){
	g_autofree gchar *owner = NULL;
	g_autofree gchar *repo = NULL;
	g_autofree gchar *trimmed = NULL;
	const gchar *base;
	gsize length;

	/* An explicit URL on the repository is used exactly as given: it is
	 * there precisely because this repository does not follow the
	 * pattern, so composing anything onto it would defeat the point. */
	if (!venture_string_is_empty(repo_clone_url))
		return g_strdup(repo_clone_url);

	if (!venture_forge_repo_split(full_name, &owner, &repo))
		return NULL;

	base = !venture_string_is_empty(forge_clone_base) ? forge_clone_base
	                                                  : forge_base_url;

	if (venture_string_is_empty(base))
		return NULL;

	/* A trailing separator is how a person naturally writes a base, and
	 * doubling it produces a URL that fails in a way nobody reads
	 * carefully. */
	trimmed = g_strdup(base);
	length = strlen(trimmed);

	while ((length > 0) &&
	       (('/' == trimmed[length - 1]) || (':' == trimmed[length - 1])))
	{
		trimmed[length - 1] = '\0';
		length--;
	}

	if (0 == length)
		return NULL;

	/*
	 * scp-like syntax when the base names a user and no scheme: git
	 * takes git@host:owner/repo.git, and the colon is not optional --
	 * git@host/owner/repo.git is read as a path, not a host.
	 */
	if ((NULL == strstr(trimmed, "://")) && (NULL != strchr(trimmed, '@')))
		return g_strdup_printf("%s:%s/%s.git", trimmed, owner, repo);

	return g_strdup_printf("%s/%s/%s.git", trimmed, owner, repo);
}

gchar *
venture_forge_web_url(
	const gchar	*base_url,
	const gchar	*full_name,
	const gchar	*branch
){
	g_autofree gchar *owner = NULL;
	g_autofree gchar *repo = NULL;
	g_autofree gchar *owner_escaped = NULL;
	g_autofree gchar *repo_escaped = NULL;
	g_autofree gchar *trimmed = NULL;
	gsize length;

	if (venture_string_is_empty(base_url))
		return NULL;

	if (!venture_forge_repo_split(full_name, &owner, &repo))
		return NULL;

	trimmed = g_strdup(base_url);
	length = strlen(trimmed);

	while ((length > 0) && ('/' == trimmed[length - 1]))
	{
		trimmed[length - 1] = '\0';
		length--;
	}

	owner_escaped = g_uri_escape_string(owner, NULL, FALSE);
	repo_escaped = g_uri_escape_string(repo, NULL, FALSE);

	if (venture_string_is_empty(branch))
	{
		return g_strdup_printf("%s/%s/%s", trimmed, owner_escaped,
		                       repo_escaped);
	}

	{
		g_autofree gchar *branch_escaped = NULL;

		/* Slashes are kept: a branch called feature/thing is ordinary,
		 * and the route wants those separators as separators. */
		branch_escaped = g_uri_escape_string(branch, "/", FALSE);

		return g_strdup_printf("%s/%s/%s/src/branch/%s", trimmed,
		                       owner_escaped, repo_escaped, branch_escaped);
	}
}
