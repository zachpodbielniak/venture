/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/* Keys are public, but changing a pin is access administration. These rows
 * are owner-only on every local surface and are never federated themselves. */
static const VentureFieldDecl peer_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Operator label for this server"),
	VENTURE_FIELD("origin", "HTTPS origin", "Exact origin, without a trailing slash",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("public-key", "Ed25519 public key", "Base64 raw 32-byte public key; verify out of band",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("active", "Allowed", "Explicitly permit this pinned server",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureFederationPeer, venture_federation_peer, peer_fields)

/* One grant names one record, not a graph traversal. Sharing a project must
 * never silently publish its private expenses, credentials or future fields. */
static const VentureFieldDecl grant_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What is being shared and why"),
	VENTURE_FIELD_REF("peer-id", "Server", "Zero means any authenticated key in global mode",
		"federation_peer", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("collection", "Collection", "Optional shared venture or project label for batch pull",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("record-type", "Record type", "Canonical singular type name",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("record-uuid", "Record UUID", "Stable identity on this server",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("fields", "Readable fields", "Comma-separated wire names; no wildcard",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("write-fields", "Editable fields", "Explicit subset of readable fields; empty is read-only",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Shared", "Revocation takes effect on the next request",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureFederationGrant, venture_federation_grant, grant_fields)

/* A type explicitly declares that its semantics permit federation. Unknown
 * plugin types fail closed until their author makes this one metadata choice. */
void
venture_entity_class_set_federation_access(
	VentureEntityClass *klass,
	gboolean writable
){
	g_type_set_qdata(G_TYPE_FROM_CLASS(klass),
		g_quark_from_static_string("venture-federation-access"),
		GUINT_TO_POINTER(writable ? 2 : 1));
}

guint
venture_entity_type_get_federation_access(GType type)
{
	g_autoptr(GTypeClass) klass = NULL;

	if (!g_type_is_a(type, VENTURE_TYPE_ENTITY))
		return 0;
	klass = g_type_class_ref(type);
	return GPOINTER_TO_UINT(g_type_get_qdata(type,
		g_quark_from_static_string("venture-federation-access")));
}

/* Replicas are protocol snapshots, not counterfeit local accounting rows.
 * UUID references remain scoped to the source; saves cannot fire local
 * invoice, journal, notification or automation rules for remote objects. */
static const VentureFieldDecl replica_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Local label for a shared object"),
	VENTURE_FIELD("source-identity", "Source identity", "Unique peer/type/UUID tuple", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_IMMUTABLE),
	VENTURE_FIELD_REF("peer-id", "Server", "Pinned authoritative server", "federation_peer", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("peer-key", "Source key", "Identity pinned when this replica was imported", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_IMMUTABLE),
	VENTURE_FIELD("record-type", "Record type", "Remote canonical type", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_IMMUTABLE),
	VENTURE_FIELD("record-uuid", "Record UUID", "Remote stable UUID", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_IMMUTABLE),
	VENTURE_FIELD_TEXT("base", "Last remote snapshot", "Three-way merge base"),
	VENTURE_FIELD_TEXT("working", "Local working copy", "Durable offline edits"),
	VENTURE_FIELD_TEXT("conflicts", "Conflicts", "Explicit base/local/remote values; never silently overwritten"),
	VENTURE_FIELD("status", "Sync status", "clean, pending, conflict or unavailable", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureFederationReplica, venture_federation_replica, replica_fields)
