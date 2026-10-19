/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The preference centre (docs/marketing.org, "Preferences"). One row per
 * address and organization, written only from the address's own
 * capability page or by an operator recording what the person asked;
 * absent means "everything, as often as you send". Campaigns ask it at
 * preview and again before queueing; a marketing journey (a sequence with
 * a topic) asks it before each email step. Stop-everything is still the
 * existing suppression, which every sender already honours. */

static gint compare_ids(gconstpointer a, gconstpointer b)
{
	gint64 left = *(const gint64 *)a, right = *(const gint64 *)b;
	return left < right ? -1 : left > right;
}
static gint frequency_days(gint frequency)
{
	return frequency == 1 ? 7 : frequency == 2 ? 30 : 0;
}
static gboolean parse_frequency(const gchar *text, gint *value, GError **error)
{
	if (!g_strcmp0(text, "any") || !text || !*text) *value = 0;
	else if (!g_strcmp0(text, "weekly")) *value = 1;
	else if (!g_strcmp0(text, "monthly")) *value = 2;
	else return refuse(error, "How often is any, weekly or monthly");
	return TRUE;
}
/* Topic ids, each an offered topic of this organization, sorted and once. */
static gchar *normalize_topics(VentureMarketingService *self, gint64 org, const gchar *text, GError **error)
{
	g_auto(GStrv) parts = g_strsplit(text ? text : "", ",", -1);
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;
	for (i = 0; parts[i]; i++) {
		gint64 id = 0;
		g_autoptr(VentureEntity) topic = NULL;
		g_strstrip(parts[i]);
		if (!*parts[i]) continue;
		if (!g_ascii_string_to_signed(parts[i], 10, 1, G_MAXINT64, &id, NULL)) { refuse(error, "Topics are topic numbers"); return NULL; }
		topic = venture_database_get(self->database, VENTURE_TYPE_MARKETING_TOPIC, id, NULL);
		if (!topic || venture_entity_is_deleted(topic) || venture_entity_get_organization_id(topic) != org) {
			refuse(error, "No such topic in this organization"); return NULL;
		}
		g_array_append_val(ids, id);
	}
	g_array_sort(ids, compare_ids);
	for (i = 0; i < ids->len; i++) {
		gint64 id = g_array_index(ids, gint64, i);
		if (i && g_array_index(ids, gint64, i - 1) == id) continue;
		g_string_append_printf(out, "%s%" G_GINT64_FORMAT, out->len ? "," : "", id);
	}
	return g_string_free(g_steal_pointer(&out), FALSE);
}
static gboolean topic_listed(const gchar *topics, gint64 topic)
{
	g_auto(GStrv) parts = g_strsplit(topics ? topics : "", ",", -1);
	guint i;
	for (i = 0; parts[i]; i++)
		if (g_ascii_strtoll(parts[i], NULL, 10) == topic) return TRUE;
	return FALSE;
}
static gboolean store_preferences(VentureMarketingService *self, gint64 org, const gchar *email,
	const gchar *topics_off, const gchar *frequency_text, const gchar *source, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *topics = NULL, *address = normalize(email);
	gint frequency = 0;
	if (!valid_address(address)) return refuse(error, "Preferences need one valid address");
	if (!parse_frequency(frequency_text, &frequency, error)) return FALSE;
	topics = normalize_topics(self, org, topics_off, error);
	if (!topics) return FALSE;
	row = find_string(self, VENTURE_TYPE_MARKETING_PREFERENCE, org, "email", address, error);
	if (error && *error) return FALSE;
	if (!row) row = g_object_new(VENTURE_TYPE_MARKETING_PREFERENCE, "organization-id", org, "email", address, NULL);
	g_object_set(row, "topics-off", topics, "frequency", frequency, "chosen-at", now, "source", source, NULL);
	return save(self, row, actor, error);
}
gboolean venture_marketing_service_set_preferences(VentureMarketingService *self, const gchar *token,
	const gchar *topics_off, const gchar *frequency, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) recipient = NULL;
	g_autofree gchar *email = NULL;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), FALSE);
	if (!self->database) return refuse(error, "Marketing database is unavailable");
	/* The capability is the authority, as for unsubscribing. */
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!venture_database_begin(self->database, error)) return FALSE;
	recipient = token_recipient(self, token, "unsubscribe-hash", error);
	if (!recipient) goto fail;
	email = string(recipient, "email");
	if (!store_preferences(self, venture_entity_get_organization_id(recipient), email, topics_off, frequency,
		"The recipient's preference page", NULL, error)) goto fail;
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database); return FALSE;
}
gboolean venture_marketing_service_record_preferences(VentureMarketingService *self, gint64 organization_id,
	const gchar *email, const gchar *topics_off, const gchar *frequency, const gchar *source,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), FALSE);
	if (!allowed(self, organization_id, error)) return FALSE;
	if (!bounded(source, 2048)) return refuse(error, "Say what the person asked and how");
	if (!venture_database_begin(self->database, error)) return FALSE;
	if (!store_preferences(self, organization_id, email, topics_off, frequency, source, actor, error)) {
		venture_database_rollback(self->database); return FALSE;
	}
	return venture_database_commit(self->database, error);
}
VentureEntity *venture_marketing_service_preferences_for(VentureMarketingService *self,
	gint64 organization_id, const gchar *email, GError **error)
{
	g_autofree gchar *address = normalize(email);
	VentureEntity *row;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), NULL);
	row = find_string(self, VENTURE_TYPE_MARKETING_PREFERENCE, organization_id, "email", address, error);
	if (row && venture_entity_is_deleted(row)) g_clear_object(&row);
	return row;
}
gboolean venture_marketing_service_preference_view(VentureMarketingService *self, const gchar *token,
	GPtrArray **topics, VentureEntity **preference, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) recipient = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_TOPIC);
	g_autofree gchar *email = NULL;
	gint64 org;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), FALSE);
	if (!self->database) return refuse(error, "Marketing database is unavailable");
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	recipient = token_recipient(self, token, "unsubscribe-hash", error);
	if (!recipient) return FALSE;
	org = venture_entity_get_organization_id(recipient);
	email = string(recipient, "email");
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 100);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	*topics = venture_database_find(self->database, query, error);
	if (!*topics) return FALSE;
	*preference = venture_marketing_service_preferences_for(self, org, email, error);
	return !error || !*error;
}
/* Marketing messages to @email whose clock falls in [from, now]: campaign
 * mail the outbox took, and email steps of marketing journeys. */
