/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The sending domain check: SPF, DKIM and DMARC, read from DNS through an
 * injectable resolver, each result in plain words with what to change.
 * It reads public DNS only; it cannot see what the relay actually signs,
 * so a pass here is necessary, not sufficient (docs/deliverability.org). */
#include <venture.h>
#include <string.h>

#define LOOKUP_TIMEOUT_MS 5000

G_DEFINE_INTERFACE(VentureDnsResolver, venture_dns_resolver, G_TYPE_OBJECT)
static void venture_dns_resolver_default_init(VentureDnsResolverInterface *iface)
{
	(void)iface;
}
gchar **venture_dns_resolver_lookup_txt(VentureDnsResolver *self, const gchar *name,
	GCancellable *cancellable, GError **error)
{
	VentureDnsResolverInterface *iface;
	g_return_val_if_fail(VENTURE_IS_DNS_RESOLVER(self), NULL);
	g_return_val_if_fail(name != NULL, NULL);
	iface = VENTURE_DNS_RESOLVER_GET_IFACE(self);
	g_return_val_if_fail(iface->lookup_txt != NULL, NULL);
	return iface->lookup_txt(self, name, cancellable, error);
}

/* ------------------------------------------------------------------ */
/* GResolver, bounded. */

struct _VentureGioDnsResolver {
	GObject parent_instance;
	guint timeout_ms;
};
static void gio_iface_init(VentureDnsResolverInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureGioDnsResolver, venture_gio_dns_resolver, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_DNS_RESOLVER, gio_iface_init))

typedef struct {
	GMainLoop *loop;
	GList *records;
	GError *error;
	gboolean done;
} Lookup;

static void lookup_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Lookup *lookup = data;
	lookup->records = g_resolver_lookup_records_finish(G_RESOLVER(source), result, &lookup->error);
	lookup->done = TRUE;
	g_main_loop_quit(lookup->loop);
}
static gboolean lookup_timeout(gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));
	return G_SOURCE_REMOVE;
}
/* The lookup runs on GResolver's worker and finishes on a private main
 * context: the server's own loop is not iterated underneath a request,
 * and the deadline cancels a resolver that never answers. */
static gchar **gio_lookup(VentureDnsResolver *resolver, const gchar *name, GCancellable *cancellable, GError **error)
{
	VentureGioDnsResolver *self = VENTURE_GIO_DNS_RESOLVER(resolver);
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GCancellable) deadline = g_cancellable_new();
	g_autoptr(GSource) timer = NULL;
	g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
	Lookup lookup = { NULL, NULL, NULL, FALSE };
	gulong forwarded = 0;
	GList *item;
	g_main_context_push_thread_default(context);
	lookup.loop = g_main_loop_new(context, FALSE);
	timer = g_timeout_source_new(self->timeout_ms);
	g_source_set_callback(timer, lookup_timeout, deadline, NULL);
	g_source_attach(timer, context);
	if (cancellable) forwarded = g_cancellable_connect(cancellable, G_CALLBACK(g_cancellable_cancel), deadline, NULL);
	g_resolver_lookup_records_async(g_resolver_get_default(), name, G_RESOLVER_RECORD_TXT, deadline, lookup_done, &lookup);
	while (!lookup.done) g_main_loop_run(lookup.loop);
	g_source_destroy(timer);
	if (cancellable) g_cancellable_disconnect(cancellable, forwarded);
	g_main_loop_unref(lookup.loop);
	g_main_context_pop_thread_default(context);
	if (lookup.error) {
		g_propagate_error(error, lookup.error);
		return NULL;
	}
	for (item = lookup.records; item; item = item->next) {
		GVariant *record = item->data;
		g_autoptr(GVariant) strings = g_variant_get_child_value(record, 0);
		g_autofree const gchar **parts = g_variant_get_strv(strings, NULL);
		/* A TXT record longer than 255 bytes is several strings; they are
		 * one value, joined with nothing between them. */
		g_ptr_array_add(out, g_strjoinv("", (gchar **)parts));
	}
	g_list_free_full(lookup.records, (GDestroyNotify)g_variant_unref);
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}
static void gio_iface_init(VentureDnsResolverInterface *iface)
{
	iface->lookup_txt = gio_lookup;
}
static void venture_gio_dns_resolver_class_init(VentureGioDnsResolverClass *klass)
{
	(void)klass;
}
static void venture_gio_dns_resolver_init(VentureGioDnsResolver *self)
{
	self->timeout_ms = LOOKUP_TIMEOUT_MS;
}
VentureGioDnsResolver *venture_gio_dns_resolver_new(guint timeout_ms)
{
	VentureGioDnsResolver *self = g_object_new(VENTURE_TYPE_GIO_DNS_RESOLVER, NULL);
	if (timeout_ms) self->timeout_ms = timeout_ms;
	return self;
}
void venture_deliverability_set_resolver(VentureDatabase *database, VentureDnsResolver *resolver)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	g_object_set_data_full(G_OBJECT(database), "venture-deliverability-resolver",
		resolver ? g_object_ref(resolver) : NULL, resolver ? g_object_unref : NULL);
}
VentureDnsResolver *venture_deliverability_get_resolver(VentureDatabase *database)
{
	VentureDnsResolver *resolver;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	resolver = g_object_get_data(G_OBJECT(database), "venture-deliverability-resolver");
	if (resolver) return g_object_ref(resolver);
	return VENTURE_DNS_RESOLVER(venture_gio_dns_resolver_new(LOOKUP_TIMEOUT_MS));
}

