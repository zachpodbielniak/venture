/*
 * venture-confirmation-store.c - Changes proposed but not yet made
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/* ==========================================================================
 * One staged change
 * ========================================================================== */

struct _VentureConfirmation
{
	GObject parent_instance;

	gchar				*id;
	gchar				*summary;
	gchar				*origin;
	gchar				*via;
	gchar				*prompt;
	VentureActorKind		 origin_kind;
	VentureAuditAction		 action;
	JsonNode			*diff;
	VentureConfirmationState	 state;
	GDateTime			*created_at;
	GDateTime			*expires_at;

	/*
	 * The record with the change already applied in memory, held until a
	 * decision is made. Staging the object rather than the instruction
	 * means approval cannot re-interpret the request differently from
	 * what the diff showed.
	 *
	 * @original is the row as it stood when this was staged. It is what
	 * makes a stale approval explainable: on a conflict it is compared
	 * against the row as it is now, so the refusal can name the fields
	 * somebody else moved instead of saying only that something did.
	 */
	VentureEntity			*staged;
	VentureEntity			*original;
};

G_DEFINE_FINAL_TYPE(VentureConfirmation, venture_confirmation, G_TYPE_OBJECT)

static void
venture_confirmation_finalize(GObject *object)
{
	VentureConfirmation *self;

	self = VENTURE_CONFIRMATION(object);

	g_clear_pointer(&self->id, g_free);
	g_clear_pointer(&self->summary, g_free);
	g_clear_pointer(&self->origin, g_free);
	g_clear_pointer(&self->via, g_free);
	g_clear_pointer(&self->prompt, g_free);
	g_clear_pointer(&self->diff, json_node_unref);
	g_clear_pointer(&self->created_at, g_date_time_unref);
	g_clear_pointer(&self->expires_at, g_date_time_unref);
	g_clear_object(&self->staged);
	g_clear_object(&self->original);

	G_OBJECT_CLASS(venture_confirmation_parent_class)->finalize(object);
}

static void
venture_confirmation_class_init(VentureConfirmationClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_confirmation_finalize;
}

static void
venture_confirmation_init(VentureConfirmation *self)
{
	self->state = VENTURE_CONFIRMATION_STATE_PENDING;
	self->origin_kind = VENTURE_ACTOR_KIND_SYSTEM;
}

const gchar *
venture_confirmation_get_id(VentureConfirmation *self)
{
	g_return_val_if_fail(VENTURE_IS_CONFIRMATION(self), NULL);

	return self->id;
}

const gchar *
venture_confirmation_get_summary(VentureConfirmation *self)
{
	g_return_val_if_fail(VENTURE_IS_CONFIRMATION(self), NULL);

	return self->summary;
}

JsonNode *
venture_confirmation_get_diff(VentureConfirmation *self)
{
	g_return_val_if_fail(VENTURE_IS_CONFIRMATION(self), NULL);

	return self->diff;
}

GType
venture_confirmation_get_entity_type(VentureConfirmation *self)
{
	g_return_val_if_fail(VENTURE_IS_CONFIRMATION(self), G_TYPE_INVALID);

	if (NULL == self->staged)
		return G_TYPE_INVALID;

	return G_OBJECT_TYPE(self->staged);
}

