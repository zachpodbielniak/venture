/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Owner invitations: the owner of a business invites someone to it and the
 * invitation goes out in the business's own mail. It is the workspace
 * invitation (one capability, only its hash kept, accepted at
 * /account/invitation into the same accounts and memberships), issued under
 * the owner's authority instead of a workspace administrator's:
 *
 * - the authority is an explicit owner membership of that one business, now
 *   and in the credential; acceptance re-checks it against the version the
 *   invitation pinned, so a demoted or removed owner's invitations die;
 * - the tenant role is always member and the business role never owner, so
 *   an owner can hand out no more than they hold, and never workspace
 *   administration;
 * - the message is queued in the business's own outbox, which sends through
 *   the business's own mail account and never the installation's;
 * - a retry with the same key answers the first invitation and queues no
 *   second message; a resend rotates the capability, so only the newest
 *   link works; a revocation kills the capability whatever the mail did.
 */

#define OWNER_INVITATION_SECONDS 604800
#define OWNER_INVITATION_LIST_MAX 200

static const gchar *const owner_invitation_roles[] = { "admin", "editor", "accountant", "viewer" };

static gboolean
owner_invitation_refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "Invitation: %s", message);
	return FALSE;
}

static gint
owner_invitation_role(const gchar *role)
{
	gint value = -1;
	guint i;
	for (i = 0; i < G_N_ELEMENTS(owner_invitation_roles); i++)
		if (g_strcmp0(role, owner_invitation_roles[i]) == 0 &&
		    venture_enum_from_nick(VENTURE_TYPE_ORGANIZATION_ROLE, role, &value)) return value;
	return -1;
}

/* The version of @user_id's active owner membership of @organization_id, or
 * -1: what an owner invitation pins and acceptance compares. */
static gint64
owner_membership_version(VentureTenantService *self, gint64 user_id, gint64 organization_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user_id, NULL);
	rows = venture_database_find(self->database, query, error);
	if (!rows) return -1;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		gboolean active = FALSE;
		gint role = -1;
		g_object_get(row, "active", &active, "role", &role, NULL);
		if (active && !venture_entity_is_deleted(row) && role == VENTURE_ORGANIZATION_ROLE_OWNER)
			return venture_entity_get_version(row);
	}
	return -1;
}

static gchar *
owner_invitation_hash(gint64 organization_id, const gchar *email, const gchar *name, const gchar *role)
{
	g_autofree gchar *business = g_strdup_printf("%" G_GINT64_FORMAT, organization_id);
	g_autofree gchar *folded = g_ascii_strdown(email, -1);
	const gchar *fields[4];
	fields[0] = business; fields[1] = folded; fields[2] = name ? name : ""; fields[3] = role;
	return signup_request_hash(fields, G_N_ELEMENTS(fields));
}

/* The invitation mail, from the business to the invitee. The link is in the
 * private body only, with the capability after '#', which a browser never
 * sends to a server: it stays out of request logs and referrers, and the
 * acceptance page reads it into its form. Nothing here names the platform:
 * the business is the sender. */
static VentureMailMessage *
owner_invitation_message(VentureTenantService *self, VentureEntity *invitation, const gchar *business,
	const gchar *inviter, const gchar *capability, gint64 sends)
{
	VentureMailMessage *message = venture_mail_message_new();
	g_autoptr(GDateTime) expires = NULL;
	g_autofree gchar *email = NULL, *name = NULL, *when = NULL, *subject = NULL, *text = NULL;
	g_autofree gchar *body = NULL, *key = NULL, *greeting = NULL;
	g_object_get(invitation, "email", &email, "invitee-name", &name, "expires-at", &expires, NULL);
	when = venture_time_to_date_string(expires, NULL);
	greeting = venture_string_is_empty(name) ? g_strdup("Hello,") : g_strdup_printf("Hello %s,", name);
	subject = g_strdup_printf("%s invited you to join %s", inviter, business);
	text = g_strdup_printf("%s invited you to join %s.", inviter, business);
	body = g_strdup_printf("%s\n\n%s invited you to join %s.\n\nAccept the invitation here:\n%s/account/invitation#code=%s\n\n"
		"The link works once and expires on %s. Keep it private: whoever opens it can join in your place.\n"
		"If you were not expecting this, ignore this message.\n\n%s\n",
		greeting, inviter, business, self->origin, capability, when, business);
	key = g_strdup_printf("owner-invitation:%s:%" G_GINT64_FORMAT, venture_entity_get_uuid(invitation), sends);
	g_object_set(message, "to", email, "subject", subject,
		"text-body", text, "private-text-body", body, "idempotency-key", key,
		"related-type", "tenant_invitation", "related-id", venture_entity_get_id(invitation), NULL);
	return message;
}

