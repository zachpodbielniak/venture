/*
 * venture-write-risk.h - Which writes may skip a person's approval
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An agent connected over MCP stages every write for approval by default,
 * or applies all of them with --apply-writes. The middle setting --
 * `--apply-writes=low-risk` -- applies ordinary work directly and still
 * stages anything a mistake in would be expensive or hard to take back.
 * This file is the one place that says which is which, so the stdio
 * server, POST /mcp and anything else that asks agree.
 *
 * High risk, and so still staged:
 *
 * - **any delete**;
 * - **money**: every type the access policy treats as financial (the
 *   finance module and everything that requires it), and every type with
 *   a money field -- an invoice, a payment, a budget, a quote with
 *   amounts, a trade -- derived from the field table, so a new type with
 *   money in it is high risk without anybody remembering to say so;
 * - **people and access**: users, memberships, teams, tokens, sessions,
 *   sign-in and second-factor records, portal access, tenants;
 * - **credentials and wiring**: forges and their rules, webhooks,
 *   integration connections, mail, calendar, telephony and bank
 *   connections, federation, AI grants and configuration, plugin
 *   configuration, backups;
 * - **outward and irreversible**: a sent message, text or quote, a
 *   marketing send, a sequence enrolment, a release, a deployment.
 *
 * Everything else -- tickets, comments, worklogs, links, contacts, notes,
 * knowledge-base articles, dashboards, goals -- is low risk.
 */

#ifndef VENTURE_WRITE_RISK_H
#define VENTURE_WRITE_RISK_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VentureWriteRisk:
 * @VENTURE_WRITE_RISK_LOW: may be applied without approval under
 *   `--apply-writes=low-risk`
 * @VENTURE_WRITE_RISK_HIGH: always staged unless every write applies
 *
 * Since: 0.10.0
 */
typedef enum
{
	VENTURE_WRITE_RISK_LOW = 0,
	VENTURE_WRITE_RISK_HIGH
} VentureWriteRisk;

/**
 * venture_write_risk_for_type:
 * @type: a registered record type
 *
 * The risk of creating or changing a record of @type.
 *
 * Returns: the class
 */
VentureWriteRisk
venture_write_risk_for_type(GType type);

/**
 * venture_write_risk_for_write:
 * @type: a registered record type
 * @method: the HTTP method: `POST`, `PUT`, `PATCH` or `DELETE`
 *
 * The risk of one write: a delete is always high, anything else is the
 * type's.
 *
 * Returns: the class
 */
VentureWriteRisk
venture_write_risk_for_write(GType type, const gchar *method);

/**
 * venture_write_risk_nick:
 * @risk: a class
 *
 * Returns: (transfer none): `low` or `high`, as /api/v1/schema says it
 */
const gchar *
venture_write_risk_nick(VentureWriteRisk risk);

G_END_DECLS

#endif /* VENTURE_WRITE_RISK_H */
