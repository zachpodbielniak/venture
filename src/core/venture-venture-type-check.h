/*
 * venture-venture-type-check.h - Declarative venture types, held at the save
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A venture type's YAML promises `required`, `choices`, `min` and `max` for
 * the fields it adds to a venture's attribute bag. The promise is kept here,
 * as a `venture` save validator, so every writer -- the form, the API, an
 * approved staged change, the assistant, the CLI -- is held to it.
 */

#ifndef VENTURE_VENTURE_TYPE_CHECK_H
#define VENTURE_VENTURE_TYPE_CHECK_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * venture_venture_type_check_install:
 * @context: the wiring
 *
 * Installs the `venture` save validator that holds a venture to its
 * declared type, and points it at @context's venture-type registry.
 *
 * The validator is added once per database; every later context over the
 * same database only re-points it at its own registry, so the registry the
 * plugin manager filled is the one consulted.
 *
 * What it checks, and when:
 *
 * - a venture created with, or moved to, a type is checked against every
 *   field that type declares;
 * - a venture keeping its type has only the declared attributes this save
 *   changes checked, so an attribute stored before a rule existed does not
 *   block an unrelated edit;
 * - a type written that is not registered is refused, naming the ones
 *   that are -- unless the registry is empty, as it is with plugins off,
 *   where there is nothing to judge against. A venture keeping an
 *   unregistered type it already had stays editable.
 */
void
venture_venture_type_check_install(VentureContext *context);

G_END_DECLS

#endif /* VENTURE_VENTURE_TYPE_CHECK_H */