JsonNode *
venture_confirmation_to_json(VentureConfirmation *self)
{
	g_autoptr(JsonBuilder) builder = NULL;

	g_return_val_if_fail(VENTURE_IS_CONFIRMATION(self), NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, self->id);

	json_builder_set_member_name(builder, "summary");
	json_builder_add_string_value(builder, self->summary);

	json_builder_set_member_name(builder, "action");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_AUDIT_ACTION, (gint)self->action));

	json_builder_set_member_name(builder, "state");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_CONFIRMATION_STATE,
		                     (gint)self->state));

	/*
	 * The type, the record and its label, so somebody reading this in
	 * another program can decide without the request that produced it.
	 * A decision inbox in another process is the intended reader and it
	 * cannot go and look at the HTTP call.
	 */
	if (NULL != self->staged)
	{
		g_autofree gchar *label = NULL;

		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder,
			venture_entity_get_entity_name(self->staged));

		label = venture_entity_get_display_name(self->staged);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, label);

		/* Absent rather than zero for a creation: there is no record
		 * yet, and a client that read 0 as an id would go looking. */
		if (0 != venture_entity_get_id(self->staged))
		{
			json_builder_set_member_name(builder, "record_id");
			json_builder_add_int_value(builder,
				venture_entity_get_id(self->staged));
		}
	}

	/*
	 * Who asked, and how. The prompt behind an assistant-staged change is
	 * deliberately *not* here: this endpoint is open to any viewer, and a
	 * prompt is the text of somebody's private chat thread. It reaches
	 * the audit trail instead, which is where "why did this change"
	 * belongs.
	 */
	json_builder_set_member_name(builder, "origin");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "kind");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_ACTOR_KIND, (gint)self->origin_kind));
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, self->origin);
	json_builder_set_member_name(builder, "via");
	json_builder_add_string_value(builder, self->via);
	json_builder_end_object(builder);

	if (NULL != self->diff)
	{
		json_builder_set_member_name(builder, "diff");
		json_builder_add_value(builder, json_node_ref(self->diff));
	}

	if (NULL != self->created_at)
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_string(self->created_at);
		json_builder_set_member_name(builder, "created_at");
		json_builder_add_string_value(builder, text);
	}

	if (NULL != self->expires_at)
	{
		g_autofree gchar *text = NULL;

		text = venture_time_to_string(self->expires_at);
		json_builder_set_member_name(builder, "expires_at");
		json_builder_add_string_value(builder, text);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * The queue
 * ========================================================================== */

struct _VentureConfirmationStore
{
	GObject parent_instance;

	VentureDatabase	*database;
	gint64		 ttl_seconds;
	gint64		 limit;

	/* Confirmation id -> VentureConfirmation. */
	GHashTable	*pending;
};

G_DEFINE_FINAL_TYPE(VentureConfirmationStore, venture_confirmation_store,
                    G_TYPE_OBJECT)

static void
venture_confirmation_store_finalize(GObject *object)
{
	VentureConfirmationStore *self;

	self = VENTURE_CONFIRMATION_STORE(object);

	g_clear_pointer(&self->pending, g_hash_table_unref);
	g_clear_object(&self->database);

	G_OBJECT_CLASS(venture_confirmation_store_parent_class)->finalize(object);
}

static void
venture_confirmation_store_class_init(VentureConfirmationStoreClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_confirmation_store_finalize;
}

static void
venture_confirmation_store_init(VentureConfirmationStore *self)
{
	self->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      g_object_unref);
}

VentureConfirmationStore *
venture_confirmation_store_new(
	VentureDatabase	*database,
	gint64		 ttl_seconds,
	gint64		 limit
){
	VentureConfirmationStore *self;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	self = g_object_new(VENTURE_TYPE_CONFIRMATION_STORE, NULL);
	self->database = g_object_ref(database);
	self->ttl_seconds = (ttl_seconds > 0) ? ttl_seconds : 3600;
	self->limit = (limit > 0) ? limit : 0;

	return self;
}

/*
 * Drops everything that has run out of time.
 *
 * An entry is *removed*, not merely marked, and that is the difference
 * between a queue and a leak: the previous implementation flipped the state
 * to expired and left the object in the table, so a server left running for
 * a month accumulated every change nobody ever answered. Nothing reads an
 * expired confirmation -- it cannot be approved, and its staged object is
 * pinned to a version the row has almost certainly moved past.
 */