static gint64 recent_messages(VentureDatabase *database, gint64 org, const gchar *email, GDateTime *from,
	GDateTime *now, GError **error)
{
	g_autoptr(VentureQuery) recipients = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	g_autoptr(VentureQuery) people = NULL;
	g_autoptr(GPtrArray) rows = NULL, contacts = NULL;
	gint64 count = 0;
	guint i, j;
	venture_query_set_organization(recipients, org);
	venture_query_set_limit(recipients, 200);
	venture_query_add_filter_string(recipients, "email", VENTURE_FILTER_OP_EQ, email, NULL);
	venture_query_add_filter_int(recipients, "mail-id", VENTURE_FILTER_OP_GT, 0, NULL);
	venture_query_add_order(recipients, "id", VENTURE_SORT_DESCENDING, NULL);
	rows = venture_database_find(database, recipients, error);
	if (!rows) return -1;
	for (i = 0; i < rows->len; i++) {
		g_autoptr(VentureEntity) mail = venture_database_get(database, VENTURE_TYPE_MAIL_MESSAGE, number(g_ptr_array_index(rows, i), "mail-id"), NULL);
		g_autoptr(GDateTime) at = NULL;
		g_autofree gchar *state = mail ? string(mail, "state") : NULL;
		if (!mail || !g_strcmp0(state, "cancelled") || !g_strcmp0(state, "dead")) continue;
		g_object_get(mail, "created-at", &at, NULL);
		if (at && g_date_time_compare(at, from) >= 0 && g_date_time_compare(at, now) <= 0) count++;
	}
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "sequence")) return count;
	people = venture_query_new(VENTURE_TYPE_CONTACT);
	venture_query_set_organization(people, org);
	venture_query_set_limit(people, 20);
	venture_query_add_filter_string(people, "email", VENTURE_FILTER_OP_ILIKE, email, NULL);
	contacts = venture_database_find(database, people, error);
	if (!contacts) return -1;
	for (i = 0; i < contacts->len; i++) {
		VentureEntity *person = g_ptr_array_index(contacts, i);
		g_autoptr(VentureQuery) enrolled = venture_query_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT);
		g_autoptr(GPtrArray) enrollments = NULL;
		g_autofree gchar *raw = string(person, "email"), *address = normalize(raw);
		if (g_strcmp0(address, email)) continue;
		venture_query_set_organization(enrolled, org);
		venture_query_set_limit(enrolled, 200);
		venture_query_add_filter_int(enrolled, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(person), NULL);
		enrollments = venture_database_find(database, enrolled, error);
		if (!enrollments) return -1;
		for (j = 0; j < enrollments->len; j++) {
			VentureEntity *enrollment = g_ptr_array_index(enrollments, j);
			g_autoptr(VentureEntity) sequence = venture_database_get(database, VENTURE_TYPE_SEQUENCE, number(enrollment, "sequence-id"), NULL);
			g_autoptr(VentureQuery) sent = NULL;
			g_autofree gchar *low = NULL, *high = NULL;
			g_autoptr(GPtrArray) deliveries = NULL;
			guint k;
			if (!sequence || number(sequence, "topic-id") <= 0) continue;
			sent = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
			low = venture_time_to_string(from); high = venture_time_to_string(now);
			venture_query_set_organization(sent, org);
			venture_query_set_limit(sent, 200);
			venture_query_add_filter_int(sent, "enrollment-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(enrollment), NULL);
			venture_query_add_filter_string(sent, "scheduled-at", VENTURE_FILTER_OP_GTE, low, NULL);
			venture_query_add_filter_string(sent, "scheduled-at", VENTURE_FILTER_OP_LTE, high, NULL);
			deliveries = venture_database_find(database, sent, error);
			if (!deliveries) return -1;
			for (k = 0; k < deliveries->len; k++) {
				VentureEntity *delivery = g_ptr_array_index(deliveries, k);
				gint channel = choice(delivery, "channel"), state = choice(delivery, "state");
				if (channel == 0 && (state == 0 || state == 1)) count++;
			}
		}
	}
	return count;
}
gboolean venture_marketing_preference_allows(VentureDatabase *database, gint64 organization_id,
	const gchar *email, gint64 topic_id, GDateTime *now, const gchar **reason, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *topics = NULL;
	gint frequency = 0;
	if (reason) *reason = NULL;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "marketing_preference")) return TRUE;
	query = venture_query_new(VENTURE_TYPE_MARKETING_PREFERENCE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, email, NULL);
	row = venture_database_find_one(database, query, error);
	if (error && *error) return FALSE;
	if (!row) return TRUE;
	topics = string(row, "topics-off");
	if (topic_id > 0 && topic_listed(topics, topic_id)) {
		if (reason) *reason = "topic_turned_off";
		return FALSE;
	}
	frequency = choice(row, "frequency");
	if (frequency_days(frequency) && now) {
		g_autoptr(GDateTime) from = g_date_time_add_days(now, -frequency_days(frequency));
		gint64 recent = recent_messages(database, organization_id, email, from, now, error);
		if (recent < 0) return FALSE;
		if (recent > 0) {
			if (reason) *reason = "frequency";
			return FALSE;
		}
	}
	return TRUE;
}
/* The words a journey's skipped step keeps. */
static const gchar *preference_words(const gchar *reason)
{
	if (!g_strcmp0(reason, "topic_turned_off")) return "The recipient turned this topic off on their preference page";
	return "The recipient asked for fewer emails (at most one a week or a month)";
}
static gboolean journey_gate(VentureDatabase *database, VentureEntity *sequence, VentureEntity *contact,
	GDateTime *as_of, gchar **skip_reason, gpointer data, GError **error)
{
	g_autofree gchar *raw = NULL, *email = NULL;
	const gchar *reason = NULL;
	gint64 topic = number(sequence, "topic-id");
	(void)data;
	if (topic <= 0) return TRUE;
	raw = string(contact, "email"); email = normalize(raw);
	if (venture_marketing_preference_allows(database, venture_entity_get_organization_id(contact), email, topic, as_of, &reason, error))
		return TRUE;
	if (error && *error) return FALSE;
	*skip_reason = g_strdup(preference_words(reason));
	return TRUE;
}
