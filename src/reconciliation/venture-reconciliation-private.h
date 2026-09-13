#ifndef VENTURE_RECONCILIATION_PRIVATE_H
#define VENTURE_RECONCILIATION_PRIVATE_H
/* Reads only declared, non-sensitive fields of the requested property type. */
gpointer venture_reconciliation_dup_field(VentureEntity *entity, const gchar *name, GType type);
#endif
