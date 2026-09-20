/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "venture-sales-private.h"

struct _VentureSalesService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureSalesService, venture_sales_service, G_TYPE_OBJECT)
static void sales_finalize(GObject *object)
{
	VentureSalesService *self = VENTURE_SALES_SERVICE(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_sales_service_parent_class)->finalize(object);
}
static void venture_sales_service_class_init(VentureSalesServiceClass *klass)
{ G_OBJECT_CLASS(klass)->finalize = sales_finalize; }
static void venture_sales_service_init(VentureSalesService *self) { (void)self; }
VentureSalesService *venture_sales_service_get(VentureDatabase *database)
{
	VentureSalesService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-sales-service");
	if (!self) {
		self = g_object_new(VENTURE_TYPE_SALES_SERVICE, NULL); self->database = database;
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
		g_object_set_data_full(G_OBJECT(database), "venture-sales-service", self, g_object_unref);
	}
	return self;
}
static gboolean fail(GError **error, const gchar *message)
{ g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureSalesService: %s", message); return FALSE; }
static gboolean module_enabled(void)
{ return venture_entity_registry_lookup(venture_entity_registry_get_default(), "sales_territory") != G_TYPE_INVALID; }
/* Once history exists, hiding its UI must not stop cancellation recording. */
static gboolean tracking_ready(VentureDatabase *db, gboolean *ready, GError **error)
{
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GHashTable) columns = NULL;
	*ready = TRUE;
	if (module_enabled()) return TRUE;
	prototype = g_object_new(VENTURE_TYPE_SALES_CREDIT, NULL);
	columns = venture_schema_get_existing_columns(venture_database_get_connection(db), venture_entity_get_table_name(prototype), error);
	if (!columns) return FALSE;
	*ready = g_hash_table_size(columns) != 0;
	return TRUE;
}
static gint64 number(VentureEntity *entity, const gchar *field)
{
	gint64 value = 0;
	GParamSpec *spec;
	if (!entity) return 0;
	spec = g_object_class_find_property(G_OBJECT_GET_CLASS(entity), field);
	if (G_TYPE_IS_ENUM(G_PARAM_SPEC_VALUE_TYPE(spec))) {
		gint enumeration = 0; g_object_get(entity, field, &enumeration, NULL); return enumeration;
	}
	g_object_get(entity, field, &value, NULL); return value;
}
static gchar *string(VentureEntity *entity, const gchar *field)
{ gchar *value = NULL; if (entity) g_object_get(entity, field, &value, NULL); return value; }
static gboolean active(VentureEntity *entity)
{ gboolean value = FALSE; if (entity) g_object_get(entity, "active", &value, NULL); return value && !venture_entity_is_deleted(entity); }
static VentureEntity *reference(VentureDatabase *db, GType type, gint64 id, gint64 org, GError **error)
{
	VentureEntity *entity = id > 0 ? venture_database_get(db, type, id, NULL) : NULL;
	if (!entity || venture_entity_is_deleted(entity) || venture_entity_get_organization_id(entity) != org) {
		g_clear_object(&entity); fail(error, "reference must be live and in this organization"); return NULL;
	}
	return entity;
}
static gboolean member(VentureDatabase *db, gint64 org, gint64 user, gint64 team, gboolean require_active)
{
	g_autoptr(VentureQuery) query = venture_query_new(team > 0 ? VENTURE_TYPE_TEAM_MEMBERSHIP : VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, org); venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user, NULL);
	if (team > 0) venture_query_add_filter_int(query, "team-id", VENTURE_FILTER_OP_EQ, team, NULL);
	rows = venture_database_find(db, query, NULL);
	for (i = 0; rows && i < rows->len; i++) if (!require_active || active(g_ptr_array_index(rows, i))) return TRUE;
	return FALSE;
}
/* User rows are private. Only identities verified against explicit business
 * membership leave this internal lookup; no credential property is copied. */