/* What the owner sees of one invitation. Never the capability or its hash. */
static JsonObject *
owner_invitation_object(VentureTenantService *self, VentureEntity *invitation, GError **error)
{
	g_autoptr(GDateTime) expires = NULL, consumed = NULL, revoked = NULL, now = g_date_time_new_now_utc();
	g_autoptr(VentureIntegrationConnection) mailer = NULL;
	g_autoptr(GError) unconfigured = NULL;
	g_autofree gchar *email = NULL, *name = NULL, *role = NULL, *delivery = NULL;
	JsonObject *object, *mail;
	gint64 organization = 0, accepted = 0, message_id = 0, sends = 0;
	const gchar *state;
	g_object_get(invitation, "email", &email, "invitee-name", &name, "expires-at", &expires,
		"consumed-at", &consumed, "consumed-user-id", &accepted, "revoked-at", &revoked,
		"initial-organization-id", &organization, "mail-message-id", &message_id, "sent-count", &sends, NULL);
	role = record_enum_nick(invitation, "organization-role");
	state = consumed ? "accepted" : revoked ? "revoked" : (!expires || g_date_time_compare(expires, now) <= 0) ? "expired" : "pending";
	if (message_id > 0) {
		g_autoptr(GError) local = NULL;
		g_autoptr(VentureEntity) row = venture_database_get(self->database, VENTURE_TYPE_MAIL_MESSAGE, message_id, &local);
		if (local && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return NULL;
		}
		if (row) g_object_get(row, "state", &delivery, NULL);
	}
	mailer = venture_integration_service_find(venture_integration_service_get(self->database), organization, "smtp", &unconfigured);
	object = json_object_new();
	json_object_set_int_member(object, "invitation_id", venture_entity_get_id(invitation));
	json_object_set_int_member(object, "organization_id", organization);
	json_object_set_string_member(object, "email", email ? email : "");
	if (venture_string_is_empty(name)) json_object_set_null_member(object, "name");
	else json_object_set_string_member(object, "name", name);
	json_object_set_string_member(object, "role", role ? role : "");
	json_object_set_string_member(object, "state", state);
	if (expires) {
		g_autofree gchar *text = venture_time_to_string(expires);
		json_object_set_string_member(object, "expires_at", text);
	} else json_object_set_null_member(object, "expires_at");
	if (accepted > 0) json_object_set_int_member(object, "accepted_user_id", accepted);
	else json_object_set_null_member(object, "accepted_user_id");
	mail = json_object_new();
	/* The outbox's own state: queued, sending, sent (the relay accepted it,
	 * not that it reached an inbox), failed, uncertain, dead or cancelled. */
	json_object_set_string_member(mail, "state", delivery ? delivery : "none");
	json_object_set_int_member(mail, "sends", sends);
	json_object_set_boolean_member(mail, "mail_ready", mailer != NULL);
	json_object_set_object_member(object, "delivery", mail);
	return object;
}

static JsonNode *
owner_invitation_node(VentureTenantService *self, VentureEntity *invitation, GError **error)
{
	JsonObject *object = owner_invitation_object(self, invitation, error);
	JsonNode *node;
	if (!object) return NULL;
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, object);
	return node;
}

/* The owner's authority, the business's name and the inviter's name; every
 * operation starts here, before anything is read for the caller. */
