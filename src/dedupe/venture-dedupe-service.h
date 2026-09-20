/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DEDUPE_SERVICE_H
#define VENTURE_DEDUPE_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_DEDUPE_SERVICE (venture_dedupe_service_get_type())
G_DECLARE_FINAL_TYPE(VentureDedupeService, venture_dedupe_service, VENTURE, DEDUPE_SERVICE, GObject)

/**
 * venture_dedupe_service_get:
 * @database: database owning the records
 *
 * Returns the per-database duplicate service, installing on first use the
 * save validators that keep =duplicate_candidate= rows and the
 * =merged_into_id= forwarding pointer out of reach of generic writes.
 *
 * Returns: (transfer none): the service; the database owns it
 */
VentureDedupeService *venture_dedupe_service_get(VentureDatabase *database);

/**
 * venture_dedupe_service_scan:
 * @self: the service
 * @organization_id: one legal entity; the scan never crosses organizations
 * @kind: "company" or "contact"
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal or persistence failure
 *
 * Compares every live record of @kind in the organization with every other
 * and writes one open =duplicate_candidate= per matching pair, in one
 * transaction. A pair matches on an equal normalised email, phone or
 * website (the leads module's rules), on a shared email domain with a name
 * similarity of at least 50, or on a name similarity of at least 75. A
 * rescan updates the existing row for a pair that still matches, restores
 * one that was dropped, and drops open candidates that no longer match;
 * merged and dismissed candidates are left as they are.
 *
 * Returns: the number of open candidates for @kind after the scan, or -1
 */
gint venture_dedupe_service_scan(VentureDedupeService *self, gint64 organization_id,
	const gchar *kind, const VentureActor *actor, GError **error);

/**
 * venture_dedupe_service_merge:
 * @self: the service
 * @candidate: an open =duplicate_candidate=
 * @survivor_id: which of the pair to keep; the other is the loser
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal or persistence failure
 *
 * venture_dedupe_service_merge_records() for the candidate's pair, then
 * marks the candidate =merged= and drops every other open candidate that
 * named the loser. One transaction.
 *
 * Returns: (transfer full) (nullable): the survivor as stored, or %NULL
 */
VentureEntity *venture_dedupe_service_merge(VentureDedupeService *self, VentureEntity *candidate,
	gint64 survivor_id, const VentureActor *actor, GError **error);

/**
 * venture_dedupe_service_merge_records:
 * @self: the service
 * @kind: "company" or "contact"
 * @survivor_id: the record kept
 * @loser_id: the record folded into it
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal or persistence failure
 *
 * In one transaction: re-points every reference field the field tables
 * declare against @kind from the loser to the survivor, whatever module owns
 * it, except references declaring %VENTURE_COLUMN_FLAG_RETAIN_REFERENCE;
 * retained evidence keeps its original subject. Fills each empty survivor
 * field from the loser and keeps the survivor's value on conflict, noting the loser's differing values as an interaction
 * on the survivor's timeline; stamps the loser =merged_into_id= and soft
 * deletes it; and writes one =dedupe= audit entry against the survivor that
 * lists what moved. A failure anywhere leaves nothing changed.
 *
 * Refused: a survivor and loser in different organizations, a record merged
 * onto itself, a loser with issued invoices or bills in a currency the
 * survivor's own issued documents do not use, and a deleted or unknown id.
 *
 * Returns: (transfer full) (nullable): the survivor as stored, or %NULL
 */
VentureEntity *venture_dedupe_service_merge_records(VentureDedupeService *self, const gchar *kind,
	gint64 survivor_id, gint64 loser_id, const VentureActor *actor, GError **error);

/**
 * venture_dedupe_service_dismiss:
 * @self: the service
 * @candidate: an open =duplicate_candidate=
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal or persistence failure
 *
 * Marks a pair as not duplicates. Later scans leave it alone.
 *
 * Returns: %TRUE on success
 */
gboolean venture_dedupe_service_dismiss(VentureDedupeService *self, VentureEntity *candidate,
	const VentureActor *actor, GError **error);

/**
 * venture_dedupe_name_similarity:
 * @a: (nullable): one name
 * @b: (nullable): another
 *
 * The documented name similarity: both names are lower-cased, every
 * character that is not a letter or digit becomes a space, the words are
 * split on whitespace, and the legal and filler words =inc=, =ltd=, =llc=,
 * =co=, =corp=, =gmbh=, =plc=, =the=, =and=, =of= are dropped. The score is
 * the number of words the two lists share, as a percentage of the longer
 * list. Two empty lists score 0.
 *
 * Returns: 0 to 100
 */
gint venture_dedupe_name_similarity(const gchar *a, const gchar *b);

/**
 * venture_dedupe_field_text:
 * @record: a record
 * @field: a declared field name
 *
 * The value of one field as short text, the way the merge notes it: a
 * money amount with its currency, a timestamp in ISO 8601, an enum by its
 * nick, a boolean as yes or no.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL when the field is
 *   empty or unknown
 */
gchar *venture_dedupe_field_text(VentureEntity *record, const gchar *field);

/**
 * venture_dedupe_merged_into:
 * @database: database owning the records
 * @record: (nullable): a company or contact, possibly soft-deleted
 *
 * Follows a merged record's =merged_into_id= to the live survivor, so a
 * link kept from before the merge still resolves.
 *
 * Returns: the survivor's id, or 0 when @record was not merged away
 */
gint64 venture_dedupe_merged_into(VentureDatabase *database, VentureEntity *record);

/**
 * venture_dedupe_actions_register:
 * @database: database owning the action registry
 *
 * Registers the type-level =scan= action and the record-level =merge= and
 * =dismiss= actions on =duplicate_candidate=: the REST endpoints, the CLI
 * verbs and the assistant tools.
 */
void venture_dedupe_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
