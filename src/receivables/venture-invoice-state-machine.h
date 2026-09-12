/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_INVOICE_STATE_MACHINE_H
#define VENTURE_INVOICE_STATE_MACHINE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS
#define VENTURE_TYPE_INVOICE_STATE_MACHINE (venture_invoice_state_machine_get_type())
G_DECLARE_DERIVABLE_TYPE(VentureInvoiceStateMachine, venture_invoice_state_machine,
	VENTURE, INVOICE_STATE_MACHINE, GObject)

/**
 * VentureInvoiceStateMachineClass:
 * @parent_class: the parent
 * @transition: default handler, run after ordinary signal handlers
 *
 * A FALSE result vetoes the transition and stops signal emission.
 */
struct _VentureInvoiceStateMachineClass
{
	GObjectClass parent_class;
	gboolean (*transition)(VentureInvoiceStateMachine *self, VentureInvoice *invoice,
		const gchar *from, const gchar *to);
	gpointer padding[8];
};

/**
 * venture_invoice_state_machine_new:
 * Returns: (transfer full): the built-in invoice lifecycle
 */
VentureInvoiceStateMachine *venture_invoice_state_machine_new(void);

/**
 * venture_invoice_state_machine_add_state:
 * @self: the lifecycle
 * @name: a new state name
 * @phase: draft or sent; financial states remain derived from allocations
 * @error: (out) (optional): the error
 * Returns: TRUE if the plugin state was added
 */
gboolean venture_invoice_state_machine_add_state(VentureInvoiceStateMachine *self,
	const gchar *name, VentureInvoiceStatus phase, GError **error);

/**
 * venture_invoice_state_machine_add_transition:
 * @self: the lifecycle
 * @from: the source state
 * @to: the destination state
 * @error: (out) (optional): the error
 * Returns: TRUE if the edge was added
 */
gboolean venture_invoice_state_machine_add_transition(VentureInvoiceStateMachine *self,
	const gchar *from, const gchar *to, GError **error);

/**
 * venture_invoice_state_machine_get_phase:
 * @self: the lifecycle
 * @name: the state
 * @phase: (out): the financial phase
 * @error: (out) (optional): the error
 * Returns: TRUE if the state exists
 */
gboolean venture_invoice_state_machine_get_phase(VentureInvoiceStateMachine *self,
	const gchar *name, VentureInvoiceStatus *phase, GError **error);

/**
 * venture_invoice_state_machine_check:
 * @self: the lifecycle
 * @invoice: the invoice being changed
 * @from: source state
 * @to: destination state
 * @error: (out) (optional): the error
 *
 * Checks the declared edge, then emits transition. Ordinary handlers run
 * first, the class handler last. The first FALSE vetoes the operation.
 * Returns: TRUE if all handlers allow the transition
 */
gboolean venture_invoice_state_machine_check(VentureInvoiceStateMachine *self,
	VentureInvoice *invoice, const gchar *from, const gchar *to, GError **error);
G_END_DECLS
#endif