static void
venture_confirmation_store_sweep(VentureConfirmationStore *self)
{
	g_autoptr(GDateTime) now = NULL;
	GHashTableIter iter;
	gpointer value;

	now = venture_time_now();

	g_hash_table_iter_init(&iter, self->pending);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		VentureConfirmation *confirmation;

		confirmation = value;

		if ((NULL != confirmation->expires_at) &&
		    (g_date_time_compare(now, confirmation->expires_at) >= 0))
		{
			confirmation->state = VENTURE_CONFIRMATION_STATE_EXPIRED;
			g_hash_table_iter_remove(&iter);
		}
	}
}

/*
 * Builds the one-line description a person is shown.
 *
 * Written here rather than at each staging site so that a change proposed by
 * the assistant and the same change proposed over the REST API read
 * identically in the queue. Two spellings would look like two kinds of
 * change.
 */
static gchar *
venture_confirmation_summarise(
	VentureAuditAction	 action,
	VentureEntity		*record
){
	g_autofree gchar *label = NULL;

	label = venture_entity_get_display_name(record);

	switch (action)
	{
	case VENTURE_AUDIT_ACTION_CREATE:
		return g_strdup_printf("Create %s \"%s\"",
		                       venture_entity_get_entity_name(record), label);

	case VENTURE_AUDIT_ACTION_DELETE:
		/* Saying it is recoverable matters: deletion here is a soft
		 * delete, and presenting it as destruction invites a refusal
		 * that costs more than the change would have. */
		return g_strdup_printf("Delete %s \"%s\" (recoverable)",
		                       venture_entity_get_entity_name(record), label);

	case VENTURE_AUDIT_ACTION_UPDATE:
	case VENTURE_AUDIT_ACTION_LOGIN:
	case VENTURE_AUDIT_ACTION_LOGOUT:
	case VENTURE_AUDIT_ACTION_TOOL_CALL:
	case VENTURE_AUDIT_ACTION_CONFIRM:
	case VENTURE_AUDIT_ACTION_REJECT:
	case VENTURE_AUDIT_ACTION_AUTOMATION:
	case VENTURE_AUDIT_ACTION_EXPORT:
	default:
		break;
	}

	return g_strdup_printf("Update %s \"%s\"",
	                       venture_entity_get_entity_name(record), label);
}