/* ------------------------------------------------------------------ */
/* The checks. */

typedef enum { CHECK_PASS, CHECK_WEAK, CHECK_MISSING, CHECK_ERROR } CheckStatus;
static const gchar *const status_names[] = { "pass", "weak", "missing", "error" };

static void add_check(JsonBuilder *builder, const gchar *id, const gchar *label, CheckStatus status,
	const gchar *name, const gchar *record, const gchar *summary, const gchar *fix)
{
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id"); json_builder_add_string_value(builder, id);
	json_builder_set_member_name(builder, "label"); json_builder_add_string_value(builder, label);
	json_builder_set_member_name(builder, "status"); json_builder_add_string_value(builder, status_names[status]);
	json_builder_set_member_name(builder, "name"); json_builder_add_string_value(builder, name);
	json_builder_set_member_name(builder, "record");
	if (record) json_builder_add_string_value(builder, record); else json_builder_add_null_value(builder);
	json_builder_set_member_name(builder, "summary"); json_builder_add_string_value(builder, summary);
	json_builder_set_member_name(builder, "fix"); json_builder_add_string_value(builder, fix ? fix : "");
	json_builder_end_object(builder);
}
/* TXT records at @name starting with @prefix (case aside), or NULL with
 * *failed set when DNS could not answer (as opposed to "none there"). */