static gboolean
owner_invitation_prepare(VentureTenantService *self, gint64 organization_id, gint64 *actor_id,
	gchar **business, gchar **inviter, VentureActor *audit, GError **error)
{
	const VentureAuthPrincipal *actor;
	g_autoptr(VentureEntity) organization = NULL, user = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autofree gchar *display = NULL, *username = NULL;
	if (!venture_tenant_service_check_organization_owner(self, organization_id, NULL, error)) return FALSE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") == G_TYPE_INVALID)
		return owner_invitation_refuse(error, VENTURE_ERROR_CONFIG, "the business's mail is switched off, so no invitation can be sent");
	actor = venture_access_policy_get_actor(venture_database_get_access_policy(self->database));
	*actor_id = actor->user_id;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
	user = organization ? venture_database_get(self->database, VENTURE_TYPE_USER, actor->user_id, error) : NULL;
	if (!user) return FALSE;
	if (business) g_object_get(organization, "name", business, NULL);
	g_object_get(user, "display-name", &display, "username", &username, NULL);
	if (inviter) *inviter = g_strdup(!venture_string_is_empty(display) ? display : username);
	audit->kind = VENTURE_ACTOR_KIND_IMPORT;
	audit->name = actor->name;
	audit->prompt = NULL;
	audit->request_id = NULL;
	audit->approved_by = NULL;
	return TRUE;
}

/* An owner invitation of @organization_id by id, or NOT_FOUND: another
 * business's invitation, and an administrator's, are not the owner's. */
static VentureEntity *
owner_invitation_find(VentureTenantService *self, gint64 organization_id, gint64 invitation_id, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(VentureEntity) invitation = invitation_id > 0 ?
		venture_database_get(self->database, VENTURE_TYPE_TENANT_INVITATION, invitation_id, &local) : NULL;
	gint64 issuer_organization = 0;
	if (local && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (invitation) g_object_get(invitation, "issuer-organization-id", &issuer_organization, NULL);
	if (!invitation || venture_entity_is_deleted(invitation) || issuer_organization != organization_id) {
		owner_invitation_refuse(error, VENTURE_ERROR_NOT_FOUND, "no such invitation for this business");
		return NULL;
	}
	return g_steal_pointer(&invitation);
}

/* A new capability, a fresh expiry and one more queued message. The old
 * capability's hash is replaced, so its link stops working at once. */
static gboolean
owner_invitation_send(VentureTenantService *self, VentureEntity *invitation, const gchar *business,
	const gchar *inviter, const gchar *send_key, const VentureActor *audit, GError **error)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc(), expires = g_date_time_add_seconds(now, OWNER_INVITATION_SECONDS);
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	g_autofree gchar *capability = venture_generate_token(32);
	g_autofree gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, capability, -1);
	gint64 sends = 0, organization = 0;
	g_object_get(invitation, "sent-count", &sends, "initial-organization-id", &organization, NULL);
	g_object_set(invitation, "token-hash", hash, "expires-at", expires, "sent-count", sends + 1, "send-key", send_key, NULL);
	if (!venture_database_save(self->database, invitation, NULL, error)) return FALSE;
	message = owner_invitation_message(self, invitation, business, inviter, capability, sends + 1);
	g_object_set(message, "organization-id", organization, NULL);
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(self->database), message, audit, error);
	if (!queued) return FALSE;
	g_object_set(invitation, "mail-message-id", venture_entity_get_id(VENTURE_ENTITY(queued)), NULL);
	return venture_database_save(self->database, invitation, NULL, error);
}

static VentureEntity *
owner_invitation_by_key(VentureTenantService *self, gint64 organization_id, const gchar *key, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TENANT_INVITATION);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "issuer-organization-id", VENTURE_FILTER_OP_EQ, organization_id, NULL);
	venture_query_add_filter_string(query, "idempotency-key", VENTURE_FILTER_OP_EQ, key, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(self->database, query, error);
	return rows && rows->len ? g_object_ref(g_ptr_array_index(rows, 0)) : NULL;
}

/* A pending invitation of this business for the same address: a second one
 * would be a second mail and a second live link for one person. */
