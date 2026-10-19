/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

struct _VentureLateFeeService
{
	GObject parent_instance;
};

G_DEFINE_FINAL_TYPE(VentureLateFeeService, venture_late_fee_service, G_TYPE_OBJECT)

static void venture_late_fee_service_class_init(VentureLateFeeServiceClass *klass) { (void)klass; }
static void venture_late_fee_service_init(VentureLateFeeService *self) { (void)self; }

static gboolean
not_yet(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Late fees are not implemented yet");
	return FALSE;
}

VentureLateFeeService *
venture_late_fee_service_get(VentureDatabase *database)
{
	static VentureLateFeeService *only = NULL;
	(void)database;
	if (only == NULL)
		only = g_object_new(VENTURE_TYPE_LATE_FEE_SERVICE, NULL);
	return only;
}

JsonNode *venture_late_fee_service_preview(VentureLateFeeService *self, gint64 invoice_id, GDateTime *as_of, GError **error)
{ (void)self; (void)invoice_id; (void)as_of; not_yet(error); return NULL; }
gboolean venture_late_fee_service_approve(VentureLateFeeService *self, VentureEntity *fee, GDateTime *date, const VentureActor *actor, GError **error)
{ (void)self; (void)fee; (void)date; (void)actor; return not_yet(error); }
gboolean venture_late_fee_service_skip(VentureLateFeeService *self, VentureEntity *fee, const gchar *reason, const VentureActor *actor, GError **error)
{ (void)self; (void)fee; (void)reason; (void)actor; return not_yet(error); }
gboolean venture_late_fee_service_skip_notice(VentureLateFeeService *self, VentureEntity *fee, const gchar *reason, const VentureActor *actor, GError **error)
{ (void)self; (void)fee; (void)reason; (void)actor; return not_yet(error); }
gboolean venture_late_fee_service_waive(VentureLateFeeService *self, VentureEntity *fee, const gchar *reason, const gchar *note, GDateTime *date, const VentureActor *actor, GError **error)
{ (void)self; (void)fee; (void)reason; (void)note; (void)date; (void)actor; return not_yet(error); }
JsonNode *venture_late_fee_service_decide(VentureLateFeeService *self, gint64 organization_id, const gint64 *ids, gsize n_ids, gboolean approve, const gchar *reason, const VentureActor *actor, GError **error)
{ (void)self; (void)organization_id; (void)ids; (void)n_ids; (void)approve; (void)reason; (void)actor; not_yet(error); return NULL; }
gboolean venture_late_fee_service_pause(VentureLateFeeService *self, const gchar *scope, gint64 id, GDateTime *until, const gchar *reason, const VentureActor *actor, GError **error)
{ (void)self; (void)scope; (void)id; (void)until; (void)reason; (void)actor; return not_yet(error); }
gboolean venture_late_fee_service_resume(VentureLateFeeService *self, const gchar *scope, gint64 id, const VentureActor *actor, GError **error)
{ (void)self; (void)scope; (void)id; (void)actor; return not_yet(error); }
gchar *venture_late_fee_service_pause_state(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *at)
{ (void)self; (void)invoice; (void)at; return NULL; }
gboolean venture_late_fee_service_sweep_invoice(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *as_of, gboolean dry_run, JsonArray *plan, VentureLateFeeSweepTally *tally, const VentureActor *actor, GError **error)
{ (void)self; (void)invoice; (void)as_of; (void)dry_run; (void)plan; (void)tally; (void)actor; (void)error; return FALSE; }
gboolean venture_late_fee_service_sweep_needed(VentureLateFeeService *self, gint64 organization_id)
{ (void)self; (void)organization_id; return FALSE; }
gchar *venture_late_fee_service_notice_text(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *as_of)
{ (void)self; (void)invoice; (void)as_of; return g_strdup(""); }
gboolean venture_late_fee_service_record_notice(VentureLateFeeService *self, VentureEntity *invoice, VentureEntity *event, GDateTime *as_of, const VentureActor *actor, GError **error)
{ (void)self; (void)invoice; (void)event; (void)as_of; (void)actor; (void)error; return TRUE; }
gboolean venture_late_fee_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{ (void)database; (void)record; (void)removal; (void)error; return TRUE; }
void venture_late_fee_actions_register(VentureDatabase *database) { (void)database; }
void venture_late_fee_register_reports(VentureReportRegistry *registry) { (void)registry; }
void venture_late_fee_append_timeline(VentureContext *context, const gchar *target_type, gint64 target_id, guint limit, VentureLateFeeTimelineAdd add, GPtrArray *events)
{ (void)context; (void)target_type; (void)target_id; (void)limit; (void)add; (void)events; }