static GPtrArray *matching(VentureDnsResolver *resolver, const gchar *name, const gchar *prefix, gboolean *failed)
{
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) records = venture_dns_resolver_lookup_txt(resolver, name, NULL, &error);
	GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
	guint i;
	*failed = FALSE;
	if (!records) {
		if (!g_error_matches(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND)) *failed = TRUE;
		return found;
	}
	for (i = 0; records[i]; i++) {
		const gchar *record = records[i];
		while (g_ascii_isspace(*record)) record++;
		if (!g_ascii_strncasecmp(record, prefix, strlen(prefix)) &&
			(record[strlen(prefix)] == '\0' || record[strlen(prefix)] == ' ' || record[strlen(prefix)] == ';'))
			g_ptr_array_add(found, g_strdup(record));
	}
	return found;
}
/* A tag's value in a "k=v; k=v" record, lower case, or NULL. */
static gchar *tag(const gchar *record, const gchar *name)
{
	g_auto(GStrv) parts = g_strsplit(record, ";", 64);
	guint i;
	for (i = 0; parts[i]; i++) {
		gchar *part = g_strstrip(parts[i]);
		gchar *equals = strchr(part, '=');
		if (!equals) continue;
		*equals = '\0';
		if (!g_ascii_strcasecmp(g_strstrip(part), name)) return g_ascii_strdown(g_strstrip(equals + 1), -1);
	}
	return NULL;
}
static CheckStatus check_spf(VentureDnsResolver *resolver, JsonBuilder *builder, const gchar *domain)
{
	gboolean failed;
	g_autoptr(GPtrArray) found = matching(resolver, domain, "v=spf1", &failed);
	const gchar *record;
	g_autofree gchar *lower = NULL;
	if (failed) {
		add_check(builder, "spf", "SPF", CHECK_ERROR, domain, NULL, "DNS did not answer, so SPF could not be checked.", "Try again in a few minutes.");
		return CHECK_ERROR;
	}
	if (!found->len) {
		add_check(builder, "spf", "SPF", CHECK_MISSING, domain, NULL,
			"No SPF record: receivers cannot tell which servers may send mail for this domain.",
			"Add the TXT record your mail provider gives you, for example v=spf1 include:<your provider> -all, at the domain itself.");
		return CHECK_MISSING;
	}
	if (found->len > 1) {
		add_check(builder, "spf", "SPF", CHECK_WEAK, domain, g_ptr_array_index(found, 0),
			"More than one SPF record: receivers treat that as an error and ignore all of them.",
			"Merge them into one v=spf1 record and delete the others.");
		return CHECK_WEAK;
	}
	record = g_ptr_array_index(found, 0);
	lower = g_ascii_strdown(record, -1);
	if (strstr(lower, "+all") || g_str_has_suffix(lower, " all")) {
		add_check(builder, "spf", "SPF", CHECK_WEAK, domain, record,
			"The SPF record ends in +all, which lets any server on the internet send as this domain.",
			"Change +all to ~all (or -all once you are sure every sender is listed).");
		return CHECK_WEAK;
	}
	if (strstr(lower, "?all") || !strstr(lower, "all")) {
		add_check(builder, "spf", "SPF", CHECK_WEAK, domain, record,
			"The SPF record does not say what to do with other servers, so it protects nothing.",
			"End the record with ~all, or -all once you are sure every sender is listed.");
		return CHECK_WEAK;
	}
	add_check(builder, "spf", "SPF", CHECK_PASS, domain, record,
		"SPF lists the servers allowed to send for this domain and refuses or flags the rest.", NULL);
	return CHECK_PASS;
}
static CheckStatus check_dkim(VentureDnsResolver *resolver, JsonBuilder *builder, const gchar *domain, const gchar *selector)
{
	g_autofree gchar *name = NULL, *key = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) records = NULL;
	const gchar *record = NULL;
	guint i;
	if (!selector || !*selector) {
		g_autofree gchar *where = g_strdup_printf("<selector>._domainkey.%s", domain);
		add_check(builder, "dkim", "DKIM", CHECK_MISSING, where, NULL,
			"No DKIM selector is set, so the key cannot be looked up.",
			"Ask your mail provider for the DKIM selector (the part before ._domainkey) and save it on the sending domain.");
		return CHECK_MISSING;
	}
	name = g_strdup_printf("%s._domainkey.%s", selector, domain);
	records = venture_dns_resolver_lookup_txt(resolver, name, NULL, &error);
	if (!records && !g_error_matches(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND)) {
		add_check(builder, "dkim", "DKIM", CHECK_ERROR, name, NULL, "DNS did not answer, so DKIM could not be checked.", "Try again in a few minutes.");
		return CHECK_ERROR;
	}
	for (i = 0; records && records[i] && !record; i++)
		if (strstr(records[i], "p=")) record = records[i];
	if (!record) {
		g_autofree gchar *fix = g_strdup_printf("Publish the DKIM key your mail provider gives you as a TXT record at %s, "
			"or check the selector is the one your provider uses.", name);
		add_check(builder, "dkim", "DKIM", CHECK_MISSING, name, NULL,
			"No DKIM key at this selector: receivers cannot verify the signature on your mail.", fix);
		return CHECK_MISSING;
	}
	key = tag(record, "p");
	if (!key || !*key) {
		add_check(builder, "dkim", "DKIM", CHECK_MISSING, name, record,
			"The DKIM key at this selector is empty, which means it was revoked.",
			"Publish the current key from your mail provider, or use the selector it signs with now.");
		return CHECK_MISSING;
	}
	add_check(builder, "dkim", "DKIM", CHECK_PASS, name, record,
		"A DKIM key is published, so receivers can check that mail signed with it really came from you. "
		"This cannot see whether your relay actually signs; send yourself a test and look for dkim=pass.", NULL);
	return CHECK_PASS;
}
static CheckStatus check_dmarc(VentureDnsResolver *resolver, JsonBuilder *builder, const gchar *domain)
{
	g_autofree gchar *name = g_strdup_printf("_dmarc.%s", domain), *policy = NULL, *percent = NULL, *reports = NULL;
	gboolean failed;
	g_autoptr(GPtrArray) found = matching(resolver, name, "v=DMARC1", &failed);
	const gchar *record;
	if (failed) {
		add_check(builder, "dmarc", "DMARC", CHECK_ERROR, name, NULL, "DNS did not answer, so DMARC could not be checked.", "Try again in a few minutes.");
		return CHECK_ERROR;
	}
	if (found->len != 1) {
		add_check(builder, "dmarc", "DMARC", found->len ? CHECK_WEAK : CHECK_MISSING, name, found->len ? g_ptr_array_index(found, 0) : NULL,
			found->len ? "More than one DMARC record: receivers ignore all of them."
				: "No DMARC record: receivers do not know what to do with mail that fails SPF and DKIM, and large providers now expect one from bulk senders.",
			found->len ? "Keep one v=DMARC1 record at _dmarc and delete the others."
				: "Add a TXT record at _dmarc with v=DMARC1; p=none; rua=mailto:<an address you read>, watch the reports, then move to p=quarantine.");
		return found->len ? CHECK_WEAK : CHECK_MISSING;
	}
	record = g_ptr_array_index(found, 0);
	policy = tag(record, "p");
	percent = tag(record, "pct");
	reports = tag(record, "rua");
	if (!policy || (g_strcmp0(policy, "none") && g_strcmp0(policy, "quarantine") && g_strcmp0(policy, "reject"))) {
		add_check(builder, "dmarc", "DMARC", CHECK_WEAK, name, record, "The DMARC record has no valid p= policy, so receivers ignore it.",
			"Set p=none to start watching, then p=quarantine.");
		return CHECK_WEAK;
	}
	if (!g_strcmp0(policy, "none")) {
		add_check(builder, "dmarc", "DMARC", CHECK_WEAK, name, record,
			"DMARC is in monitoring mode (p=none): receivers report on mail that fails, but deliver it anyway, so anyone can still send as you.",
			reports ? "Once the reports show only your own senders, change p=none to p=quarantine, and later p=reject."
				: "Add rua=mailto:<an address you read> to get reports, then change p=none to p=quarantine.");
		return CHECK_WEAK;
	}
	if (percent && g_strcmp0(percent, "100")) {
		add_check(builder, "dmarc", "DMARC", CHECK_WEAK, name, record,
			"The DMARC policy applies to only part of the mail that fails (pct is below 100).",
			"Raise pct to 100, or remove it, once the reports look clean.");
		return CHECK_WEAK;
	}
	add_check(builder, "dmarc", "DMARC", CHECK_PASS, name, record,
		!g_strcmp0(policy, "reject") ? "DMARC tells receivers to refuse mail that fails SPF and DKIM."
			: "DMARC tells receivers to treat mail that fails SPF and DKIM as spam.",
		reports ? NULL : "Optional: add rua=mailto:<an address you read> to receive DMARC reports.");
	return CHECK_PASS;
}
static gboolean dns_name(const gchar *value, gboolean label_only)
{
	const gchar *p;
	guint dots = 0;
	if (!value || !*value || strlen(value) > 253 || value[0] == '.' || value[0] == '-') return FALSE;
	for (p = value; *p; p++) {
		if (*p == '.') { if (label_only || p[1] == '.' || p[1] == '\0') return FALSE; dots++; continue; }
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') return FALSE;
	}
	return label_only || dots >= 1;
}
JsonNode *venture_deliverability_check_domain(VentureDnsResolver *resolver, const gchar *domain,
	const gchar *selector, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *name = NULL, *key = NULL;
	CheckStatus statuses[3], overall = CHECK_PASS;
	guint i;
	g_return_val_if_fail(VENTURE_IS_DNS_RESOLVER(resolver), NULL);
	name = g_ascii_strdown(domain ? domain : "", -1);
	g_strstrip(name);
	key = selector ? g_strstrip(g_strdup(selector)) : NULL;
	if (!dns_name(name, FALSE)) {
		venture_set_error_validation(error, "domain", "Enter a domain such as example.com");
		return NULL;
	}
	if (key && *key && !dns_name(key, TRUE)) {
		venture_set_error_validation(error, "selector", "A DKIM selector is one DNS label, such as mail or s1");
		return NULL;
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "domain"); json_builder_add_string_value(builder, name);
	json_builder_set_member_name(builder, "selector"); json_builder_add_string_value(builder, key ? key : "");
	json_builder_set_member_name(builder, "checks");
	json_builder_begin_array(builder);
	statuses[0] = check_spf(resolver, builder, name);
	statuses[1] = check_dkim(resolver, builder, name, key);
	statuses[2] = check_dmarc(resolver, builder, name);
	json_builder_end_array(builder);
	/* The worst of the three, error aside: a lookup that failed says
	 * nothing about the domain. */
	for (i = 0; i < G_N_ELEMENTS(statuses); i++) {
		CheckStatus status = statuses[i] == CHECK_ERROR ? CHECK_PASS : statuses[i];
		if (status > overall) overall = status;
	}
	if (statuses[0] == CHECK_ERROR && statuses[1] == CHECK_ERROR && statuses[2] == CHECK_ERROR) overall = CHECK_ERROR;
	json_builder_set_member_name(builder, "overall"); json_builder_add_string_value(builder, status_names[overall]);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}