static VentureEntity *rep(VentureDatabase *db, gint64 org, gint64 id, const gchar *name,
	gint64 team, gboolean require_active, gboolean require_member, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	g_autoptr(VentureEntity) user = NULL;
	if (id > 0) user = venture_database_get(db, VENTURE_TYPE_USER, id, NULL);
	else if (!venture_string_is_empty(name)) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
		venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, name, NULL);
		user = venture_database_find_one(db, query, NULL);
	}
	if (!user || venture_entity_is_deleted(user) || (require_active && !active(user)) ||
		!member(db, org, venture_entity_get_id(user), 0, require_active) ||
		(team > 0 && !member(db, org, venture_entity_get_id(user), team, require_active))) {
		if (user || require_member || id > 0) fail(error, "representative must hold the required organization/team membership and active account");
		return NULL;
	}
	return g_steal_pointer(&user);
}
gboolean venture_sales_routing_validate(VentureDatabase *db, VentureEntity *rule, GError **error)
{
	g_autoptr(VentureEntity) territory = NULL, team = NULL;
	gint64 id, org;
	g_return_val_if_fail(VENTURE_IS_DATABASE(db), FALSE);
	g_return_val_if_fail(VENTURE_IS_LEAD_ROUTING_RULE(rule), FALSE);
	id = number(rule, "territory-id"); if (id == 0) return TRUE;
	if (!module_enabled()) return fail(error, "territory routing requires sales_performance");
	org = venture_entity_get_organization_id(rule);
	territory = reference(db, VENTURE_TYPE_SALES_TERRITORY, id, org, error); if (!territory) return FALSE;
	team = reference(db, VENTURE_TYPE_TEAM, number(territory, "team-id"), org, error); if (!team) return FALSE;
	if (number(rule, "team-id") != venture_entity_get_id(team)) return fail(error, "territory rules must name their territory's owning team");
	return TRUE;
}
gboolean venture_sales_routing_territory(VentureDatabase *db, VentureEntity *rule,
	VentureEntity *lead, gboolean *eligible, GError **error)
{
	g_autoptr(VentureEntity) territory = NULL;
	gint64 id;
	g_return_val_if_fail(VENTURE_IS_DATABASE(db), FALSE);
	g_return_val_if_fail(VENTURE_IS_LEAD_ROUTING_RULE(rule), FALSE);
	g_return_val_if_fail(VENTURE_IS_LEAD(lead), FALSE);
	g_return_val_if_fail(eligible != NULL, FALSE);
	*eligible = TRUE; id = number(rule, "territory-id");
	if (id == 0) { g_object_set(lead, "territory-id", (gint64)0, "team-id", number(rule, "team-id"), NULL); return TRUE; }
	if (!module_enabled()) { *eligible = FALSE; return TRUE; }
	territory = venture_database_get(db, VENTURE_TYPE_SALES_TERRITORY, id, NULL);
	if (!territory || !active(territory)) { *eligible = FALSE; return TRUE; }
	if (venture_entity_get_organization_id(rule) != venture_entity_get_organization_id(lead) ||
		!venture_sales_routing_validate(db, rule, error)) return FALSE;
	g_object_set(lead, "territory-id", id, "team-id", number(territory, "team-id"), NULL); return TRUE;
}
/* A converted deal copies its lead's assignment rather than choosing one.
 * Judging that copy as a new assignment would refuse the conversion once the
 * routed territory or representative has been deactivated, which the docs
 * promise never reassigns existing work. The lead is the baseline instead. */
