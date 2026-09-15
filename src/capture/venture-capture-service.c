/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureCaptureService
{
	GObject parent_instance;
	GWeakRef database;
	VentureEntity *writing;
	gboolean busy;
};
G_DEFINE_FINAL_TYPE(VentureCaptureService, venture_capture_service, G_TYPE_OBJECT)

static void
service_finalize(GObject *object)
{
	g_weak_ref_clear(&VENTURE_CAPTURE_SERVICE(object)->database);
	G_OBJECT_CLASS(venture_capture_service_parent_class)->finalize(object);
}

static void
venture_capture_service_class_init(VentureCaptureServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = service_finalize;
}

static void
venture_capture_service_init(VentureCaptureService *self)
{
	g_weak_ref_init(&self->database, NULL);
}

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureCaptureService: %s", message);
	return FALSE;
}

static gboolean
module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"capture_item") != G_TYPE_INVALID;
}

VentureCaptureService *
venture_capture_service_get(VentureDatabase *database)
{
	VentureCaptureService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-capture-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_CAPTURE_SERVICE, NULL);
		g_weak_ref_set(&self->database, database);
		g_object_set_data_full(G_OBJECT(database), "venture-capture-service", self, g_object_unref);
	}
	return self;
}

static VentureDatabase *
service_db(VentureCaptureService *self)
{
	return g_weak_ref_get(&self->database);
}

static gboolean
save_internal(VentureCaptureService *self, VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	VentureEntity *previous = self->writing;
	gboolean ok;
	self->writing = entity;
	ok = venture_database_save(db, entity, actor, error);
	self->writing = previous;
	return ok;
}

static gboolean
begin_op(VentureCaptureService *self, VentureDatabase *db, GError **error)
{
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG, "The capture module is disabled (modules.capture.enabled)");
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "A capture operation is already in progress");
	if (!venture_database_begin(db, error))
		return FALSE;
	self->busy = TRUE;
	return TRUE;
}

static gboolean
finish_op(VentureCaptureService *self, VentureDatabase *db, gboolean ok, GError **error)
{
	if (ok)
		ok = venture_database_commit(db, error);
	else
		venture_database_rollback(db);
	self->busy = FALSE;
	return ok;
}

VentureEntity *
venture_capture_service_ingest(VentureCaptureService *self, const gchar *kind,
	const gchar *title, const gchar *source, gint64 document_id, const gchar *vendor,
	const VentureMoney *amount, GDateTime *occurred_at, const gchar *notes,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_return_val_if_fail(VENTURE_IS_CAPTURE_SERVICE(self), NULL);
	if (g_strcmp0(kind, "receipt") != 0 && g_strcmp0(kind, "supplier_invoice") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Kind is receipt or supplier_invoice"), NULL;
	if (venture_string_is_empty(title))
		return refuse(error, VENTURE_ERROR_VALIDATION, "A captured document needs a title"), NULL;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	if (!begin_op(self, db, error))
		return NULL;
	item = VENTURE_ENTITY(venture_capture_item_new());
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) organization = venture_database_find_one(db, query, NULL);
		if (organization != NULL)
			venture_entity_set_organization_id(item, venture_entity_get_id(organization));
	}
	g_object_set(item, "title", title, "kind", kind, "status", "inbox",
		"source", source != NULL ? source : "upload", "document-id", document_id,
		"vendor", vendor, "amount", amount, "occurred-at", occurred_at, "notes", notes, NULL);
	if (!save_internal(self, db, item, actor, error))
		return finish_op(self, db, FALSE, error), NULL;
	if (!finish_op(self, db, TRUE, error))
		return NULL;
	return g_steal_pointer(&item);
}

static gint64
find_vendor(VentureDatabase *db, gint64 org, const gchar *name, gint64 company_id)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) company = NULL;
	if (company_id > 0)
		return company_id;
	if (venture_string_is_empty(name) ||
		venture_entity_registry_lookup(venture_entity_registry_get_default(), "company") == G_TYPE_INVALID)
		return 0;
	query = venture_query_new(VENTURE_TYPE_COMPANY);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, name, NULL);
	company = venture_database_find_one(db, query, NULL);
	return company != NULL ? venture_entity_get_id(company) : 0;
}