static gboolean
owner_invitation_pending_for(VentureTenantService *self, gint64 organization_id, const gchar *email, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TENANT_INVITATION);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	guint i;
	venture_query_add_filter_int(query, "issuer-organization-id", VENTURE_FILTER_OP_EQ, organization_id, NULL);
	rows = venture_database_find(self->database, query, error);
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) expires = NULL, consumed = NULL, revoked = NULL;
		g_autofree gchar *address = NULL;
		g_object_get(row, "email", &address, "expires-at", &expires, "consumed-at", &consumed, "revoked-at", &revoked, NULL);
		if (!consumed && !revoked && expires && g_date_time_compare(expires, now) > 0 &&
		    address && g_ascii_strcasecmp(address, email) == 0) return TRUE;
	}
	return FALSE;
}

JsonNode *
venture_tenant_service_owner_invite(VentureTenantService *self, gint64 organization_id, const gchar *email,
	const gchar *name, const gchar *role, const gchar *idempotency_key, gboolean *created, GError **error)
{
	VentureActor audit;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) invitation = NULL, membership = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *business = NULL, *inviter = NULL, *hash = NULL, *stored = NULL;
	gint64 actor_id = 0, owner_version;
	gint organization_role = owner_invitation_role(role);
	gboolean committed = FALSE;
	g_return_val_if_fail(VENTURE_IS_TENANT_SERVICE(self), NULL);
	if (created) *created = FALSE;
	if (!owner_invitation_prepare(self, organization_id, &actor_id, &business, &inviter, &audit, error)) return NULL;
	if (!signup_key_valid(idempotency_key)) {
		owner_invitation_refuse(error, VENTURE_ERROR_VALIDATION, "idempotency_key must be 1-128 of A-Z a-z 0-9 . _ : -");
		return NULL;
	}
	if (!signup_email_valid(email)) {
		owner_invitation_refuse(error, VENTURE_ERROR_VALIDATION, "email must be a plain address");
		return NULL;
	}
	if (name && !signup_text_valid(name, 200)) {
		owner_invitation_refuse(error, VENTURE_ERROR_VALIDATION, "name must be 1-200 characters without control characters");
		return NULL;
	}
	if (organization_role < 0) {
		owner_invitation_refuse(error, VENTURE_ERROR_VALIDATION, "role must be admin, editor, accountant or viewer");
		return NULL;
	}
	hash = owner_invitation_hash(organization_id, email, name, role);
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (!venture_database_begin(self->database, error)) goto done;
	if (!lock_administration(self, error)) goto rollback;
	invitation = owner_invitation_by_key(self, organization_id, idempotency_key, error);
	if (error && *error) goto rollback;
	if (invitation) {
		/* The first answer, as it stands now; nothing is sent again. */
		g_object_get(invitation, "request-hash", &stored, NULL);
		if (g_strcmp0(stored, hash) != 0) {
			owner_invitation_refuse(error, VENTURE_ERROR_CONFLICT, "idempotency_key was already used for a different invitation");
			goto rollback;
		}
		result = owner_invitation_node(self, invitation, error);
		goto rollback;
	}
	if (owner_invitation_pending_for(self, organization_id, email, error)) {
		owner_invitation_refuse(error, VENTURE_ERROR_CONFLICT, "that address already has a pending invitation; resend it instead");
		goto rollback;
	}
	if (error && *error) goto rollback;
	/* Re-judged inside the lock, and pinned: acceptance compares it. */
	owner_version = owner_membership_version(self, actor_id, organization_id, error);
	membership = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", actor_id, error);
	if (owner_version < 0 || !membership) {
		if (!error || !*error) tenant_fail(error, "owner authority changed");
		goto rollback;
	}
	{
		g_autoptr(VentureEntity) user = venture_database_get(self->database, VENTURE_TYPE_USER, actor_id, error);
		g_autoptr(GDateTime) generation = NULL;
		g_autofree gchar *placeholder = venture_generate_token(32);
		g_autofree gchar *placeholder_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, placeholder, -1);
		g_autoptr(GDateTime) now = g_date_time_new_now_utc();
		if (!user) goto rollback;
		g_object_get(user, "sessions-invalidated-at", &generation, NULL);
		invitation = g_object_new(VENTURE_TYPE_TENANT_INVITATION, "token-hash", placeholder_hash,
			"role", VENTURE_TENANT_ROLE_MEMBER, "initial-organization-id", organization_id,
			"organization-role", organization_role, "issuer-user-id", actor_id,
			"issuer-session-generation", generation, "issuer-membership-version", venture_entity_get_version(membership),
			"issuer-organization-id", organization_id, "issuer-owner-version", owner_version,
			"email", email, "invitee-name", name, "idempotency-key", idempotency_key, "request-hash", hash,
			"expires-at", now, NULL);
	}
	if (!owner_invitation_send(self, invitation, business, inviter, idempotency_key, &audit, error) ||
	    !record_event(self, "tenant.owner_invite", actor_id, 0, role, "Owner invitation", error)) goto rollback;
	result = owner_invitation_node(self, invitation, error);
	if (!result) goto rollback;
	committed = venture_database_commit(self->database, error);
	if (committed && created) *created = TRUE;
	if (!committed) g_clear_pointer(&result, json_node_unref);
	goto done;