static VentureEntity *inherited(VentureDatabase *db, VentureEntity *entity)
{
	VentureEntity *lead;
	if (!VENTURE_IS_DEAL(entity)) return NULL;
	lead = venture_lead_service_converting_source(venture_database_get_lead_service(db));
	if (!lead || venture_entity_get_organization_id(lead) != venture_entity_get_organization_id(entity)) return NULL;
	return lead;
}
static gboolean ownership(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, GError **error)
{
	VentureEntity *basis = previous ? previous : inherited(db, entity);
	g_autofree gchar *owner = string(entity, "owner"), *old_owner = string(basis, "owner");
	g_autoptr(VentureEntity) territory = NULL, team = NULL, user = NULL;
	gint64 org = venture_entity_get_organization_id(entity), territory_id = number(entity, "territory-id"),
		team_id = number(entity, "team-id"), user_id = number(entity, "owner-user-id");
	gboolean owner_changed = g_strcmp0(owner, old_owner) != 0;
	gboolean id_changed = user_id != number(basis, "owner-user-id");
	gboolean changed = !basis || owner_changed || id_changed || team_id != number(basis, "team-id") || territory_id != number(basis, "territory-id");
	if (!changed) return TRUE;
	if (territory_id > 0) {
		territory = reference(db, VENTURE_TYPE_SALES_TERRITORY, territory_id, org, error); if (!territory) return FALSE;
		if (!active(territory)) return fail(error, "new assignments need an active territory");
		/* Choosing a territory fills its team; an explicitly different team
		 * is refused instead of producing inconsistent owning references. */
		if (team_id == 0 || (basis && team_id == number(basis, "team-id") && territory_id != number(basis, "territory-id"))) {
			team_id = number(territory, "team-id"); g_object_set(entity, "team-id", team_id, NULL);
		}
		if (team_id != number(territory, "team-id")) return fail(error, "assignment team does not own this territory");
	}
	if (team_id > 0) { team = reference(db, VENTURE_TYPE_TEAM, team_id, org, error); if (!team) return FALSE; }
	if (owner_changed && !id_changed) user_id = 0;
	if (id_changed && !owner_changed && user_id > 0) { g_clear_pointer(&owner, g_free); }
	if (user_id > 0 || !venture_string_is_empty(owner)) {
		g_autofree gchar *resolved = NULL;
		user = rep(db, org, user_id, owner, team_id, TRUE, team_id > 0, error);
		if (!user) {
			if (error && *error) return FALSE;
			g_object_set(entity, "owner-user-id", (gint64)0, NULL); return TRUE;
		}
		resolved = string(user, "username");
		if (user_id > 0 && !venture_string_is_empty(owner) && g_strcmp0(owner, resolved)) return fail(error, "owner account and username disagree");
		g_object_set(entity, "owner", resolved, "owner-user-id", venture_entity_get_id(user), NULL);
	} else g_object_set(entity, "owner-user-id", (gint64)0, NULL);
	return TRUE;
}
static gboolean midnight(GDateTime *value)
{ return value && g_date_time_to_unix(value) % 86400 == 0 && g_date_time_get_microsecond(value) == 0; }
static gboolean quota(VentureDatabase *db, VentureEntity *entity, GError **error)
{
	g_autoptr(GDateTime) start = NULL, end = NULL;
	g_autoptr(VentureMoney) target = NULL;
	g_autoptr(VentureEntity) recipient = NULL;
	g_autofree gchar *metric = string(entity, "metric"), *key = NULL;
	gint64 user = number(entity, "owner-user-id"), team = number(entity, "team-id"), org = venture_entity_get_organization_id(entity);
	g_object_get(entity, "starts-at", &start, "ends-at", &end, "target", &target, NULL);
	if (user < 0 || team < 0 || (user > 0) == (team > 0)) return fail(error, "choose exactly one representative or team");
	if (g_strcmp0(metric, "booked_revenue")) return fail(error, "the supported quota metric is booked_revenue");
	if (!target || venture_money_is_zero(target) || venture_money_is_negative(target)) return fail(error, "target must be a positive exact monetary amount with currency");
	if (!midnight(start) || !midnight(end) || g_date_time_compare(start, end) >= 0) return fail(error, "quota periods require increasing inclusive/exclusive UTC-midnight boundaries");
	recipient = user > 0 ? rep(db, org, user, NULL, 0, FALSE, TRUE, error) : reference(db, VENTURE_TYPE_TEAM, team, org, error);
	if (!recipient) return FALSE;
	key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s:%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
		org, user, team, metric, venture_money_get_currency(target), g_date_time_to_unix(start), g_date_time_to_unix(end));
	g_object_set(entity, "quota-key", key, NULL); return TRUE;
}
static gboolean write_evidence(VentureSalesService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	VentureEntity *previous = self->writing;
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	gboolean result;
	self->writing = record; result = venture_database_save(self->database, record, actor, error); self->writing = previous; return result;
}
static gboolean assignment(VentureSalesService *self, VentureEntity *entity, VentureEntity *previous, const VentureActor *actor, GError **error)
{
	g_autofree gchar *owner = string(entity, "owner"), *old_owner = string(previous, "owner"), *key = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	if (!g_strcmp0(owner, old_owner) && number(entity, "owner-user-id") == number(previous, "owner-user-id") && number(entity, "team-id") == number(previous, "team-id") &&
		number(entity, "territory-id") == number(previous, "territory-id")) return TRUE;
	key = g_strdup_printf("%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, VENTURE_IS_LEAD(entity) ? "lead" : "deal",
		venture_entity_get_id(entity), venture_entity_get_version(entity));
	record = g_object_new(VENTURE_TYPE_SALES_ASSIGNMENT, "organization-id", venture_entity_get_organization_id(entity),
		"name", "Sales assignment", "assignment-key", key, VENTURE_IS_LEAD(entity) ? "lead-id" : "deal-id", venture_entity_get_id(entity),
		"previous-owner", old_owner, "owner", owner, "previous-owner-user-id", number(previous, "owner-user-id"), "owner-user-id", number(entity, "owner-user-id"), "previous-team-id", number(previous, "team-id"), "team-id", number(entity, "team-id"),
		"previous-territory-id", number(previous, "territory-id"), "territory-id", number(entity, "territory-id"), "assigned-at", now, NULL);
	return write_evidence(self, record, actor, error);
}
static VentureEntity *last_credit(VentureDatabase *db, VentureEntity *deal, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SALES_CREDIT);
	venture_query_set_organization(query, venture_entity_get_organization_id(deal));
	venture_query_add_filter_int(query, "deal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(deal), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	return venture_database_find_one(db, query, error);
}
static gboolean credit(VentureSalesService *self, VentureEntity *deal, VentureEntity *previous, const VentureActor *actor, GError **error)
{
	gboolean won = number(deal, "stage") == VENTURE_DEAL_STAGE_WON;
	gboolean was_won = previous && number(previous, "stage") == VENTURE_DEAL_STAGE_WON;
	g_autoptr(VentureEntity) original = NULL, record = NULL;
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *key = NULL;
	if (won == was_won) return TRUE;
	if (!won) {
		g_autofree gchar *kind = NULL;
		g_autoptr(GError) lookup_error = NULL;
		original = last_credit(self->database, deal, &lookup_error);
		if (lookup_error) { g_propagate_error(error, g_steal_pointer(&lookup_error)); return FALSE; }
		/* Legacy wins have no captured historical owner/value to reverse. */
		if (!original) return TRUE;
		kind = string(original, "kind"); if (g_strcmp0(kind, "booking")) return TRUE;
	}
	key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s", venture_entity_get_id(deal), venture_entity_get_version(deal), won ? "booking" : "reversal");
	record = g_object_new(VENTURE_TYPE_SALES_CREDIT, "organization-id", venture_entity_get_organization_id(deal),
		"name", won ? "Won booking" : "Booking cancellation", "credit-key", key, "deal-id", venture_entity_get_id(deal),
		"kind", won ? "booking" : "reversal", "metric", "booked_revenue", NULL);
	if (won) {
		g_autofree gchar *owner = string(deal, "owner"), *team_name = NULL, *territory_name = NULL;
		g_autoptr(VentureEntity) team = NULL, territory = NULL;
		gint64 org = venture_entity_get_organization_id(deal);
		/* The booking is credited to whoever the deal names at the win. An
		 * assignment ownership() accepted is not re-judged here: a rep or
		 * membership deactivated since keeps the attainment it earned. */
		if (number(deal, "team-id") > 0) { team = reference(self->database, VENTURE_TYPE_TEAM, number(deal, "team-id"), org, error); if (!team) return FALSE; }
		if (number(deal, "territory-id") > 0) { territory = reference(self->database, VENTURE_TYPE_SALES_TERRITORY, number(deal, "territory-id"), org, error); if (!territory) return FALSE; }
		team_name = string(team, "name"); territory_name = string(territory, "name");
		g_object_get(deal, "value", &value, "closed-at", &when, NULL); if (!when) when = venture_time_now();
		if (value && venture_money_is_negative(value)) return fail(error, "booked sales cannot have a negative value");
		g_object_set(record, "owner-user-id", number(deal, "owner-user-id"), "rep-name", owner,
			"team-id", number(deal, "team-id"), "team-name", team_name, "territory-id", number(deal, "territory-id"), "territory-name", territory_name, NULL);
	} else {
		static const gchar *const fields[] = { "owner-user-id", "rep-name", "team-id", "team-name", "territory-id", "territory-name", NULL };
		g_autoptr(VentureMoney) positive = NULL;
		guint i;
		for (i = 0; fields[i]; i++) {
			GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(original), fields[i]);
			GValue field = G_VALUE_INIT;
			g_value_init(&field, G_PARAM_SPEC_VALUE_TYPE(spec)); g_object_get_property(G_OBJECT(original), fields[i], &field);
			g_object_set_property(G_OBJECT(record), fields[i], &field); g_value_unset(&field);
		}
		g_object_get(original, "value", &positive, NULL);
		if (positive) { value = venture_money_multiply_int(positive, -1, error); if (!value) return FALSE; }
		when = venture_time_now(); g_object_set(record, "reverses-id", venture_entity_get_id(original), NULL);
	}
	g_object_set(record, "value", value, "credited-at", when, NULL); return write_evidence(self, record, actor, error);
}
gboolean venture_sales_check_write(VentureDatabase *db, VentureEntity *entity, gboolean removal, GError **error)
{
	if (VENTURE_IS_SALES_CREDIT(entity) || VENTURE_IS_SALES_ASSIGNMENT(entity)) {
		VentureSalesService *service = venture_sales_service_get(db);
		if (removal || service->writing != entity) return fail(error, "sales credit and assignment evidence are immutable service-owned records");
		service->writing = NULL;
	}
	if (removal && VENTURE_IS_DEAL(entity)) {
		g_autoptr(VentureEntity) stored = NULL;
		gboolean ready;
		if (!tracking_ready(db, &ready, error)) return FALSE;
		if (!ready) return TRUE;
		stored = venture_database_get(db, VENTURE_TYPE_DEAL, venture_entity_get_id(entity), error);
		if (!stored) {
			if (error && !*error) g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "VentureSalesService: deal %" G_GINT64_FORMAT " does not exist", venture_entity_get_id(entity));
			return FALSE;
		}
		/* Restoring or purging a row deleted before this module tracked wins
		 * cancels no credit; a deleted deal cannot be moved off WON first. */
		if (venture_entity_is_deleted(stored)) return TRUE;
		if (number(stored, "stage") == VENTURE_DEAL_STAGE_WON) return fail(error, "move a won deal to an open or lost stage before cancellation/removal");
	}
	return TRUE;
}
gboolean venture_sales_save_hook(VentureDatabase *db, VentureEntity *entity, const VentureActor *actor,
	VentureSalesSaveContinuation save, gboolean *handled, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	VentureSalesService *self;
	gboolean source = VENTURE_IS_LEAD(entity) || VENTURE_IS_DEAL(entity);
	gboolean ready;
	*handled = FALSE;
	if (!source && !VENTURE_IS_SALES_TERRITORY(entity) && !VENTURE_IS_SALES_QUOTA(entity) && !VENTURE_IS_LEAD_ROUTING_RULE(entity)) return TRUE;
	if (source) {
		if (!tracking_ready(db, &ready, error)) return FALSE;
		if (!ready) return TRUE;
	}
	*handled = TRUE; self = venture_sales_service_get(db);
	if (!venture_database_begin(db, error)) return FALSE;
	if (venture_entity_is_persisted(entity)) {
		previous = venture_database_get(db, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error); if (!previous) goto failed;
		if (venture_entity_get_organization_id(previous) != venture_entity_get_organization_id(entity)) { fail(error, "sales records cannot move between organizations"); goto failed; }
	}
	if (source && !ownership(db, entity, previous, error)) goto failed;
	if (VENTURE_IS_SALES_TERRITORY(entity)) {
		g_autoptr(VentureEntity) team = reference(db, VENTURE_TYPE_TEAM, number(entity, "team-id"), venture_entity_get_organization_id(entity), error); if (!team) goto failed;
	}
	if (VENTURE_IS_SALES_QUOTA(entity) && !quota(db, entity, error)) goto failed;
	if (VENTURE_IS_LEAD_ROUTING_RULE(entity) && !venture_sales_routing_validate(db, entity, error)) goto failed;
	if (VENTURE_IS_DEAL(entity) && previous && number(previous, "stage") == VENTURE_DEAL_STAGE_WON && number(entity, "stage") == VENTURE_DEAL_STAGE_WON) {
		g_autoptr(VentureMoney) before = NULL, after = NULL;
		g_object_get(previous, "value", &before, NULL); g_object_get(entity, "value", &after, NULL);
		if (!venture_money_equal(before, after)) { fail(error, "reopen a won deal before changing its booked value or currency"); goto failed; }
	}
	if (!save(db, entity, actor, error)) goto failed;
	if (source && !assignment(self, entity, previous, actor, error)) goto failed;
	if (VENTURE_IS_DEAL(entity) && !credit(self, entity, previous, actor, error)) goto failed;
	if (!venture_database_commit(db, error)) goto failed;
	return TRUE;
failed:
	venture_database_rollback(db); return FALSE;
}

