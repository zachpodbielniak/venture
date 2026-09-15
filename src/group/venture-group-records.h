/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_GROUP_RECORDS_H
#define VENTURE_GROUP_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_INTERCOMPANY_LINK (venture_intercompany_link_get_type())
VENTURE_DECLARE_ENTITY(VentureIntercompanyLink, venture_intercompany_link, INTERCOMPANY_LINK)
#define VENTURE_TYPE_ELIMINATION (venture_elimination_get_type())
VENTURE_DECLARE_ENTITY(VentureElimination, venture_elimination, ELIMINATION)
G_END_DECLS
#endif