rollback:
	venture_database_rollback(self->database);
done:
	self->maintenance_depth--;
	return g_steal_pointer(&result);
}

JsonNode *
venture_tenant_service_owner_invitations(VentureTenantService *self, gint64 organization_id, GError **error)
{
	VentureActor audit;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	JsonArray *list;
	JsonObject *object;
	JsonNode *node;
	gint64 actor_id = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_TENANT_SERVICE(self), NULL);
	if (!owner_invitation_prepare(self, organization_id, &actor_id, NULL, NULL, &audit, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	query = venture_query_new(VENTURE_TYPE_TENANT_INVITATION);
	venture_query_add_filter_int(query, "issuer-organization-id", VENTURE_FILTER_OP_EQ, organization_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	/* One past the bound, refused rather than cut short. */
	venture_query_set_limit(query, OWNER_INVITATION_LIST_MAX + 1);
	rows = venture_database_find(self->database, query, error);
	if (!rows) return NULL;
	if (rows->len > OWNER_INVITATION_LIST_MAX) {
		owner_invitation_refuse(error, VENTURE_ERROR_VALIDATION, "more invitations than one list answers");
		return NULL;
	}
	list = json_array_new();
	for (i = 0; i < rows->len; i++) {
		JsonObject *entry = owner_invitation_object(self, g_ptr_array_index(rows, i), error);
		if (!entry) { json_array_unref(list); return NULL; }
		json_array_add_object_element(list, entry);
	}
	object = json_object_new();
	json_object_set_int_member(object, "organization_id", organization_id);
	json_object_set_array_member(object, "invitations", list);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, object);
	return node;
}

JsonNode *
venture_tenant_service_owner_resend(VentureTenantService *self, gint64 organization_id, gint64 invitation_id,
	const gchar *idempotency_key, gboolean *sent, GError **error)
{
	VentureActor audit;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) invitation = NULL;
	g_autoptr(GDateTime) consumed = NULL, revoked = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *business = NULL, *inviter = NULL, *last = NULL;
	gint64 actor_id = 0;
	gboolean committed = FALSE;
	g_return_val_if_fail(VENTURE_IS_TENANT_SERVICE(self), NULL);
	if (sent) *sent = FALSE;
	if (!owner_invitation_prepare(self, organization_id, &actor_id, &business, &inviter, &audit, error)) return NULL;
	if (!signup_key_valid(idempotency_key)) {
		owner_invitation_refuse(error, VENTURE_ERROR_VALIDATION, "idempotency_key must be 1-128 of A-Z a-z 0-9 . _ : -");
		return NULL;
	}
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (!venture_database_begin(self->database, error)) goto done;
	if (!lock_administration(self, error)) goto rollback;
	invitation = owner_invitation_find(self, organization_id, invitation_id, error);
	if (!invitation) goto rollback;
	g_object_get(invitation, "consumed-at", &consumed, "revoked-at", &revoked, "send-key", &last, NULL);
	if (g_strcmp0(last, idempotency_key) == 0) {
		/* This resend already happened: answer it, send nothing more. */
		result = owner_invitation_node(self, invitation, error);
		goto rollback;
	}
	if (consumed || revoked) {
		owner_invitation_refuse(error, VENTURE_ERROR_CONFLICT, consumed ?
			"the invitation was already accepted" : "the invitation was revoked; invite them again");
		goto rollback;
	}
	{
		/* A resend is a new link under the authority of whoever resends it,
		 * pinned again as a new invitation would be. */
		g_autoptr(VentureEntity) membership = find_number(self, VENTURE_TYPE_TENANT_MEMBERSHIP, "user-id", actor_id, error);
		g_autoptr(VentureEntity) user = membership ? venture_database_get(self->database, VENTURE_TYPE_USER, actor_id, error) : NULL;
		g_autoptr(GDateTime) generation = NULL;
		gint64 owner_version = owner_membership_version(self, actor_id, organization_id, error);
		if (owner_version < 0 || !user) {
			if (!error || !*error) tenant_fail(error, "owner authority changed");
			goto rollback;
		}
		g_object_get(user, "sessions-invalidated-at", &generation, NULL);
		g_object_set(invitation, "issuer-user-id", actor_id, "issuer-owner-version", owner_version,
			"issuer-membership-version", venture_entity_get_version(membership),
			"issuer-session-generation", generation, NULL);
	}
	if (!owner_invitation_send(self, invitation, business, inviter, idempotency_key, &audit, error) ||
	    !record_event(self, "tenant.owner_invite_resend", actor_id, 0, "invitation", "Owner invitation resent", error)) goto rollback;
	result = owner_invitation_node(self, invitation, error);
	if (!result) goto rollback;
	committed = venture_database_commit(self->database, error);
	if (committed && sent) *sent = TRUE;
	if (!committed) g_clear_pointer(&result, json_node_unref);
	goto done;
rollback:
	venture_database_rollback(self->database);
done:
	self->maintenance_depth--;
	return g_steal_pointer(&result);
}

