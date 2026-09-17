/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#define STR(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
#define INT(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
#define DATE(n, l) VENTURE_FIELD(n, l, NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
/* An account row never holds a password: secret-env names the variable
 * whose value is read at sync time, the same shape as the Stripe module. */
static const VentureFieldDecl account_fields[] = {
	VENTURE_FIELD_NAME("address", "Address", "The mailbox this account receives as"),
	VENTURE_FIELD_NAME("imap-host", "IMAP host", NULL),
	INT("imap-port", "IMAP port", "993 for TLS, 143 otherwise"),
	STR("imap-tls", "Security", "tls, starttls or none"),
	STR("username", "Username", NULL),
	STR("secret-env", "Secret variable", "NAME of a VENTURE_IMAP_* environment variable holding the password or app token; never the value"),
	STR("folders", "Folders", "Comma-separated folders to watch; INBOX when empty"),
	STR("capture-address", "Capture address", "Mail to this address becomes a capture inbox item"),
	STR("capture-folder", "Capture folder", "Every message in this folder becomes a capture inbox item"),
	VENTURE_FIELD("cursors", "Cursors", "Last synced UID per folder, maintained by the sync service", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", "Included in the sweep", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	DATE("last-synced-at", "Last synced"),
	VENTURE_FIELD_TEXT("last-error", "Last error", NULL)
};
VENTURE_DEFINE_ENTITY(VentureMailAccount, venture_mail_account, account_fields)
/* One row per fetched message. The UID key is what makes a rerun a no-op
 * even when the cursor write was lost. */
static const VentureFieldDecl inbound_fields[] = {
	VENTURE_FIELD_REF("account-id", "Account", NULL, "mail_account", VENTURE_COLUMN_FLAG_NONE),
	STR("folder", "Folder", NULL),
	INT("uid", "UID", NULL),
	VENTURE_FIELD("uid-key", "UID key", "account:folder:uid", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("message-id", "Message-ID", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("thread-id", "Thread", "Message-ID of the thread root", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	STR("from-address", "From", NULL),
	VENTURE_FIELD_NAME("subject", "Subject", NULL),
	DATE("received-at", "Date"),
	VENTURE_FIELD_REF("document-id", "Raw message", NULL, "document", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("interaction-id", "Interaction", NULL, "interaction", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("capture-item-id", "Capture item", NULL, "capture_item", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("outbound", "Outbound", "Sent from the account address", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureMailInbound, venture_mail_inbound, inbound_fields)
static const VentureFieldDecl unmatched_fields[] = {
	VENTURE_FIELD("address", "Address", "Normalised, unique within the organization", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	STR("name", "Name", "Display name from the last message"),
	INT("seen", "Seen", "Messages involving this address"),
	STR("last-subject", "Last subject", NULL),
	DATE("last-seen-at", "Last seen")
};
VENTURE_DEFINE_ENTITY(VentureMailUnmatchedSender, venture_mail_unmatched_sender, unmatched_fields)
