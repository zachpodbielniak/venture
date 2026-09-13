/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_FEDERATION_H
#define VENTURE_FEDERATION_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_federation_install_validators:
 * @database: the local database
 *
 * Installs the peer and grant invariants at the common save boundary.
 */
void venture_federation_install_validators(VentureDatabase *database);
/**
 * venture_federation_identity:
 * @context: the local server
 * @error: return location for an error
 *
 * Issues a short-lived single-use challenge and the public server identity.
 * Disabled or incompletely configured federation refuses the request.
 * Returns: (transfer full) (nullable): the identity and challenge
 */
JsonNode *venture_federation_identity(VentureContext *context, GError **error);
/**
 * venture_federation_sign:
 * @context: the signing server
 * @identity: the destination identity and challenge
 * @operation: a protocol operation
 * @signature: (out): base64 Ed25519 signature
 * @error: return location for an error
 *
 * Signs the complete envelope bytes, binding the destination key and origin.
 * Returns: (transfer full) (nullable): the exact request bytes
 */
GBytes *venture_federation_sign(VentureContext *context, JsonNode *identity,
	JsonNode *operation, gchar **signature, GError **error);
/**
 * venture_federation_receive:
 * @context: the authoritative server
 * @body: exact request bytes
 * @signature: base64 signature
 * @error: return location for an error
 *
 * Verifies identity and one-use challenge, rechecks grants and executes a
 * bounded operation. Remote identities never become local API principals.
 * Returns: (transfer full) (nullable): the authorized result
 */
JsonNode *venture_federation_receive(VentureContext *context, GBytes *body,
	const gchar *signature, GError **error);
/**
 * venture_federation_request:
 * @context: the local server
 * @peer_id: an active, owner-pinned destination
 * @operation: a protocol operation
 * @error: return location for an error
 *
 * Calls an HTTPS peer with certificate validation, pin validation, no redirects
 * or ambient proxy credentials, and bounded responses. Runs asynchronous I/O
 * in the main context; callers must not hold a database transaction.
 * Returns: (transfer full) (nullable): the remote result
 */
JsonNode *venture_federation_request(VentureContext *context, gint64 peer_id,
	JsonNode *operation, GError **error);
/**
 * venture_federation_replica_pull:
 * @context: the local server
 * @peer_id: a pinned source server
 * @type: remote record type
 * @uuid: remote UUID
 * @actor: local operator
 * @error: return location for an error
 *
 * Imports one shared record or merges its latest version into an existing
 * working copy. Never pushes edits during a pull.
 * Returns: (transfer full) (nullable): the persistent replica
 */
VentureEntity *venture_federation_replica_pull(VentureContext *context, gint64 peer_id,
	const gchar *type, const gchar *uuid, const VentureActor *actor, GError **error);
/**
 * venture_federation_replica_edit:
 * @context: the local server
 * @id: replica id
 * @version: expected local replica version
 * @fields: object of field values to change
 * @actor: local operator
 * @error: return location for an error
 *
 * Saves durable offline edits to advertised writable fields. Changing a field
 * also explicitly resolves its recorded conflict in favor of the new value.
 * Returns: (transfer full) (nullable): the updated replica
 */
VentureEntity *venture_federation_replica_edit(VentureContext *context, gint64 id,
	gint64 version, JsonNode *fields, const VentureActor *actor, GError **error);
/**
 * venture_federation_replica_sync:
 * @context: the local server
 * @id: replica id
 * @actor: local operator
 * @error: return location for an error
 *
 * Fetches, three-way merges, then sends conflict-free changes with an expected
 * remote version. A failure preserves working data for a safe retry.
 * Returns: (transfer full) (nullable): the replica, including unresolved conflicts
 */
VentureEntity *venture_federation_replica_sync(VentureContext *context, gint64 id,
	const VentureActor *actor, GError **error);
/**
 * venture_federation_sign_response:
 * @context: signing server
 * @body: exact response bytes
 * @binding: client nonce or SHA256 request digest
 * @error: return location for an error
 * Returns: (transfer full) (nullable): base64 domain-separated Ed25519 signature
 */
gchar *venture_federation_sign_response(VentureContext *context, GBytes *body,
	const gchar *binding, GError **error);
/**
 * venture_federation_verify_response:
 * @public_key: owner-pinned raw base64 Ed25519 key
 * @body: exact response bytes
 * @binding: client nonce or SHA256 request digest
 * @signature: response signature
 * @error: return location for an error
 * Returns: whether the response proves possession of the pinned key
 */
gboolean venture_federation_verify_response(const gchar *public_key, GBytes *body,
	const gchar *binding, const gchar *signature, GError **error);
/**
 * venture_federation_replica_resolve:
 * @context: local server
 * @id: replica id
 * @version: expected local version
 * @field: conflicted field
 * @keep_local: keep the local value, otherwise accept the remote value or absence
 * @actor: local operator
 * @error: return location for an error
 * Returns: (transfer full) (nullable): the updated replica
 */
VentureEntity *venture_federation_replica_resolve(VentureContext *context, gint64 id,
	gint64 version, const gchar *field, gboolean keep_local, const VentureActor *actor, GError **error);
/**
 * venture_federation_collection_pull:
 * @context: local server
 * @peer_id: pinned source
 * @collection: explicit grant collection label
 * @offset: page offset, initially zero
 * @actor: local operator
 * @error: return location for an error
 *
 * Pulls at most ten explicitly shared records. Reports each durable result
 * independently so a failed record never hides successful imports.
 * Returns: (transfer full) (nullable): replica outcomes, next_offset and more
 */
JsonNode *venture_federation_collection_pull(VentureContext *context, gint64 peer_id,
	const gchar *collection, gint64 offset, const VentureActor *actor, GError **error);
/**
 * venture_federation_sync_start:
 * @context: local server
 * Starts opt-in bounded reconnect synchronization on the main context.
 */
void venture_federation_sync_start(VentureContext *context);
/**
 * venture_federation_sync_stop:
 * @context: local server
 * Stops scheduling reconnect work; in-flight operations retain their context.
 */
void venture_federation_sync_stop(VentureContext *context);
G_END_DECLS
#endif
