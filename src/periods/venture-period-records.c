/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

void
venture_period_records_register_constraints(void)
{
	g_autoptr(VentureEntityClass) klass = NULL;
	VentureColumnFlags flags;

	klass = g_type_class_ref(VENTURE_TYPE_ACCOUNT);
	flags = venture_entity_class_get_column_flags(klass, "code");
	venture_entity_class_set_column_flags(klass, "code",
		(flags & ~VENTURE_COLUMN_FLAG_UNIQUE) |
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION);
}