/* Evidence stays linked even when its optional module is currently hidden. */
gboolean venture_sales_check_purge(VentureDatabase *db, VentureEntity *entity, GError **error)
{
	GType types[2];
	g_autoptr(VentureAccessScope) internal = NULL;
	guint i;
	if (!VENTURE_IS_LEAD(entity) && !VENTURE_IS_DEAL(entity)) return TRUE;
	/* Authorization has already checked the source. Hidden evidence must
	 * still prevent deletion of its historical referent. */
	internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	types[0] = VENTURE_TYPE_SALES_ASSIGNMENT; types[1] = VENTURE_TYPE_SALES_CREDIT;
	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		g_autoptr(VentureEntity) prototype = g_object_new(types[i], NULL);
		g_autoptr(GHashTable) columns = venture_schema_get_existing_columns(venture_database_get_connection(db), venture_entity_get_table_name(prototype), error);
		g_autoptr(VentureQuery) query = NULL;
		gint64 count;
		if (!columns) return FALSE;
		if (g_hash_table_size(columns) == 0 || (i == 1 && VENTURE_IS_LEAD(entity))) continue;
		query = venture_query_new(types[i]);
		venture_query_add_filter_int(query, VENTURE_IS_LEAD(entity) ? "lead-id" : "deal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(entity), NULL);
		count = venture_database_count(db, query, error);
		if (count < 0) return FALSE;
		if (count > 0) return fail(error, "sales evidence retains this source; archive it instead of purging");
	}
	return TRUE;
}
