/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

typedef struct
{
	GHashTable *states;
	GHashTable *edges;
} VentureInvoiceStateMachinePrivate;

G_DEFINE_TYPE_WITH_PRIVATE(VentureInvoiceStateMachine, venture_invoice_state_machine, G_TYPE_OBJECT)

static gboolean
allow_transition(VentureInvoiceStateMachine *self, VentureInvoice *invoice,
	const gchar *from, const gchar *to)
{
	return TRUE;
}

static gboolean
accumulate_veto(GSignalInvocationHint *hint, GValue *accumulator,
	const GValue *result, gpointer data)
{
	gboolean allow;

	allow = g_value_get_boolean(result);
	g_value_set_boolean(accumulator, allow);
	return allow;
}

static void
machine_finalize(GObject *object)
{
	VentureInvoiceStateMachinePrivate *priv;

	priv = venture_invoice_state_machine_get_instance_private(VENTURE_INVOICE_STATE_MACHINE(object));
	g_hash_table_unref(priv->states);
	g_hash_table_unref(priv->edges);
	G_OBJECT_CLASS(venture_invoice_state_machine_parent_class)->finalize(object);
}

static void
venture_invoice_state_machine_class_init(VentureInvoiceStateMachineClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = machine_finalize;
	klass->transition = allow_transition;
	/**
	 * VentureInvoiceStateMachine::transition:
	 * @self: the lifecycle
	 * @invoice: the invoice
	 * @from: source state
	 * @to: destination state
	 *
	 * RUN_LAST. Handlers run before the default class handler, inside the
	 * settlement transaction. A FALSE result stops emission and vetoes all
	 * writes. Handlers must keep external side effects until after commit.
	 * Returns: TRUE to allow, FALSE to veto
	 */
	g_signal_new("transition", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET(VentureInvoiceStateMachineClass, transition),
		accumulate_veto, NULL, NULL, G_TYPE_BOOLEAN, 3,
		VENTURE_TYPE_INVOICE, G_TYPE_STRING, G_TYPE_STRING);
}

static void
venture_invoice_state_machine_init(VentureInvoiceStateMachine *self)
{
	/* All built-in lifecycle transitions are declared here, including the
	 * reopening caused by a refund. Services never repeat this table. */
	static const gchar *const edges[][2] = {
		{ "draft", "sent" }, { "draft", "void" },
		{ "sent", "partially_paid" }, { "sent", "paid" }, { "sent", "void" },
		{ "partially_paid", "paid" }, { "partially_paid", "sent" },
		{ "paid", "partially_paid" }, { "paid", "sent" }
	};
	VentureInvoiceStateMachinePrivate *priv;
	guint i;

	priv = venture_invoice_state_machine_get_instance_private(self);
	priv->states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	priv->edges = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i <= VENTURE_INVOICE_STATUS_PARTIALLY_PAID; i++)
		g_hash_table_insert(priv->states, g_strdup(venture_enum_to_nick(
			VENTURE_TYPE_INVOICE_STATUS, (gint)i)), GUINT_TO_POINTER(i + 1));
	for (i = 0; i < G_N_ELEMENTS(edges); i++)
		g_hash_table_add(priv->edges, g_strdup_printf("%s>%s", edges[i][0], edges[i][1]));
}

VentureInvoiceStateMachine *
venture_invoice_state_machine_new(void)
{
	return g_object_new(VENTURE_TYPE_INVOICE_STATE_MACHINE, NULL);
}

gboolean
venture_invoice_state_machine_get_phase(VentureInvoiceStateMachine *self,
	const gchar *name, VentureInvoiceStatus *phase, GError **error)
{
	VentureInvoiceStateMachinePrivate *priv;
	gpointer value;

	priv = venture_invoice_state_machine_get_instance_private(self);
	value = name != NULL ? g_hash_table_lookup(priv->states, name) : NULL;
	if (value == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Unknown invoice state: %s", name != NULL ? name : "(empty)");
		return FALSE;
	}
	*phase = (VentureInvoiceStatus)(GPOINTER_TO_UINT(value) - 1);
	return TRUE;
}

gboolean
venture_invoice_state_machine_add_state(VentureInvoiceStateMachine *self,
	const gchar *name, VentureInvoiceStatus phase, GError **error)
{
	VentureInvoiceStateMachinePrivate *priv;
	const gchar *p;

	priv = venture_invoice_state_machine_get_instance_private(self);
	if (name == NULL || *name == '\0' ||
		(phase != VENTURE_INVOICE_STATUS_DRAFT && phase != VENTURE_INVOICE_STATUS_SENT))
		goto invalid;
	for (p = name; *p != '\0'; p++)
		if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && *p != '_')
			goto invalid;
	if (g_hash_table_contains(priv->states, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
			"Invoice state %s already exists", name);
		return FALSE;
	}
	g_hash_table_insert(priv->states, g_strdup(name), GUINT_TO_POINTER((guint)phase + 1));
	return TRUE;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"A plugin state needs a simple name and a draft or sent financial phase");
	return FALSE;
}

gboolean
venture_invoice_state_machine_add_transition(VentureInvoiceStateMachine *self,
	const gchar *from, const gchar *to, GError **error)
{
	VentureInvoiceStateMachinePrivate *priv;
	g_autofree gchar *edge = NULL;
	VentureInvoiceStatus phase;

	priv = venture_invoice_state_machine_get_instance_private(self);
	if (!venture_invoice_state_machine_get_phase(self, from, &phase, error) ||
		!venture_invoice_state_machine_get_phase(self, to, &phase, error))
		return FALSE;
	edge = g_strdup_printf("%s>%s", from, to);
	if (g_hash_table_contains(priv->edges, edge))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
			"That invoice transition already exists");
		return FALSE;
	}
	g_hash_table_add(priv->edges, g_steal_pointer(&edge));
	return TRUE;
}

gboolean
venture_invoice_state_machine_check(VentureInvoiceStateMachine *self,
	VentureInvoice *invoice, const gchar *from, const gchar *to, GError **error)
{
	VentureInvoiceStateMachinePrivate *priv;
	g_autofree gchar *edge = NULL;
	gboolean allowed;

	priv = venture_invoice_state_machine_get_instance_private(self);
	edge = g_strdup_printf("%s>%s", from, to);
	if (!g_hash_table_contains(priv->edges, edge))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"An invoice cannot go from %s to %s", from, to);
		return FALSE;
	}
	allowed = FALSE;
	g_signal_emit_by_name(self, "transition", invoice, from, to, &allowed);
	if (!allowed)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Invoice transition %s to %s was vetoed", from, to);
	return allowed;
}