VentureConfirmation *
venture_confirmation_store_stage(
	VentureConfirmationStore	 *self,
	VentureAuditAction		  action,
	VentureEntity			 *staged,
	VentureEntity			 *original,
	const VentureActor		 *origin,
	const gchar			 *via,
	GError				**error
){
	g_autoptr(JsonNode) diff = NULL;
	g_autofree gchar *summary = NULL;
	g_autoptr(GDateTime) now = NULL;
	VentureConfirmation *confirmation;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_CONFIRMATION_STORE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(staged), NULL);
	if (VENTURE_IS_BILLING_REQUEST(staged) && !venture_billing_prepare_request(venture_billing_service_get(self->database), VENTURE_BILLING_REQUEST(staged), error))
		return NULL;

	venture_confirmation_store_sweep(self);

	/* Validate before staging, so nobody is ever asked to approve
	 * something that would fail anyway. */
	if ((VENTURE_AUDIT_ACTION_DELETE != action) &&
	    !venture_entity_validate(staged, error))
		return NULL;

	switch (action)
	{
	case VENTURE_AUDIT_ACTION_CREATE:
		diff = venture_serializable_to_json(VENTURE_SERIALIZABLE(staged),
		                                    FALSE);
		break;

	case VENTURE_AUDIT_ACTION_DELETE:
		/* Nothing changes field by field; the summary carries it. */
		break;

	case VENTURE_AUDIT_ACTION_UPDATE:
	case VENTURE_AUDIT_ACTION_LOGIN:
	case VENTURE_AUDIT_ACTION_LOGOUT:
	case VENTURE_AUDIT_ACTION_TOOL_CALL:
	case VENTURE_AUDIT_ACTION_CONFIRM:
	case VENTURE_AUDIT_ACTION_REJECT:
	case VENTURE_AUDIT_ACTION_AUTOMATION:
	case VENTURE_AUDIT_ACTION_EXPORT:
	default:
		if (NULL == original)
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "An update cannot be staged without the "
			                    "record it would change");
			return NULL;
		}

		diff = venture_entity_diff(original, staged);

		if (0 == json_object_get_size(json_node_get_object(diff)))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_VALIDATION,
			                    "That would not change anything");
			return NULL;
		}

		break;
	}

	summary = venture_confirmation_summarise(action, staged);

	/*
	 * A change that is already waiting is not staged twice.
	 *
	 * Models retry: a call that answers "awaiting approval" rather than
	 * "ok" reads to some of them as a failure worth another go, and the
	 * operator then faces three identical cards for one change and has to
	 * work out whether approving all three makes three records. It would.
	 * Returning the waiting confirmation makes the retry a no-op and
	 * keeps the count of cards equal to the count of changes.
	 */
	g_hash_table_iter_init(&iter, self->pending);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		VentureConfirmation *existing;

		existing = value;

		if ((existing->action != action) ||
		    (0 != g_strcmp0(existing->summary, summary)) ||
		    (0 != g_strcmp0(existing->origin, origin ? origin->name : NULL)))
			continue;

		/*
		 * Same change from the same asker. Replace the staged object
		 * so the newest attempt wins -- a retry usually carries more
		 * of the fields, not fewer -- but keep the one card.
		 */
		g_set_object(&existing->staged, staged);
		g_set_object(&existing->original, original);
		g_clear_pointer(&existing->diff, json_node_unref);
		existing->diff = (NULL != diff) ? json_node_ref(diff) : NULL;

		return existing;
	}

	/*
	 * A cap as well as a lifetime. The lifetime bounds how long one
	 * change waits; only the cap bounds how many an agent in a retry loop
	 * can pile up inside that window, and "it clears itself in an hour"
	 * is no comfort to a server that ran out of memory in ten minutes.
	 */
	if ((self->limit > 0) &&
	    ((gint64)g_hash_table_size(self->pending) >= self->limit))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "There are already %" G_GINT64_FORMAT " changes waiting "
		            "for a decision, which is the configured limit "
		            "(ai.confirmation_limit). Approve or reject some before "
		            "staging more.", self->limit);
		return NULL;
	}

	confirmation = g_object_new(VENTURE_TYPE_CONFIRMATION, NULL);
	confirmation->id = venture_generate_token(8);
	confirmation->summary = g_steal_pointer(&summary);
	confirmation->action = action;
	confirmation->diff = (NULL != diff) ? json_node_ref(diff) : NULL;
	confirmation->staged = g_object_ref(staged);
	confirmation->original = (NULL != original) ? g_object_ref(original) : NULL;
	confirmation->via = g_strdup(via);

	if (NULL != origin)
	{
		confirmation->origin_kind = origin->kind;
		confirmation->origin = g_strdup(origin->name);
		confirmation->prompt = g_strdup(origin->prompt);
	}

	now = venture_time_now();
	confirmation->created_at = g_date_time_ref(now);
	confirmation->expires_at = g_date_time_add_seconds(now,
		(gdouble)self->ttl_seconds);

	g_hash_table_insert(self->pending, g_strdup(confirmation->id),
	                    confirmation);

	return confirmation;
}

static gint
venture_confirmation_compare_created(
	gconstpointer	a,
	gconstpointer	b
){
	VentureConfirmation *first;
	VentureConfirmation *second;

	first = *(VentureConfirmation * const *)a;
	second = *(VentureConfirmation * const *)b;

	if ((NULL == first->created_at) || (NULL == second->created_at))
		return 0;

	return g_date_time_compare(first->created_at, second->created_at);
}

GPtrArray *
venture_confirmation_store_list_pending(VentureConfirmationStore *self)
{
	GPtrArray *pending;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_CONFIRMATION_STORE(self), NULL);

	venture_confirmation_store_sweep(self);

	pending = g_ptr_array_new();

	g_hash_table_iter_init(&iter, self->pending);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(pending, value);

	/* A hash table has no order, and a decision queue that reshuffles
	 * itself between two reads is one nobody can work through. */
	g_ptr_array_sort(pending, venture_confirmation_compare_created);

	return pending;
}

