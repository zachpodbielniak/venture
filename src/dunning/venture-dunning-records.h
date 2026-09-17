/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DUNNING_RECORDS_H
#define VENTURE_DUNNING_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_DUNNING_POLICY (venture_dunning_policy_get_type())
VENTURE_DECLARE_ENTITY(VentureDunningPolicy, venture_dunning_policy, DUNNING_POLICY)
#define VENTURE_TYPE_DUNNING_EVENT (venture_dunning_event_get_type())
VENTURE_DECLARE_ENTITY(VentureDunningEvent, venture_dunning_event, DUNNING_EVENT)
G_END_DECLS
#endif