VentureEntity *
venture_capture_service_convert(VentureCaptureService *self, VentureEntity *item,
	const gchar *as, JsonObject *options, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *vendor = NULL;
	g_autofree gchar *kind = NULL;
	gint64 document_id = 0;
	gint64 org;
	g_return_val_if_fail(VENTURE_IS_CAPTURE_SERVICE(self), NULL);
	if (g_strcmp0(as, "expense") != 0 && g_strcmp0(as, "vendor_bill") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Convert as expense or vendor_bill"), NULL;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	if (!begin_op(self, db, error))
		return NULL;
	g_object_get(item, "status", &status, "title", &title, "vendor", &vendor, "kind", &kind,
		"amount", &amount, "occurred-at", &when, "document-id", &document_id, NULL);
	if (g_strcmp0(status, "inbox") != 0)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Only inbox rows can be converted");
		return finish_op(self, db, FALSE, error), NULL;
	}
	org = venture_entity_get_organization_id(item);
	if (g_strcmp0(as, "expense") == 0)
	{
		const gchar *category = options != NULL ? venture_json_object_get_string(options, "category", "general") : "general";
		result = VENTURE_ENTITY(venture_expense_new());
		g_object_set(result, "description", title, "vendor", vendor, "amount", amount,
			"occurred-at", when, "category", category, NULL);
		venture_entity_set_organization_id(result, org);
		if (!venture_database_save(db, result, actor, error))
			return finish_op(self, db, FALSE, error), NULL;
		if (document_id > 0)
		{
			g_autoptr(VentureEntity) document = venture_database_get(db, VENTURE_TYPE_DOCUMENT, document_id, NULL);
			if (document != NULL)
			{
				g_object_set(document, "expense-id", venture_entity_get_id(result), NULL);
				if (!venture_database_save(db, document, actor, error))
					return finish_op(self, db, FALSE, error), NULL;
			}
		}
	}
	else
	{
		gint64 company_id = options != NULL ? venture_json_object_get_int(options, "company_id", 0) : 0;
		g_autoptr(VentureEntity) line = NULL;
		g_autofree gchar *number = NULL;
		company_id = find_vendor(db, org, vendor, company_id);
		if (company_id == 0)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "A supplier invoice needs company_id or a matching vendor company");
			return finish_op(self, db, FALSE, error), NULL;
		}
		number = g_strdup_printf("CAP-%" G_GINT64_FORMAT, venture_entity_get_id(item));
		result = VENTURE_ENTITY(venture_vendor_bill_new());
		g_object_set(result, "company-id", company_id, "number", number, "currency",
			amount != NULL ? venture_money_get_currency(amount) : "USD",
			"status", "draft", "memo", title, NULL);
		if (when == NULL)
			when = venture_time_now();
		g_object_set(result, "bill-date", when, NULL);
		venture_entity_set_organization_id(result, org);
		if (!venture_database_save(db, result, actor, error))
			return finish_op(self, db, FALSE, error), NULL;
		line = VENTURE_ENTITY(venture_vendor_bill_line_new());
		g_object_set(line, "bill-id", venture_entity_get_id(result), "description", title,
			"quantity", "1", "unit-price", amount, NULL);
		venture_entity_set_organization_id(line, org);
		if (!venture_database_save(db, line, actor, error))
			return finish_op(self, db, FALSE, error), NULL;
	}
	g_object_set(item, "status", "converted", "result-type", as, "result-id", venture_entity_get_id(result), NULL);
	if (!save_internal(self, db, item, actor, error))
		return finish_op(self, db, FALSE, error), NULL;
	if (!finish_op(self, db, TRUE, error))
		return NULL;
	return g_steal_pointer(&result);
}

gboolean
venture_capture_service_reject(VentureCaptureService *self, VentureEntity *item,
	const gchar *reason, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autofree gchar *status = NULL;
	g_return_val_if_fail(VENTURE_IS_CAPTURE_SERVICE(self), FALSE);
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!begin_op(self, db, error))
		return FALSE;
	g_object_get(item, "status", &status, NULL);
	if (g_strcmp0(status, "inbox") != 0)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Only inbox rows can be rejected");
		return finish_op(self, db, FALSE, error);
	}
	g_object_set(item, "status", "rejected", "rejection-reason", reason, NULL);
	if (!save_internal(self, db, item, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

gboolean
venture_capture_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureCaptureService *self;
	g_autofree gchar *status = NULL;
	(void)actor;
	*handled = FALSE;
	if (!VENTURE_IS_CAPTURE_ITEM(record))
		return TRUE;
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG, "The capture module is disabled (modules.capture.enabled)");
	self = venture_capture_service_get(database);
	if (self->writing == record)
		return TRUE;
	g_object_get(record, "status", &status, NULL);
	if (g_strcmp0(status, "converted") == 0 || g_strcmp0(status, "rejected") == 0)
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
			"Conversion and rejection go through VentureCaptureService");
	return TRUE;
}
