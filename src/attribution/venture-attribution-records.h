/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ATTRIBUTION_RECORDS_H
#define VENTURE_ATTRIBUTION_RECORDS_H
#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_ATTRIBUTION_SITE (venture_attribution_site_get_type())
VENTURE_DECLARE_ENTITY(VentureAttributionSite, venture_attribution_site, ATTRIBUTION_SITE)
#define VENTURE_TYPE_ATTRIBUTION_VISITOR (venture_attribution_visitor_get_type())
VENTURE_DECLARE_ENTITY(VentureAttributionVisitor, venture_attribution_visitor, ATTRIBUTION_VISITOR)
#define VENTURE_TYPE_ATTRIBUTION_TOUCH (venture_attribution_touch_get_type())
VENTURE_DECLARE_ENTITY(VentureAttributionTouch, venture_attribution_touch, ATTRIBUTION_TOUCH)
#define VENTURE_TYPE_ATTRIBUTION_SUBMISSION (venture_attribution_submission_get_type())
VENTURE_DECLARE_ENTITY(VentureAttributionSubmission, venture_attribution_submission, ATTRIBUTION_SUBMISSION)
#define VENTURE_TYPE_ATTRIBUTION_BINDING (venture_attribution_binding_get_type())
VENTURE_DECLARE_ENTITY(VentureAttributionBinding, venture_attribution_binding, ATTRIBUTION_BINDING)
G_END_DECLS
#endif
