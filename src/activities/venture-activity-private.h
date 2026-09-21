/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACTIVITY_PRIVATE_H
#define VENTURE_ACTIVITY_PRIVATE_H
/* Database initialization installs the canonical validator once. */
VentureActivityService *venture_activity_service_new(VentureDatabase *database);
/* Both the action surfaces and the direct service validate this field-derived schema. */
GPtrArray *venture_activity_call_parameters(void);

#endif
