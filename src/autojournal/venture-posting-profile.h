/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_POSTING_PROFILE_H
#define VENTURE_POSTING_PROFILE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_POSTING_PROFILE (venture_posting_profile_get_type())
VENTURE_DECLARE_ENTITY(VenturePostingProfile, venture_posting_profile, POSTING_PROFILE)
G_END_DECLS
#endif