JsonNode *
venture_tenant_service_owner_revoke(VentureTenantService *self, gint64 organization_id, gint64 invitation_id, GError **error)
{
	VentureActor audit;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) invitation = NULL;
	g_autoptr(GDateTime) consumed = NULL, revoked = NULL, now = g_date_time_new_now_utc();
	g_autoptr(JsonNode) result = NULL;
	gint64 actor_id = 0, message_id = 0;
	gboolean committed = FALSE;
	g_return_val_if_fail(VENTURE_IS_TENANT_SERVICE(self), NULL);
	if (!owner_invitation_prepare(self, organization_id, &actor_id, NULL, NULL, &audit, error)) return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	self->maintenance_depth++;
	if (!venture_database_begin(self->database, error)) goto done;
	if (!lock_administration(self, error)) goto rollback;
	invitation = owner_invitation_find(self, organization_id, invitation_id, error);
	if (!invitation) goto rollback;
	g_object_get(invitation, "consumed-at", &consumed, "revoked-at", &revoked, "mail-message-id", &message_id, NULL);
	if (consumed) {
		owner_invitation_refuse(error, VENTURE_ERROR_CONFLICT, "the invitation was already accepted; change their access instead");
		goto rollback;
	}
	if (!revoked) {
		g_object_set(invitation, "revoked-at", now, NULL);
		if (!venture_database_save(self->database, invitation, NULL, error) ||
		    !record_event(self, "tenant.owner_invite_revoke", actor_id, 0, "invitation", "Owner invitation revoked", error)) goto rollback;
		/* Mail not yet handed to the relay is held back; mail already sent
		 * carries a link that no longer works, which is what revoking means. */
		if (message_id > 0) {
			g_autoptr(GError) in_flight = NULL;
			if (!venture_mail_outbox_cancel(venture_database_get_mail_outbox(self->database), organization_id, message_id,
			        "Invitation revoked", &audit, &in_flight) &&
			    !g_error_matches(in_flight, VENTURE_ERROR, VENTURE_ERROR_VALIDATION) &&
			    !g_error_matches(in_flight, VENTURE_ERROR, VENTURE_ERROR_CONFLICT)) {
				g_propagate_error(error, g_steal_pointer(&in_flight));
				goto rollback;
			}
		}
	}
	result = owner_invitation_node(self, invitation, error);
	if (!result) goto rollback;
	committed = venture_database_commit(self->database, error);
	if (!committed) g_clear_pointer(&result, json_node_unref);
	goto done;
rollback:
	venture_database_rollback(self->database);
done:
	self->maintenance_depth--;
	return g_steal_pointer(&result);
}
