/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const VentureFieldDecl backup_fields[] = {
	VENTURE_FIELD("format", "Format", "json or csv", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_TEXT("payload", "Payload", "Exported accounting pack"),
	VENTURE_FIELD("state", "State", "exported or restored", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("checksum", "Checksum", "SHA-256 of the payload", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureAccountingBackup, venture_accounting_backup, backup_fields)