VentureConfirmation *
venture_confirmation_store_find(
	VentureConfirmationStore	*self,
	const gchar			*confirmation_id
){
	g_return_val_if_fail(VENTURE_IS_CONFIRMATION_STORE(self), NULL);

	if (NULL == confirmation_id)
		return NULL;

	venture_confirmation_store_sweep(self);

	return g_hash_table_lookup(self->pending, confirmation_id);
}

/*
 * Names what somebody else changed under a staged update.
 *
 * "Someone changed it since you loaded it" is true and useless: whoever is
 * holding the confirmation has to go and work out what moved before they can
 * decide whether their change still makes sense. The original snapshot is
 * kept precisely so this can be answered.
 */
static gchar *
venture_confirmation_describe_drift(
	VentureConfirmationStore	*self,
	VentureConfirmation		*confirmation
){
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(JsonNode) drift = NULL;
	g_autoptr(GString) names = NULL;
	JsonObject *object;
	GList *members;
	GList *iter;

	if (NULL == confirmation->original)
		return NULL;

	current = venture_database_get(self->database,
	                               G_OBJECT_TYPE(confirmation->original),
	                               venture_entity_get_id(confirmation->original),
	                               NULL);

	if (NULL == current)
		return g_strdup("it has since been removed");

	drift = venture_entity_diff(confirmation->original, current);
	object = json_node_get_object(drift);

	if (0 == json_object_get_size(object))
		return NULL;

	names = g_string_new(NULL);
	members = json_object_get_members(object);

	for (iter = members; NULL != iter; iter = iter->next)
	{
		if (names->len > 0)
			g_string_append(names, ", ");

		g_string_append(names, iter->data);
	}

	g_list_free(members);

	return g_strdup_printf("%s changed since this was staged", names->str);
}

gboolean
venture_confirmation_store_approve(
	VentureConfirmationStore	 *self,
	const gchar			 *confirmation_id,
	const gchar			 *approver,
	GError				**error
){
	g_autoptr(GError) local_error = NULL;
	VentureConfirmation *confirmation;
	VentureActor actor;
	gboolean ok;

	g_return_val_if_fail(VENTURE_IS_CONFIRMATION_STORE(self), FALSE);
	g_return_val_if_fail(NULL != confirmation_id, FALSE);

	venture_confirmation_store_sweep(self);

	confirmation = g_hash_table_lookup(self->pending, confirmation_id);

	if (NULL == confirmation)
	{
		/* An expired one has been swept, so "no such change" covers
		 * both cases; saying which would need a tombstone whose only
		 * consumer is this message. */
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no change waiting with id %s. It may have "
		            "expired, or already been decided.", confirmation_id);
		return FALSE;
	}

	/*
	 * The actor says all three things the trail has to distinguish: who
	 * asked (kind and name, carried from the staging call), that this
	 * went through the queue at all (the confirmation id in request_id,
	 * which a direct write never sets), and who let it through
	 * (approved_by). A staged-then-approved change, a direct one and the
	 * assistant's own are then three different rows rather than three
	 * readings of one.
	 */
	actor.kind = confirmation->origin_kind;
	actor.name = confirmation->origin;
	actor.prompt = confirmation->prompt;
	actor.request_id = confirmation->id;
	actor.approved_by = approver;

	if (VENTURE_AUDIT_ACTION_DELETE == confirmation->action)
	{
		ok = venture_database_delete(self->database, confirmation->staged,
		                             &actor, &local_error);
	}
	else
	{
		ok = venture_database_save(self->database, confirmation->staged,
		                           &actor, &local_error);
	}

	if (ok)
	{
		confirmation->state = VENTURE_CONFIRMATION_STATE_APPROVED;
		g_hash_table_remove(self->pending, confirmation_id);
		return TRUE;
	}

	/*
	 * A conflict is terminal for this confirmation and only for this one.
	 * venture_database_save() bumps the staged record's version before the
	 * UPDATE that then matched no row, so the object now describes a
	 * version that never existed and approving it again could not work.
	 * Drop it, say what moved, and let the change be staged afresh
	 * against the record as it is.
	 */
	if (g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT))
	{
		g_autofree gchar *drift = NULL;

		drift = venture_confirmation_describe_drift(self, confirmation);

		confirmation->state = VENTURE_CONFIRMATION_STATE_FAILED;

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "%s was not applied: %s. Nothing was written -- the "
		            "record was not overwritten with what was staged. "
		            "Look at it as it is now and stage the change again.",
		            confirmation->summary,
		            (NULL != drift) ? drift
		                            : local_error->message);

		g_hash_table_remove(self->pending, confirmation_id);
		return FALSE;
	}

	/*
	 * Anything else -- a validation failure, a reference pointing at a
	 * record that does not exist yet -- is refused before the save
	 * touches the record, so the staged object is exactly as it was and
	 * the confirmation stays approvable once the cause is fixed.
	 */
	g_propagate_error(error, g_steal_pointer(&local_error));

	return FALSE;
}

