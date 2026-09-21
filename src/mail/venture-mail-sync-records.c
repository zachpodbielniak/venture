/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#define STR(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
#define INT(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
#define DATE(n, l) VENTURE_FIELD(n, l, NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
/* Account identity is public metadata; an explicit encrypted binding supplies
 * the credential. Historical environment labels are never resolved. */
static const VentureFieldDecl account_fields[] = {
	VENTURE_FIELD_REF("private-owner-id", "Private owner", "User whose private connector this is; zero explicitly shares business data with the organization", "user", VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER),
	VENTURE_FIELD_NAME("address", "Address", "The mailbox this account receives as"),
	VENTURE_FIELD_NAME("imap-host", "IMAP host", NULL),
	VENTURE_FIELD("active", "Active", "Included in the sweep; switched off after repeated refused logins", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	INT("consecutive-failures", "Consecutive failures", "Sync attempts in a row that failed before reaching the mailbox; maintained by the sync service"),
	VENTURE_FIELD("next-attempt-at", "Next attempt", "The sweep skips the account until then; maintained by the sync service", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	DATE("last-synced-at", "Last synced"),
	VENTURE_FIELD_TEXT("last-error", "Last error", NULL),
	INT("imap-port", "IMAP port", "993 for TLS, 143 otherwise"),
	STR("imap-tls", "Security", "tls or starttls"),
	STR("username", "Username", NULL),
	STR("secret-env", "Legacy secret variable", "Unused historical metadata; configure an explicit encrypted connector binding"),
	STR("folders", "Folders", "Comma-separated folders to watch; INBOX when empty. The capture folder is always included"),
	STR("capture-address", "Capture address", "Mail to exactly this address, or to it with a +tag, becomes a capture inbox item; must differ from the account address"),
	STR("capture-folder", "Capture folder", "Every message in this folder becomes a capture inbox item"),
	VENTURE_FIELD_TEXT("ignore-patterns", "Ignore patterns", "Comma- or newline-separated address globs such as noreply@* or *@notifications.example; matching addresses touch no CRM record"),
	STR("internal-domains", "Internal domains", "Comma-separated domains whose addresses never become unmatched senders"),
	VENTURE_FIELD("sync-since", "Sync since", "A folder's first sync starts at mail received on or after this date; empty means everything", VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cursors", "Cursors", "Per folder: last synced UID, UIDVALIDITY and a failing message's attempts; maintained by the sync service", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("sync-lease-until", "Sync lease", "Held while a sync runs so an overlapping caller skips the account", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureMailAccount, venture_mail_account, account_fields)
/* One row per fetched message. The UID key is what makes a rerun a no-op
 * even when the cursor write was lost. */
static const VentureFieldDecl inbound_fields[] = {
	VENTURE_FIELD_REF("account-id", "Account", NULL, "mail_account", VENTURE_COLUMN_FLAG_OPTIONAL_PERSONAL_OWNER),
	STR("folder", "Folder", NULL),
	INT("uid", "UID", NULL),
	INT("uid-validity", "UIDVALIDITY", "The folder's UIDVALIDITY when the UID was read; UIDs from another value are different messages"),
	VENTURE_FIELD("uid-key", "UID key", "account:folder:uidvalidity:uid; account:folder:uid on rows filed before UIDVALIDITY was recorded", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("message-id", "Message-ID", "Synthesised from the message's hash when it had none", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("thread-id", "Thread", "Message-ID of the thread root", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("from-address", "From", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("subject", "Subject", NULL),
	DATE("received-at", "Date"),
	VENTURE_FIELD_REF("document-id", "Raw message", NULL, "document", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("interaction-id", "Interaction", NULL, "interaction", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("capture-item-id", "Capture item", NULL, "capture_item", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("outbound", "Outbound", "Sent from the account address", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("duplicate-of-id", "Duplicate of", "The row first filed with this Message-ID; this one records only another folder or account holding it", "mail_inbound", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("skip-reason", "Skip reason", "Why the message was filed as a stub rather than processed")
};
VENTURE_DEFINE_ENTITY(VentureMailInbound, venture_mail_inbound, inbound_fields)
static const VentureFieldDecl unmatched_fields[] = {
	VENTURE_FIELD("address", "Address", "Normalised, unique within the organization", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	STR("name", "Name", "Display name from the last message"),
	INT("seen", "Seen", "Messages involving this address"),
	STR("last-subject", "Last subject", NULL),
	DATE("last-seen-at", "Last seen"),
	VENTURE_FIELD("dismissed", "Dismissed", "An ignore-list entry: later mail still counts here but asks for nothing; filter the list with dismissed=false", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureMailUnmatchedSender, venture_mail_unmatched_sender, unmatched_fields)
