/*
 * venture.h - Umbrella header for VENTURE
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This is the only header a consumer should include:
 *
 *   #include <venture/venture.h>
 *
 * Every other header refuses to be included on its own, which keeps the
 * public surface a single, versioned entry point.
 *
 * The header serves two audiences with slightly different needs:
 *
 *   - venturectl and anything else linking libventure-core.a sees the type
 *     system, the boxed types, the interfaces, the domain model, the config
 *     loader and the utilities. That is enough to build, validate and
 *     serialise records without a database.
 *
 *   - the server, its plugins and its podomation modules additionally see
 *     the database, reporting, AI, automation, plugin and web subsystems.
 *     Those are gated behind VENTURE_SERVER_BUILD, which the build system
 *     defines for the server object tree and for anything compiled against
 *     it.
 */

#ifndef VENTURE_H
#define VENTURE_H

/*
 * VENTURE_INSIDE tells the individual headers that they are being pulled in
 * through the umbrella rather than directly. It is undefined again at the
 * bottom so a later direct include still trips the guard.
 */
#define VENTURE_INSIDE

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

/* --- Foundation ---------------------------------------------------------- */

#include "venture-version.h"
#include "venture-types.h"
#include "venture-enums.h"
#include "venture-error.h"

/* --- Boxed value types --------------------------------------------------- */

#include "boxed/venture-money.h"
#include "boxed/venture-date-range.h"
#include "boxed/venture-metric.h"
#include "boxed/venture-field-spec.h"

/* --- Interfaces ---------------------------------------------------------- */

#include "interfaces/venture-serializable.h"

/* --- Utilities ----------------------------------------------------------- */

#include "util/venture-string-util.h"
#include "util/venture-time-util.h"
#include "util/venture-json-util.h"

/* --- Domain model -------------------------------------------------------- */

#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
#include "model/venture-records.h"
#include "model/venture-federation-records.h"
#include "ledger/venture-journal.h"
#include "receivables/venture-receivable-records.h"
#include "receivables/venture-invoice-state-machine.h"
#include "model/venture-venture-type.h"
#include "model/venture-entity-registry.h"
#include "model/venture-module.h"
#include "periods/venture-period-records.h"
#include "mail/venture-mail-records.h"

#include "autojournal/venture-posting-profile.h"

/* --- Configuration ------------------------------------------------------- */

#include "config/venture-config.h"

/* --- MCP ----------------------------------------------------------------- */

/*
 * `venturectl mcp` serves the REST API to an AI agent, so this lives on the
 * core side rather than behind VENTURE_SERVER_BUILD: the subcommand has to
 * build on a machine that cannot build the server at all, and it needs
 * nothing the CLI does not already link.
 */

#include "mcp/venture-mcp-catalog.h"
#include "mcp/venture-mcp-server.h"

/* --- Server-only subsystems ---------------------------------------------- */

#ifdef VENTURE_SERVER_BUILD

#include "plugin/venture-crispy-host.h"

/* The database comes before the plugin manager, whose configuration API
 * names VentureActor in its signatures. */
#include "db/venture-schema.h"
#include "db/venture-query.h"
#include "db/venture-database.h"
#include "periods/venture-period-constraints.h"
#include "periods/venture-period-check.h"
#include "periods/venture-period-service.h"
#include "periods/venture-period-guard.h"

#include "plugin/venture-plugin-manager.h"

#include "automation/venture-automation.h"

#include "report/venture-report.h"
#include "receivables/venture-settlement-service.h"
#include "mail/venture-mailer.h"
#include "mail/venture-mailer-registry.h"
#include "mail/venture-smtp-mailer.h"
#include "mail/venture-mail-outbox.h"
#include "mail/venture-mail-template.h"
#include "periods/venture-period-report.h"

/* The confirmation store comes first: the context owns one and names its
 * type in an accessor. */
#include "core/venture-confirmation-store.h"

#include "core/venture-context.h"
#include "mail/venture-mail-consumers.h"
#include "core/venture-federation.h"
#include "ledger/venture-posting-rule.h"
#include "ledger/venture-posting-service.h"
#include "autojournal/venture-autojournal-service.h"
#include "statements/venture-ledger-balances.h"
#include "core/venture-ticket-relation.h"
#include "core/venture-record-link.h"
#include "core/venture-dashboard.h"
#include "core/venture-factory.h"
#include "core/venture-notify.h"
#include "core/venture-sla.h"
#include "core/venture-desk.h"
#include "core/venture-webhook.h"
#include "core/venture-routing.h"
#include "core/venture-action.h"
#include "ledger/venture-journal-actions.h"

/* Auth comes before the AI service, which names VentureAuthPrincipal in its
 * signatures, and before the web server, which uses both. */
#include "web/venture-auth.h"

#include "ai/venture-ai-service.h"
#include "ai/venture-ai-assist.h"
#include "ai/venture-ai-skills.h"
#include "ai/venture-ai-models.h"
#include "ai/venture-ai-harness.h"

/* --- Forge integration --------------------------------------------------- */

#include "kb/venture-embedding.h"
#include "kb/venture-kb-chunk.h"
#include "kb/venture-kb-service.h"
#include "kb/venture-kb-ingest.h"
#include "kb/venture-kb-crossref.h"
#include "forge/venture-forge-client.h"
#include "forge/venture-forgejo-client.h"
#include "forge/venture-forge-rules.h"
#include "forge/venture-work-tools.h"
#include "forge/venture-work-service.h"

#include "web/venture-web-server.h"

#endif /* VENTURE_SERVER_BUILD */

#undef VENTURE_INSIDE

#endif /* VENTURE_H */