gboolean
venture_confirmation_store_reject(
	VentureConfirmationStore	 *self,
	const gchar			 *confirmation_id,
	const gchar			 *approver,
	GError				**error
){
	g_autoptr(VentureAuditEntry) entry = NULL;
	VentureConfirmation *confirmation;

	g_return_val_if_fail(VENTURE_IS_CONFIRMATION_STORE(self), FALSE);
	g_return_val_if_fail(NULL != confirmation_id, FALSE);

	venture_confirmation_store_sweep(self);

	confirmation = g_hash_table_lookup(self->pending, confirmation_id);

	if (NULL == confirmation)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no change waiting with id %s. It may have "
		            "expired, or already been decided.", confirmation_id);
		return FALSE;
	}

	confirmation->state = VENTURE_CONFIRMATION_STATE_REJECTED;

	/* The refusal is audited too: knowing what was proposed and denied is
	 * as useful as knowing what was done. */
	entry = venture_audit_entry_new_for_change(VENTURE_AUDIT_ACTION_REJECT,
		confirmation->origin_kind, confirmation->origin,
		confirmation->staged, confirmation->diff);

	g_object_set(entry,
	             "prompt", confirmation->prompt,
	             "request-id", confirmation->id,
	             "approved-by", approver,
	             "source", (NULL != confirmation->via) ? confirmation->via
	                                                   : "confirmation",
	             NULL);

	venture_database_save(self->database, VENTURE_ENTITY(entry), NULL, NULL);

	g_hash_table_remove(self->pending, confirmation_id);

	return TRUE;
}

gboolean
venture_confirmation_parse_stage_flag(
	const gchar	 *value,
	gboolean	 *out_stage,
	GError		**error
){
	static const gchar *const truths[] = { "1", "true", "yes", "on", NULL };
	static const gchar *const falsehoods[] = { "0", "false", "no", "off",
	                                           NULL };

	g_return_val_if_fail(NULL != out_stage, FALSE);

	*out_stage = FALSE;

	if ((NULL == value) || ('\0' == value[0]))
		return TRUE;

	if (g_strv_contains(truths, value))
	{
		*out_stage = TRUE;
		return TRUE;
	}

	if (g_strv_contains(falsehoods, value))
		return TRUE;

	/*
	 * Refused rather than read as false. A caller who wrote a spelling
	 * this build does not know was trying to hold the write back; taking
	 * the unknown value as "no" applies it instead, and the record is
	 * changed by the very request that asked for a review.
	 */
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "stage=%s is not a value this understands. Use stage=1 to "
	            "propose the change for approval, or leave it out to apply "
	            "it.", value);

	return FALSE;
}
