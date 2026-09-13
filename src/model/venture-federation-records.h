/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_FEDERATION_RECORDS_H
#define VENTURE_FEDERATION_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_FEDERATION_PEER (venture_federation_peer_get_type())
VENTURE_DECLARE_ENTITY(VentureFederationPeer, venture_federation_peer, FEDERATION_PEER)
#define VENTURE_TYPE_FEDERATION_GRANT (venture_federation_grant_get_type())
VENTURE_DECLARE_ENTITY(VentureFederationGrant, venture_federation_grant, FEDERATION_GRANT)
/**
 * venture_entity_class_set_federation_access:
 * @klass: the record class, during class initialization
 * @writable: whether ordinary validated updates are safe for this type
 *
 * Opts a type into explicit federation grants. Defaults to inaccessible;
 * subclasses must opt in independently. This does not share any records.
 */
void venture_entity_class_set_federation_access(VentureEntityClass *klass, gboolean writable);
/**
 * venture_entity_type_get_federation_access:
 * @type: a record type
 *
 * Returns: 0 for local-only, 1 for read grants, 2 for read and update grants
 */
guint venture_entity_type_get_federation_access(GType type);
#define VENTURE_TYPE_FEDERATION_REPLICA (venture_federation_replica_get_type())
VENTURE_DECLARE_ENTITY(VentureFederationReplica, venture_federation_replica, FEDERATION_REPLICA)
G_END_DECLS
#endif
