/*
 * venture-database.c - Storage and record persistence
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"
#include "db/venture-database-snapshot-private.h"
#include "sequences/venture-sequence-service-private.h"
#include "ledger/venture-ledger-private.h"
#include "db/venture-migrations.h"
#include "activities/venture-activity-private.h"
#include "pipelines/venture-pipelines-private.h"
#include "report/venture-headline-private.h"

#include <string.h>

static gboolean
stripe_owned(VentureEntity *entity)
{
	GType type = G_OBJECT_TYPE(entity);
	return type == venture_stripe_checkout_get_type() ||
		type == venture_stripe_event_get_type() ||
		type == venture_processor_payout_get_type() ||
		type == venture_processor_payout_item_get_type() ||
		type == venture_processor_dispute_get_type() ||
		type == venture_processor_exception_get_type();
}

struct _VentureDatabase
{
	GObject parent_instance;

	OrmEngine		*engine;
	OrmConnection		*connection;
	OrmTransaction		*transaction;
	VentureDatabaseBackend	 backend;
	gchar			*uri;

	/*
	 * How many begins are open, and which thread opened them.
	 *
	 * The lock is taken once per begin and released once per matching
	 * commit or rollback, so the recursive count returns to zero when the
	 * outermost pair closes. Both are needed: without the depth an inner
	 * commit would commit the outer transaction, and without the owner a
	 * commit issued with no begin would release a level this thread never
	 * took. Either one leaks a level, which is invisible while everything
	 * is single-threaded and a permanent, silent deadlock the first time
	 * anything else touches the database.
	 */
	guint			 transaction_depth;
	VentureMailOutbox *mail_outbox;
	GThread			*transaction_owner;

	/*
	 * Serialises every operation. SQLite admits one writer at a time
	 * regardless, and for a single-operator system one connection behind
	 * a mutex is both simpler and easier to reason about than a pool --
	 * there is no interleaving to get wrong.
	 */
	GRecMutex		 lock;

	/*
	 * Save validators, run inside the lock before a write. A record type
	 * whose invariants span rows -- a link whose both ends must exist --
	 * cannot check them from venture_entity_validate(), which has no
	 * database, so it registers one of these instead. Every writer goes
	 * through venture_database_save(), so every writer gets the check.
	 */
	GPtrArray		*validators;
	VentureEntity *stripe_write_permit;
	VentureAssetService *asset_service;
	VentureAccessPolicy *access_policy;
	VentureBillingService *billing;
	VentureQuoteService *quote_service;
	VentureLeadService *lead_service;
	VentureActivityService *activities;
	VenturePayablesService *payables;
	VentureBankMatchService *bank_match_service;
	VentureDealService *deal_service;
	VentureSequenceService *sequence_service;
	VentureActionRegistry *actions;
};

typedef struct
{
	GType			 entity_type;
	VentureSaveValidator	 validate;
	gpointer		 user_data;
	GDestroyNotify		 destroy;
} VentureDatabaseValidator;

static void
venture_database_validator_free(gpointer data)
{
	VentureDatabaseValidator *validator;

	validator = data;

	if (NULL != validator->destroy)
		validator->destroy(validator->user_data);

	g_free(validator);
}

enum
{
	SIGNAL_ENTITY_SAVED,
	SIGNAL_ENTITY_DELETED,
	SIGNAL_AUDIT,
	SIGNAL_TRANSACTION_FINISHED,
	N_SIGNALS
};

static guint venture_database_signals[N_SIGNALS] = { 0 };

G_DEFINE_FINAL_TYPE(VentureDatabase, venture_database, G_TYPE_OBJECT)

static void
venture_database_finalize(GObject *object)
{
	VentureDatabase *self;

	self = VENTURE_DATABASE(object);
	g_clear_object(&self->mail_outbox);

	g_clear_object(&self->access_policy);
	g_clear_object(&self->quote_service);
	g_clear_object(&self->payables);
	g_clear_object(&self->bank_match_service);
	g_clear_object(&self->deal_service);
	g_clear_object(&self->sequence_service);
	g_clear_object(&self->actions);
	g_clear_object(&self->transaction);
	g_clear_object(&self->billing);
	g_clear_object(&self->lead_service);

	if (NULL != self->connection)
	{
		orm_connection_close(self->connection);
		g_clear_object(&self->connection);
	}

	g_clear_object(&self->engine);
	g_clear_pointer(&self->uri, g_free);
	g_rec_mutex_clear(&self->lock);
	g_clear_pointer(&self->validators, g_ptr_array_unref);
	g_clear_object(&self->asset_service);
	g_clear_object(&self->activities);

	G_OBJECT_CLASS(venture_database_parent_class)->finalize(object);
}

static void
venture_database_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (1 == id)
		g_value_set_object(value, venture_database_get_action_registry(VENTURE_DATABASE(object)));
	else if (2 == id)
		g_value_set_object(value, venture_billing_service_get(VENTURE_DATABASE(object)));
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

VentureBillingService *
venture_billing_service_get(VentureDatabase *database)
{
	if (database->billing == NULL)
		database->billing = g_object_new(VENTURE_TYPE_BILLING_SERVICE, "database", database, NULL);
	return database->billing;
}

static void
venture_database_class_init(VentureDatabaseClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_database_finalize;
	G_OBJECT_CLASS(klass)->get_property = venture_database_get_property;
	g_object_class_install_property(G_OBJECT_CLASS(klass), 2,
		g_param_spec_object("billing-service", "Billing service", "Subscription lifecycle authority",
			VENTURE_TYPE_BILLING_SERVICE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(G_OBJECT_CLASS(klass), 1,
		g_param_spec_object("action-registry", "Action registry", "Shared record actions",
			VENTURE_TYPE_ACTION_REGISTRY, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

	/**
	 * VentureDatabase::entity-saved:
	 * @self: the database
	 * @entity: the record that was written
	 * @created: %TRUE if it was inserted rather than updated
	 *
	 * Emitted after a successful write. The automation engine listens for
	 * this to fire record-change events, which is how "when a sale is
	 * recorded, do X" works without the write path knowing about
	 * automations at all.
	 */
	venture_database_signals[SIGNAL_ENTITY_SAVED] =
		g_signal_new("entity-saved", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 2, VENTURE_TYPE_ENTITY, G_TYPE_BOOLEAN);

	/**
	 * VentureDatabase::entity-deleted:
	 * @self: the database
	 * @entity: the record that was removed
	 */
	venture_database_signals[SIGNAL_ENTITY_DELETED] =
		g_signal_new("entity-deleted", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, VENTURE_TYPE_ENTITY);

	/**
	 * VentureDatabase::audit:
	 * @self: the database
	 * @entry: the audit record just written
	 */
	venture_database_signals[SIGNAL_AUDIT] =
		g_signal_new("audit", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, VENTURE_TYPE_ENTITY);

	/**
	 * VentureDatabase::transaction-finished:
	 * @self: the database
	 * @committed: TRUE only after a successful outermost commit
	 *
	 * An inner rollback emits FALSE immediately; an inner commit emits
	 * nothing. Services use this to publish committed effects only.
	 */
	venture_database_signals[SIGNAL_TRANSACTION_FINISHED] =
		g_signal_new("transaction-finished", G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
venture_database_init(VentureDatabase *self)
{
	/* Recursive, because a write inside a transaction re-enters through
	 * the same lock and a plain mutex would deadlock on itself. */
	g_rec_mutex_init(&self->lock);
	self->validators = g_ptr_array_new_with_free_func(
		venture_database_validator_free);
	self->activities = venture_activity_service_new(self);
	venture_headline_install_validators(self);
	venture_mail_sync_install_validators(self);
}

VentureQuoteService *
venture_database_get_quote_service(VentureDatabase *self)
{
	if (self->quote_service == NULL)
		self->quote_service = g_object_new(VENTURE_TYPE_QUOTE_SERVICE, "database", self, NULL);
	return self->quote_service;
}

/* --- Opening ------------------------------------------------------------- */

VentureDatabase *
venture_database_new(
	const gchar	 *uri,
	GError		**error
){
	g_autoptr(VentureDatabase) self = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *safe_uri = NULL;

	g_return_val_if_fail(NULL != uri, NULL);

	self = g_object_new(VENTURE_TYPE_DATABASE, NULL);
	self->uri = g_strdup(uri);

	/*
	 * Every message below names the URI, and a connection failure is
	 * precisely when someone copies that message into a bug report or a
	 * chat window. The password must not travel with it.
	 */
	safe_uri = venture_string_redact_uri(uri);

	if (g_str_has_prefix(uri, "sqlite://"))
	{
		const gchar *path;

		self->backend = VENTURE_DATABASE_BACKEND_SQLITE;
		path = uri + strlen("sqlite://");

		/* Create the containing directory rather than failing on a
		 * fresh install where only the parent exists. */
		if ((0 != g_strcmp0(path, ":memory:")) && g_path_is_absolute(path))
		{
			g_autofree gchar *directory = NULL;

			directory = g_path_get_dirname(path);
			g_mkdir_with_parents(directory, 0700);
		}

		self->engine = orm_engine_new_sqlite(path, &local_error);
	}
	else
	{
		self->backend = VENTURE_DATABASE_BACKEND_POSTGRES;
		self->engine = orm_engine_new(uri, &local_error);
	}

	if (NULL == self->engine)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		            "Cannot open %s: %s", safe_uri,
		            (NULL != local_error) ? local_error->message
		                                  : "unknown failure");
		return NULL;
	}

	self->connection = orm_engine_connect(self->engine, &local_error);

	if (NULL == self->connection)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		            "Cannot connect to %s: %s", safe_uri,
		            (NULL != local_error) ? local_error->message
		                                  : "unknown failure");
		return NULL;
	}

	if (VENTURE_DATABASE_BACKEND_SQLITE == self->backend)
	{
		/* Write-ahead logging lets a reader run while a write is in
		 * flight, which matters because the web UI polls while an
		 * import or an automation is writing. Foreign keys are off by
		 * default in SQLite and have to be asked for. */
		orm_connection_execute(self->connection, "PRAGMA journal_mode=WAL",
		                       NULL);
		orm_connection_execute(self->connection, "PRAGMA foreign_keys=ON",
		                       NULL);
		orm_connection_execute(self->connection, "PRAGMA synchronous=NORMAL",
		                       NULL);
	}

	return g_steal_pointer(&self);
}

/*
 * Inserts the password from the environment into @uri's authority, so that
 * `postgres://venture@host/db` becomes `postgres://venture:secret@host/db`.
 *
 * Returns: (transfer full) (nullable): the rewritten URI, or %NULL if there
 *   is nothing to do -- no variable named, nothing in it, or a password
 *   already present
 */
static gchar *
venture_database_uri_apply_password(
	const gchar	*uri,
	VentureConfig	*config
){
	g_autofree gchar *variable = NULL;
	g_autofree gchar *escaped = NULL;
	const gchar *password;
	const gchar *scheme_end;
	const gchar *authority;
	const gchar *at;

	g_object_get(config, "database-password-env", &variable, NULL);

	if (venture_string_is_empty(variable))
		return NULL;

	password = g_getenv(variable);

	if (venture_string_is_empty(password))
		return NULL;

	scheme_end = strstr(uri, "://");

	if (NULL == scheme_end)
		return NULL;

	authority = scheme_end + 3;
	at = strchr(authority, '@');

	/* No userinfo at all: there is no user to attach a password to, and
	 * inventing one would connect as somebody unexpected. */
	if (NULL == at)
		return NULL;

	/* A colon before the @ means a password is already spelled out. */
	if (NULL != memchr(authority, ':', (gsize)(at - authority)))
		return NULL;

	/* The password may contain characters that are not legal in a URI --
	 * `openssl rand -base64` happily produces `/` and `+`. */
	escaped = g_uri_escape_string(password, NULL, FALSE);

	return g_strdup_printf("%.*s:%s%s",
	                       (int)(at - uri), uri, escaped, at);
}

gchar *
venture_database_build_uri(VentureConfig *config)
{
	g_autofree gchar *uri = NULL;
	const gchar *configured;

	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);

	configured = venture_config_get_database_uri(config);

	/* A relative SQLite path is resolved against the state directory, so
	 * the default configuration works from any working directory. */
	if (g_str_has_prefix(configured, "sqlite://"))
	{
		const gchar *path;

		path = configured + strlen("sqlite://");

		if ((0 != g_strcmp0(path, ":memory:")) && !g_path_is_absolute(path))
		{
			g_autofree gchar *resolved = NULL;

			resolved = venture_config_resolve_path(config, path);
			uri = g_strconcat("sqlite://", resolved, NULL);
		}
	}

	if (NULL == uri)
		uri = g_strdup(configured);

	/*
	 * The password comes from the environment variable named by
	 * database.password_env, never from the configuration file. This is
	 * the whole point of that setting: a URI in a file that is in git
	 * must not carry a credential, and a URI in `podman inspect` output
	 * must not either.
	 *
	 * A password already present in the URI wins, so someone who
	 * deliberately spelled one out is not overridden by a stray variable
	 * in their environment.
	 */
	if (!g_str_has_prefix(uri, "sqlite://"))
	{
		g_autofree gchar *with_password = NULL;

		with_password = venture_database_uri_apply_password(uri, config);

		if (NULL != with_password)
		{
			g_free(uri);
			uri = g_steal_pointer(&with_password);
		}
	}

	return g_steal_pointer(&uri);
}

VentureDatabase *
venture_database_new_for_config(
	VentureConfig	 *config,
	GError		**error
){
	g_autofree gchar *uri = NULL;

	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);

	uri = venture_database_build_uri(config);

	return venture_database_new(uri, error);
}

void
venture_database_add_save_validator(
	VentureDatabase		*self,
	GType			 entity_type,
	VentureSaveValidator	 validate,
	gpointer		 user_data,
	GDestroyNotify		 destroy
){
	VentureDatabaseValidator *validator;

	g_return_if_fail(VENTURE_IS_DATABASE(self));
	g_return_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY));
	g_return_if_fail(NULL != validate);

	validator = g_new0(VentureDatabaseValidator, 1);
	validator->entity_type = entity_type;
	validator->validate = validate;
	validator->user_data = user_data;
	validator->destroy = destroy;

	g_ptr_array_add(self->validators, validator);
}

VentureDatabaseBackend
venture_database_get_backend(VentureDatabase *self)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(self),
	                     VENTURE_DATABASE_BACKEND_SQLITE);

	return self->backend;
}

OrmConnection *
venture_database_get_connection(VentureDatabase *self)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(self), NULL);

	return self->connection;
}

static OrmDialectType
venture_database_dialect(VentureDatabase *self)
{
	return (VENTURE_DATABASE_BACKEND_POSTGRES == self->backend)
		? ORM_DIALECT_POSTGRES : ORM_DIALECT_SQLITE;
}

/* --- Raw access ---------------------------------------------------------- */

gboolean
venture_database_execute(
	VentureDatabase	 *self,
	const gchar	 *sql,
	GList		 *params,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;
	gboolean ok;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);
	g_return_val_if_fail(NULL != sql, FALSE);

	g_rec_mutex_lock(&self->lock);

	/* A failed nested post rolled back the outer transaction too. Never let
	 * its caller's remaining writes escape into implicit autocommit. */
	if (self->transaction_depth > 0 && NULL == self->transaction)
	{
		g_rec_mutex_unlock(&self->lock);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
			"The transaction was already rolled back");
		return FALSE;
	}

	ok = (NULL != params)
		? orm_connection_execute_with_params(self->connection, sql,
		                                     params, &local_error)
		: orm_connection_execute(self->connection, sql, &local_error);

	g_rec_mutex_unlock(&self->lock);

	if (!ok)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		            "%s", (NULL != local_error) ? local_error->message
		                                        : "statement failed");
		return FALSE;
	}

	return TRUE;
}

OrmResult *
venture_database_query_raw(
	VentureDatabase	 *self,
	const gchar	 *sql,
	GList		 *params,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;
	OrmResult *result;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), NULL);
	g_return_val_if_fail(NULL != sql, NULL);

	g_rec_mutex_lock(&self->lock);

	/* PostgreSQL inserts use RETURNING through this path as well. */
	if (self->transaction_depth > 0 && NULL == self->transaction)
	{
		g_rec_mutex_unlock(&self->lock);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
			"The transaction was already rolled back");
		return NULL;
	}

	result = (NULL != params)
		? orm_connection_query_with_params(self->connection, sql, params,
		                                   &local_error)
		: orm_connection_query(self->connection, sql, &local_error);

	g_rec_mutex_unlock(&self->lock);

	if (NULL == result)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		            "%s", (NULL != local_error) ? local_error->message
		                                        : "query failed");
		return NULL;
	}

	return result;
}

/* --- Transactions -------------------------------------------------------- */

gboolean
venture_database_begin(
	VentureDatabase	 *self,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);

	g_rec_mutex_lock(&self->lock);

	if (self->transaction_depth > 0 && NULL == self->transaction)
	{
		g_rec_mutex_unlock(&self->lock);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
			"The transaction was already rolled back");
		return FALSE;
	}

	self->transaction_depth++;

	if (self->transaction_depth > 1)
	{
		/* Nested transactions are not attempted: a nested begin joins
		 * the one already running so the outermost commit stays in
		 * charge. The lock taken above is released by this begin's own
		 * matching commit or rollback, which is what keeps the
		 * recursive count balanced. */
		return TRUE;
	}

	self->transaction_owner = g_thread_self();

	self->transaction = orm_connection_begin_transaction(self->connection,
	                                                     &local_error);

	if (NULL == self->transaction)
	{
		self->transaction_depth--;
		self->transaction_owner = NULL;
		g_rec_mutex_unlock(&self->lock);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		            "Cannot begin a transaction: %s",
		            (NULL != local_error) ? local_error->message : "failed");
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_database_begin_serializable(VentureDatabase *self, GError **error)
{
	g_rec_mutex_lock(&self->lock);
	if (self->transaction_depth != 0)
	{
		g_rec_mutex_unlock(&self->lock);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Accounting approval must begin before the enclosing business transaction");
		return FALSE;
	}
	self->transaction = orm_connection_begin_transaction_with_isolation(self->connection,
		ORM_ISOLATION_SERIALIZABLE, error);
	if (self->transaction == NULL)
	{
		g_rec_mutex_unlock(&self->lock);
		return FALSE;
	}
	self->transaction_depth = 1;
	self->transaction_owner = g_thread_self();
	return TRUE;
}

gboolean
venture_database_commit(
	VentureDatabase	 *self,
	GError		**error
){
	g_autoptr(GError) local_error = NULL;
	gboolean ok;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);

	/* A commit with no begin of its own does nothing. The owner check is
	 * what makes that safe rather than merely quiet: another thread may
	 * legitimately hold a transaction, and releasing a level on its
	 * behalf would unlock a mutex this thread never took. */
	if ((0 == self->transaction_depth) ||
	    (g_thread_self() != self->transaction_owner))
		return TRUE;

	self->transaction_depth--;

	if (self->transaction_depth > 0)
	{
		/* An inner commit. Only the outermost one decides. */
		g_rec_mutex_unlock(&self->lock);
		return TRUE;
	}

	/*
	 * An inner level already rolled back, so there is nothing left to
	 * commit and reporting success would be a lie: the caller would
	 * carry on believing its writes landed. Once any level has rolled
	 * back, the outermost commit fails.
	 */
	if (NULL == self->transaction)
	{
		self->transaction_owner = NULL;
		g_rec_mutex_unlock(&self->lock);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		                    "The transaction was already rolled back");
		return FALSE;
	}

	ok = orm_transaction_commit(self->transaction, &local_error);
	g_clear_object(&self->transaction);
	self->transaction_owner = NULL;
	g_rec_mutex_unlock(&self->lock);

	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self), NULL);
		g_signal_emit(self, venture_database_signals[SIGNAL_TRANSACTION_FINISHED], 0, ok);
	}

	if (!ok)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		            "Cannot commit: %s",
		            (NULL != local_error) ? local_error->message : "failed");
		return FALSE;
	}

	return TRUE;
}

void
venture_database_rollback(VentureDatabase *self)
{
	g_return_if_fail(VENTURE_IS_DATABASE(self));

	if ((0 == self->transaction_depth) ||
	    (g_thread_self() != self->transaction_owner))
		return;

	self->transaction_depth--;

	/*
	 * An inner rollback abandons the whole transaction rather than just
	 * its own level -- a nested begin joined the outer one, so there is
	 * nothing smaller to undo. The outer commit then finds no
	 * transaction and returns without committing, which is the point:
	 * once any level has rolled back, the outermost must not succeed.
	 */
	if (NULL != self->transaction)
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self), NULL);
		orm_transaction_rollback(self->transaction, NULL);
		g_clear_object(&self->transaction);
		g_signal_emit(self, venture_database_signals[SIGNAL_TRANSACTION_FINISHED], 0, FALSE);
	}

	if (0 == self->transaction_depth)
		self->transaction_owner = NULL;

	g_rec_mutex_unlock(&self->lock);
}

/* --- Audit --------------------------------------------------------------- */

/*
 * Writes the audit record for a change.
 *
 * Audit failures are logged but never propagated: refusing a legitimate save
 * because the audit insert failed would be the wrong trade. The signal fires
 * regardless so an automation still sees the change.
 */
static void
venture_database_record_audit(
	VentureDatabase		*self,
	VentureAuditAction	 action,
	VentureEntity		*target,
	JsonNode		*diff,
	const VentureActor	*actor
){
	g_autoptr(VentureAuditEntry) entry = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(GError) local_error = NULL;

	/* An audit record about an audit record would recurse forever. */
	if (VENTURE_IS_AUDIT_ENTRY(target))
		return;

	entry = venture_audit_entry_new_for_change(action,
		(NULL != actor) ? actor->kind : VENTURE_ACTOR_KIND_SYSTEM,
		(NULL != actor) ? actor->name : NULL,
		target, diff);

	if ((NULL != actor) && (NULL != actor->prompt))
		g_object_set(entry, "prompt", actor->prompt, NULL);

	if ((NULL != actor) && (NULL != actor->request_id))
		g_object_set(entry, "request-id", actor->request_id, NULL);

	/* Only a change that came out of the confirmation queue has one, so
	 * its presence is what separates a staged-then-approved write from a
	 * direct one without anybody having to read the actor kind and guess. */
	if ((NULL != actor) && (NULL != actor->approved_by))
		g_object_set(entry, "approved-by", actor->approved_by, NULL);

	g_object_set(entry, "source", "database", NULL);

	internal = venture_access_policy_enter(venture_database_get_access_policy(self), NULL);
	if (!venture_database_save(self, VENTURE_ENTITY(entry), NULL,
	                           &local_error))
	{
		g_warning("Cannot record an audit entry for %s: %s",
		          venture_entity_get_entity_name(target),
		          local_error->message);
		return;
	}

	/* Notifications, outbound webhooks and automation are trusted service
	 * reactions to an already-authorized write, including other recipients. */
	venture_accounting_operation_suspend(self);
	g_signal_emit(self, venture_database_signals[SIGNAL_AUDIT], 0, entry);
	venture_accounting_operation_resume(self);
}

/* --- Writing ------------------------------------------------------------- */

static gboolean
venture_database_insert(
	VentureDatabase	 *self,
	VentureEntity	 *entity,
	GError		**error
){
	g_autoptr(GPtrArray) columns = NULL;
	g_autoptr(GString) sql = NULL;
	g_autofree gchar *table = NULL;
	GList *values = NULL;
	OrmDialectType dialect;
	gboolean ok;
	guint i;

	dialect = venture_database_dialect(self);
	venture_schema_bind_entity(entity, &columns, &values, FALSE);

	table = venture_schema_quote_identifier(
		venture_entity_get_table_name(entity));

	sql = g_string_new(NULL);
	g_string_append_printf(sql, "INSERT INTO %s (", table);

	for (i = 0; i < columns->len; i++)
	{
		g_autofree gchar *quoted = NULL;

		if (i > 0)
			g_string_append(sql, ", ");

		quoted = venture_schema_quote_identifier(
			g_ptr_array_index(columns, i));
		g_string_append(sql, quoted);
	}

	g_string_append(sql, ") VALUES (");

	for (i = 0; i < columns->len; i++)
	{
		if (i > 0)
			g_string_append(sql, ", ");

		if (ORM_DIALECT_POSTGRES == dialect)
			g_string_append_printf(sql, "$%u", i + 1);
		else
			g_string_append_c(sql, '?');
	}

	g_string_append_c(sql, ')');

	/* PostgreSQL will not report a rowid, so the generated key is asked
	 * for as part of the statement. */
	if (ORM_DIALECT_POSTGRES == dialect)
		g_string_append(sql, " RETURNING \"id\"");

	if (ORM_DIALECT_POSTGRES == dialect)
	{
		g_autoptr(OrmResult) result = NULL;

		result = venture_database_query_raw(self, sql->str, values, error);

		if (NULL != result)
		{
			if (orm_result_next(result))
			{
				venture_entity_set_id(entity,
					orm_row_get_integer(orm_result_get_row(result), 0));
			}

			ok = TRUE;
		}
		else
		{
			ok = FALSE;
		}
	}
	else
	{
		ok = venture_database_execute(self, sql->str, values, error);

		if (ok)
		{
			venture_entity_set_id(entity,
				orm_connection_get_last_insert_rowid(self->connection));
		}
	}

	g_list_free_full(values, (GDestroyNotify)orm_value_free);

	return ok;
}

static gboolean
venture_database_update(
	VentureDatabase	 *self,
	VentureEntity	 *entity,
	gint64		  expected_version,
	GError		**error
){
	g_autoptr(GPtrArray) columns = NULL;
	g_autoptr(GString) sql = NULL;
	g_autofree gchar *table = NULL;
	GList *values = NULL;
	OrmDialectType dialect;
	gboolean ok;
	gint changes;
	guint i;
	guint placeholder;

	dialect = venture_database_dialect(self);
	venture_schema_bind_entity(entity, &columns, &values, FALSE);

	table = venture_schema_quote_identifier(
		venture_entity_get_table_name(entity));

	sql = g_string_new(NULL);
	g_string_append_printf(sql, "UPDATE %s SET ", table);

	placeholder = 1;

	for (i = 0; i < columns->len; i++)
	{
		g_autofree gchar *quoted = NULL;

		if (i > 0)
			g_string_append(sql, ", ");

		quoted = venture_schema_quote_identifier(
			g_ptr_array_index(columns, i));

		if (ORM_DIALECT_POSTGRES == dialect)
			g_string_append_printf(sql, "%s = $%u", quoted, placeholder);
		else
			g_string_append_printf(sql, "%s = ?", quoted);

		placeholder++;
	}

	/*
	 * The WHERE clause carries the version the caller last saw. If
	 * another writer has changed the row since, no row matches and the
	 * update affects nothing -- which is how a conflict is detected
	 * without locking the row for the duration of an edit.
	 */
	if (ORM_DIALECT_POSTGRES == dialect)
	{
		g_string_append_printf(sql, " WHERE \"id\" = $%u AND \"version\" = $%u",
		                       placeholder, placeholder + 1);
	}
	else
	{
		g_string_append(sql, " WHERE \"id\" = ? AND \"version\" = ?");
	}

	values = g_list_append(values,
		orm_value_new_integer(venture_entity_get_id(entity)));
	values = g_list_append(values, orm_value_new_integer(expected_version));

	ok = venture_database_execute(self, sql->str, values, error);
	changes = orm_connection_get_changes(self->connection);

	g_list_free_full(values, (GDestroyNotify)orm_value_free);

	if (!ok)
		return FALSE;

	if (0 == changes)
	{
		g_autofree gchar *label = NULL;

		label = venture_entity_get_display_name(entity);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "%s was changed by someone else since you loaded it. "
		            "Reload and reapply your change.", label);
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_database_snapshot_insert(VentureDatabase *self, VentureEntity *record, GError **error)
{
	g_return_val_if_fail(self->transaction_depth > 0 && self->transaction_owner == g_thread_self(), FALSE);
	return venture_database_insert(self, record, error);
}

gboolean
venture_database_snapshot_update(VentureDatabase *self, VentureEntity *record, gint64 expected_version, GError **error)
{
	g_return_val_if_fail(self->transaction_depth > 0 && self->transaction_owner == g_thread_self(), FALSE);
	return venture_database_update(self, record, expected_version, error);
}

/*
 * Refuses a save whose reference fields would point at rows that are not
 * there. The declaration in the field table is the contract; without this,
 * `create expense venture_id=99999` succeeds and the dangling row surfaces
 * later as a report that quietly misses it.
 *
 * Only references being *written* are checked -- on an update, a field that
 * still holds the value it held before is left alone. A record that has
 * long pointed at a since-deleted venture can still have its notes
 * corrected; what is refused is writing a dangling pointer, not keeping an
 * old one.
 */
static gboolean
venture_database_check_references(
	VentureDatabase	 *self,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	GError		**error
){
	VentureEntityClass *klass;
	g_autofree GParamSpec **properties = NULL;
	guint n_properties;
	guint i;

	klass = VENTURE_ENTITY_GET_CLASS(entity);
	properties = venture_entity_class_list_persistent_properties(
		klass, &n_properties);

	for (i = 0; i < n_properties; i++)
	{
		g_autoptr(VentureEntity) target = NULL;
		g_autofree gchar *column = NULL;
		GParamSpec *pspec;
		const gchar *target_name;
		GType target_type;
		gint64 target_id;

		pspec = properties[i];
		target_name = venture_entity_class_get_reference(klass,
		                                                 pspec->name);

		if (NULL == target_name)
			continue;

		if (G_TYPE_INT64 != G_PARAM_SPEC_VALUE_TYPE(pspec))
			continue;

		target_id = 0;
		g_object_get(entity, pspec->name, &target_id, NULL);

		/* An empty reference is not a dangling one; whether the field
		 * may be empty is the NOT_NULL flag's question. */
		if (0 == target_id)
			continue;

		if (NULL != previous)
		{
			gint64 previous_id;

			previous_id = 0;
			g_object_get(previous, pspec->name, &previous_id, NULL);

			if (previous_id == target_id)
				continue;
		}

		target_type = venture_entity_registry_lookup(
			venture_entity_registry_get_default(), target_name);

		if (G_TYPE_INVALID == target_type)
		{
			/*
			 * Registered but hidden means the target's module is off,
			 * and a reference written now cannot be checked -- so it
			 * is refused, with the switch named. A field keeping the
			 * value it already had was skipped above, which is what
			 * keeps a record editable after the module it points into
			 * is turned off.
			 */
			if (!venture_entity_registry_is_type_enabled(
				venture_entity_registry_get_default(), target_name) &&
			    (G_TYPE_INVALID != venture_entity_registry_lookup_any(
				venture_entity_registry_get_default(), target_name)))
			{
				g_autoptr(GError) why = NULL;

				venture_entity_registry_set_unknown_type_error(
					venture_entity_registry_get_default(), target_name,
					&why);
				column = venture_entity_property_to_column(pspec->name);
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "%s.%s cannot be set: %s",
				            venture_entity_get_entity_name(entity), column,
				            why->message);
				return FALSE;
			}

			/* A reference declared against a name nothing registered
			 * is a bug in the declaration, not in the record being
			 * saved. */
			continue;
		}

		column = venture_entity_property_to_column(pspec->name);
		target = venture_database_get(self, target_type, target_id, NULL);

		if ((NULL == target) || venture_entity_is_deleted(target))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s.%s points at %s #%" G_GINT64_FORMAT
			            ", which %s",
			            venture_entity_get_entity_name(entity), column,
			            target_name, target_id,
			            (NULL == target) ? "does not exist"
			                             : "has been deleted");
			return FALSE;
		}
	}

	return TRUE;
}

/* Continue a service-wrapped write after the dispatch hooks.  This callback is
 * private so callers cannot bypass a service's single-use authorization. */
static gboolean database_save_unwrapped(VentureDatabase *self, VentureEntity *entity,
	const VentureActor *actor, GError **error);

/* Every lifecycle entry point uses the same subsystem guard set. Each
 * guard owns its record types; adding a feature must not leave restore or
 * purge as an unguarded alternate writer. */
static gboolean
check_subsystem_write(VentureDatabase *self, VentureEntity *entity, gboolean removal, GError **error)
{
	typedef gboolean (*WriteGuard)(VentureDatabase *, VentureEntity *, gboolean, GError **);
	static const WriteGuard guards[] = {
		venture_bank_check_write,
		venture_cutover_check_write,
		venture_setup_check_write,
		venture_progress_check_write,
		venture_portal_check_write,
		venture_supplier_portal_check_write,
		venture_backup_check_write,
		venture_tax_filing_check_write,
		venture_accounting_approval_check_write,
		venture_claims_check_write,
		venture_payroll_check_write,
		venture_goods_check_write,
		venture_budget_check_write,
		venture_equity_check_write,
		venture_group_check_write,
		venture_dunning_check_write,
		venture_sales_tax_check_write,
		venture_backup_schedule_check_write,
		venture_crm_import_check_write,
		venture_integration_check_write,
		venture_projects_check_write
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(guards); i++)
		if (!guards[i](self, entity, removal, error))
			return FALSE;
	return TRUE;
}

static gboolean
database_save_dispatch(
	VentureDatabase		 *self,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(entity), FALSE);
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self), entity, "write", error)) return FALSE;
	if (!venture_orgaccess_prepare(self, entity, error)) return FALSE;

	if (stripe_owned(entity))
	{
		if (self->stripe_write_permit != entity)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Stripe evidence must be written through VentureStripeService");
			return FALSE;
		}
		self->stripe_write_permit = NULL;
	}
	{
		gboolean handled;
		gboolean ok = venture_payables_expense_hook(self, entity, actor, &handled, error);
		if (!ok || handled)
			return ok;
	}
	if (!check_subsystem_write(self, entity, FALSE, error))
		return FALSE;

	VENTURE_AUTOJOURNAL_SAVE_HOOK(self, entity, actor, error);
	{
		gboolean handled = FALSE;
		gboolean ok = venture_goods_save_hook(self, entity, actor, &handled, error);
		if (handled || !ok)
			return ok;
	}
	/* Source and posting share a transaction, whichever surface saved it. */
	{
		gboolean handled = FALSE;
		gboolean ok = venture_assets_save(self, entity, actor, &handled, error);
		if (handled || !ok)
			return ok;
	}
	if (!venture_payables_is_projection_write(self, entity) && venture_ledger_wrap_source(self, entity))
		return venture_ledger_save_source(self, entity, actor, error);
	{
		gboolean handled;
		gboolean ok = venture_periods_save(self, entity, actor, &handled, error);
		if (handled || !ok)
			return ok;
	}
	{
		gboolean handled;
		gboolean ok = venture_close_save_hook(self, entity, actor, &handled, error);
		if (handled || !ok)
			return ok;
	}
	{
		gboolean handled;
		gboolean ok = venture_tax_filing_save_hook(self, entity, actor, &handled, error);
		if (handled || !ok)
			return ok;
	}
	{
		gboolean handled;
		gboolean ok = venture_capture_save_hook(self, entity, actor, &handled, error);
		if (handled || !ok)
			return ok;
	}
	{
		gboolean handled;
		gboolean ok = venture_claims_save_hook(self, entity, actor, &handled, error);
		if (handled || !ok)
			return ok;
	}
	{
		gboolean handled;
		gboolean ok = venture_payroll_save_hook(self, entity, actor, &handled, error);
		if (handled || !ok)
			return ok;
	}

	if (VENTURE_IS_MAIL_MESSAGE(entity)) venture_database_get_mail_outbox(self);
	if (VENTURE_IS_USER(entity)) {
		gboolean handled;
		gboolean ok = venture_mail_save_user(venture_database_get_mail_outbox(self), entity, actor, &handled, error);
		if (handled || !ok) return ok;
	}

	{
		gboolean handled;
		gboolean ok = venture_lead_service_save_hook(venture_database_get_lead_service(self), entity, actor, &handled, error);
		if (handled || !ok) return ok;
	}

	{
		gboolean handled;
		gboolean ok = venture_pipelines_save(self, entity, actor, &handled, error);
		if (handled || !ok)
			return ok;
		ok = venture_sequences_save_hook(self, entity, actor, database_save_unwrapped, &handled, error);
		if (handled || !ok)
			return ok;
	}

	return database_save_unwrapped(self, entity, actor, error);
}

/* Only hooks that can post need a whole-operation boundary here. Draft
 * edits remain ordinary writes; they invalidate consent through the snapshot.
 * AR/AP and other services establish their own scopes at their handled branch. */
static gboolean
accounting_save_needs_scope(VentureDatabase *database, VentureEntity *entity)
{
	g_autofree gchar *operation = NULL;
	if (VENTURE_IS_SALE(entity))
	{
		g_autoptr(VentureMoney) gross = NULL;
		g_autoptr(VentureMoney) refunded = NULL;
		gint64 product_id = 0;
		g_object_get(entity, "gross", &gross, "refunded", &refunded, "product-id", &product_id, NULL);
		/* Product-backed sales may issue inventory before their ordinary save;
		 * a refund cleared to zero may reverse an existing refund journal. */
		return gross != NULL || refunded != NULL || product_id > 0;
	}
	if (VENTURE_IS_EXPENSE(entity))
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_object_get(entity, "amount", &amount, NULL);
		return amount != NULL;
	}
	if (VENTURE_IS_DEFERRAL(entity))
	{
		if (!venture_entity_is_persisted(entity))
			return TRUE;
		g_object_get(entity, "operation", &operation, NULL);
		return g_strcmp0(operation, "settle") == 0;
	}
	if (VENTURE_IS_FIXED_ASSET(entity))
	{
		g_object_get(entity, "operation", &operation, NULL);
		return g_strcmp0(operation, "place") == 0 || g_strcmp0(operation, "dispose") == 0 ||
			g_strcmp0(operation, "write-off") == 0 || (operation != NULL && g_str_has_prefix(operation, "run-period:"));
	}
	if (VENTURE_IS_QUOTE_ACTION(entity))
	{
		g_autoptr(VentureEntity) quote = NULL;
		g_autofree gchar *mode = NULL;
		gint64 quote_id = 0;
		g_object_get(entity, "action", &operation, "quote-id", &quote_id, NULL);
		if (g_strcmp0(operation, "accept") != 0)
			return FALSE;
		quote = venture_database_get(database, VENTURE_TYPE_QUOTE, quote_id, NULL);
		/* A missing/unreadable source is refused by the service; never infer
		 * the nonposting progress branch from a failed lookup. */
		if (quote == NULL)
			return TRUE;
		g_object_get(quote, "billing-mode", &mode, NULL);
		return g_strcmp0(mode, "progress") != 0;
	}
	return FALSE;
}

/* The generic writer is itself a business command: source hooks can generate
 * postings before the ordinary row save. Scope it before dispatching any hook. */
gboolean
venture_database_save(VentureDatabase *self, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	gboolean ok;
	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(entity), FALSE);
	/* A failed child poisons the enclosing transaction. SQL after its
	 * rollback would otherwise run in autocommit and escape the operation. */
	g_rec_mutex_lock(&self->lock);
	if (self->transaction_depth != 0 && self->transaction == NULL)
	{
		g_rec_mutex_unlock(&self->lock);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "The transaction was already rolled back");
		return FALSE;
	}
	g_rec_mutex_unlock(&self->lock);
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self), entity, "write", error) ||
		!venture_orgaccess_prepare(self, entity, error))
		return FALSE;
	if (!venture_accounting_operation_guard_write(self, entity, actor, error))
		return FALSE;
	if (accounting_save_needs_scope(self, entity))
	{
		operation = venture_accounting_operation_begin(self, "record.save", entity, NULL, NULL,
			venture_entity_get_organization_id(entity), actor, error);
		if (operation == NULL)
			return FALSE;
	}
	ok = database_save_dispatch(self, entity, actor, error);
	return ok && (operation == NULL || venture_accounting_operation_finish(operation, error));
}

static gboolean
database_save_unwrapped(VentureDatabase *self, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(JsonNode) diff = NULL;
	gint64 expected_version;
	gboolean created;
	gboolean ledger_authorized;
	gboolean settlement_authorized = FALSE;

	/* A service continuation can refine the record after the public entry
	 * check. Recheck that exact proposed organization and ownership before
	 * validation and persistence, without rerunning one-use service hooks. */
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self), entity, "write", error)) return FALSE;
	if (!venture_orgaccess_prepare(self, entity, error)) return FALSE;

	/* Validation happens before anything is written, never after: a
	 * half-written invalid record is worse than a rejected one. */
	if (!venture_entity_validate(entity, error))
		return FALSE;
	if (!venture_custom_fields_validate(self, entity, error))
		return FALSE;

	if (!venture_entity_before_save(entity, error))
		return FALSE;

	{
		gboolean handled;
		gboolean ok = venture_quotes_save_hook(self, entity, actor, &handled, error);
		if (handled || !ok) return ok;
	}

	created = !venture_entity_is_persisted(entity);

	g_rec_mutex_lock(&self->lock);

	/* Before the empty-diff fast path: identity edits must not bypass this. */
	if (!venture_ledger_check_write(self, entity, NULL, FALSE, &ledger_authorized, error))
	{
		g_rec_mutex_unlock(&self->lock);
		return FALSE;
	}
	/* Financial records dispatch before the ordinary write, so receipts,
	 * invoice state and ledger batches share one encompassing transaction. */
	{
		gboolean handled;
		gboolean ok;

		ok = venture_receivables_save_hook(self, entity, actor, &handled, &settlement_authorized, error);
		if (!ok || handled)
		{
			g_rec_mutex_unlock(&self->lock);
			return ok;
		}
	}

	{
		gboolean handled;
		gboolean ok = venture_billing_save_hook(self, entity, actor, &handled, error);
		if (!ok || handled)
		{
			g_rec_mutex_unlock(&self->lock);
			return ok;
		}
	}

	{
		gboolean handled;
		gboolean ok = venture_projects_save_hook(self, entity, actor, &handled, error);
		if (!ok || handled)
		{
			g_rec_mutex_unlock(&self->lock);
			return ok;
		}
	}

	{
		gboolean handled;
		gboolean authorized;
		gboolean ok = venture_payables_save_hook(self, entity, actor, &handled, &authorized, error);
		settlement_authorized = settlement_authorized || authorized;
		if (!ok || handled)
		{
			g_rec_mutex_unlock(&self->lock);
			return ok;
		}
	}

	if (!created)
	{
		/* Fetch the stored row first, both to detect a conflict and to
		 * produce a diff for the audit trail. */
		previous = venture_database_get(self, G_OBJECT_TYPE(entity),
		                                venture_entity_get_id(entity), NULL);

		if (NULL == previous)
		{
			g_rec_mutex_unlock(&self->lock);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "%s #%" G_GINT64_FORMAT " no longer exists",
			            venture_entity_get_entity_name(entity),
			            venture_entity_get_id(entity));
			return FALSE;
		}

		diff = venture_entity_diff(previous, entity);

		/* Nothing actually changed, so there is nothing to write and
		 * nothing worth auditing. */
		if (0 == json_object_get_size(json_node_get_object(diff)))
		{
			g_rec_mutex_unlock(&self->lock);
			return TRUE;
		}
	}

	/* Inside the lock, so the row a reference was checked against cannot
	 * vanish before the write that relies on it. */
	if (!venture_database_check_references(self, entity, previous, error))
	{
		g_rec_mutex_unlock(&self->lock);
		return FALSE;
	}

	/* Likewise the validators, for the same reason. */
	if (!settlement_authorized && !venture_periods_validate_financial(self, entity, previous, error))
	{
		g_rec_mutex_unlock(&self->lock);
		return FALSE;
	}

	{
		guint v;

		for (v = 0; v < self->validators->len; v++)
		{
			VentureDatabaseValidator *validator;

			validator = g_ptr_array_index(self->validators, v);

			if (!g_type_is_a(G_OBJECT_TYPE(entity), validator->entity_type))
				continue;

			if (!validator->validate(self, entity, previous,
			                         validator->user_data, error))
			{
				g_rec_mutex_unlock(&self->lock);
				return FALSE;
			}
		}
	}

	/* Validators may fill ordinary defaults, but cannot grant a generic
	 * writer the service's authority to post or move protected lines. */
	if (!ledger_authorized &&
		!venture_ledger_check_write(self, entity, previous, FALSE, NULL, error))
	{
		g_rec_mutex_unlock(&self->lock);
		return FALSE;
	}

	expected_version = venture_entity_get_version(entity);
	venture_entity_touch(entity);

	{
		gboolean opened = FALSE;
		if (self->transaction_depth == 0)
		{
			if (!venture_database_begin(self, error))
			{
				g_rec_mutex_unlock(&self->lock);
				return FALSE;
			}
			opened = TRUE;
		}
		if (created)
		{
			if (!venture_database_insert(self, entity, error))
			{
				if (opened)
					venture_database_rollback(self);
				g_rec_mutex_unlock(&self->lock);
				return FALSE;
			}
		}
		else if (!venture_database_update(self, entity, expected_version, error))
		{
			if (opened)
				venture_database_rollback(self);
			g_rec_mutex_unlock(&self->lock);
			return FALSE;
		}
		if (!venture_custom_fields_sync(self, entity, actor, error))
		{
			if (opened)
				venture_database_rollback(self);
			g_rec_mutex_unlock(&self->lock);
			return FALSE;
		}
		if (opened && !venture_database_commit(self, error))
		{
			g_rec_mutex_unlock(&self->lock);
			return FALSE;
		}
	}

	g_rec_mutex_unlock(&self->lock);

	venture_database_record_audit(self,
		created ? VENTURE_AUDIT_ACTION_CREATE : VENTURE_AUDIT_ACTION_UPDATE,
		entity, diff, actor);

	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self), NULL);
		venture_accounting_operation_suspend(self);
		g_signal_emit(self, venture_database_signals[SIGNAL_ENTITY_SAVED], 0,
		              entity, created);
		venture_accounting_operation_resume(self);
	}
	return TRUE;
}

/* --- Reading ------------------------------------------------------------- */

/*
 * Runs a statement and materialises the rows as records.
 */
static GPtrArray *
venture_database_fetch(
	VentureDatabase	 *self,
	GType		  entity_type,
	const gchar	 *sql,
	GList		 *params,
	GError		**error
){
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GPtrArray) entities = NULL;

	result = venture_database_query_raw(self, sql, params, error);

	if (NULL == result)
		return NULL;

	entities = g_ptr_array_new_with_free_func(g_object_unref);

	while (orm_result_next(result))
	{
		VentureEntity *entity;

		entity = g_object_new(entity_type, NULL);
		venture_schema_populate_entity(entity, orm_result_get_row(result));
		g_ptr_array_add(entities, entity);
	}

	return g_steal_pointer(&entities);
}

VentureEntity *
venture_database_get(
	VentureDatabase	 *self,
	GType		  entity_type,
	gint64		  id,
	GError		**error
){
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GPtrArray) entities = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *table = NULL;
	GList *params = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), NULL);
	g_return_val_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY), NULL);

	prototype = g_object_new(entity_type, NULL);
	table = venture_schema_quote_identifier(
		venture_entity_get_table_name(prototype));

	sql = g_strdup_printf("SELECT * FROM %s WHERE \"id\" = %s", table,
		(VENTURE_DATABASE_BACKEND_POSTGRES == self->backend) ? "$1" : "?");

	params = g_list_append(params, orm_value_new_integer(id));
	entities = venture_database_fetch(self, entity_type, sql, params, error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);

	if ((NULL == entities) || (0 == entities->len))
		return NULL;

	if (!venture_access_policy_check_read(venture_database_get_access_policy(self), g_ptr_array_index(entities, 0), error)) return NULL;
	return g_object_ref(g_ptr_array_index(entities, 0));
}

VentureEntity *
venture_database_get_by_uuid(
	VentureDatabase	 *self,
	GType		  entity_type,
	const gchar	 *uuid,
	GError		**error
){
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GPtrArray) entities = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *table = NULL;
	GList *params = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), NULL);
	g_return_val_if_fail(NULL != uuid, NULL);

	prototype = g_object_new(entity_type, NULL);
	table = venture_schema_quote_identifier(
		venture_entity_get_table_name(prototype));

	sql = g_strdup_printf("SELECT * FROM %s WHERE \"uuid\" = %s", table,
		(VENTURE_DATABASE_BACKEND_POSTGRES == self->backend) ? "$1" : "?");

	params = g_list_append(params, orm_value_new_string(uuid));
	entities = venture_database_fetch(self, entity_type, sql, params, error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);

	if ((NULL == entities) || (0 == entities->len))
		return NULL;

	if (!venture_access_policy_check_read(venture_database_get_access_policy(self), g_ptr_array_index(entities, 0), error)) return NULL;
	return g_object_ref(g_ptr_array_index(entities, 0));
}

GPtrArray *
venture_database_find(
	VentureDatabase	 *self,
	VentureQuery	 *query,
	GError		**error
){
	g_autofree gchar *sql = NULL;
	GPtrArray *entities;
	GList *params = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_QUERY(query), NULL);

	if (NULL != venture_access_policy_get_actor(venture_database_get_access_policy(self))) return venture_access_policy_find(self->access_policy, query, error);

	sql = venture_query_to_sql(query, venture_database_dialect(self), FALSE,
	                           &params);
	entities = venture_database_fetch(self,
		venture_query_get_entity_type(query), sql, params, error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);

	return entities;
}

VentureEntity *
venture_database_find_one(
	VentureDatabase	 *self,
	VentureQuery	 *query,
	GError		**error
){
	g_autoptr(GPtrArray) entities = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_QUERY(query), NULL);

	venture_query_set_limit(query, 1);
	entities = venture_database_find(self, query, error);

	if ((NULL == entities) || (0 == entities->len))
		return NULL;

	return g_object_ref(g_ptr_array_index(entities, 0));
}

gint64
venture_database_count(
	VentureDatabase	 *self,
	VentureQuery	 *query,
	GError		**error
){
	g_autoptr(OrmResult) result = NULL;
	g_autofree gchar *sql = NULL;
	GList *params = NULL;
	gint64 count;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), -1);
	g_return_val_if_fail(VENTURE_IS_QUERY(query), -1);

	if (NULL != venture_access_policy_get_actor(venture_database_get_access_policy(self))) return venture_access_policy_count(self->access_policy, query, error);

	sql = venture_query_to_sql(query, venture_database_dialect(self), TRUE,
	                           &params);
	result = venture_database_query_raw(self, sql, params, error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);

	if (NULL == result)
		return -1;

	count = orm_result_next(result)
		? orm_row_get_integer(orm_result_get_row(result), 0)
		: 0;

	return count;
}

/* --- Deleting ------------------------------------------------------------ */

gboolean
venture_database_delete(
	VentureDatabase		 *self,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GRecMutexLocker) ledger_lock = NULL;
	gint64 expected_version;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(entity), FALSE);
	if (!venture_accounting_operation_guard_write(self, entity, actor, error))
		return FALSE;
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self), entity, "delete", error)) return FALSE;
	if (!venture_pipelines_check_removal(entity, error))
		return FALSE;

	{
		gboolean handled;
		gboolean result = venture_quotes_remove_hook(self, entity, 0, actor, &handled, error);
		if (handled || !result) return result;
	}

	ledger_lock = g_rec_mutex_locker_new(&self->lock);
	if (!venture_ledger_check_write(self, entity, NULL, TRUE, NULL, error))
		return FALSE;
	if (stripe_owned(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Stripe evidence is retained by VentureStripeService");
		return FALSE;
	}
	if (!venture_billing_check_removal(self, entity, error))
		return FALSE;
	if (!check_subsystem_write(self, entity, TRUE, error) ||
		!venture_payables_check_removal(self, entity, error) ||
		!venture_receivables_check_removal(self, entity, error))
		return FALSE;
	if (!venture_sequences_check_removal(entity, error))
		return FALSE;
	if (!venture_periods_check_removal(self, entity, error))
		return FALSE;
	if (!venture_assets_check_removal(self, entity, error))
		return FALSE;
	if (!venture_mail_check_removal(entity, error))
		return FALSE;
	if (!venture_quotes_check_removal(self, entity, error))
		return FALSE;
	{
		gboolean handled;
		gboolean result = venture_pipelines_remove_hook(self, entity, 0, actor, &handled, error);
		if (handled || !result) return result;
	}

	if (!venture_entity_is_persisted(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "That record has never been saved");
		return FALSE;
	}

	if (venture_entity_is_deleted(entity))
		return TRUE;

	/*
	 * Soft deletion, always. An expense that appeared in a filed return
	 * has to stay reconstructable, and a report over a past period must
	 * still produce the number it produced then.
	 */
	now = venture_time_now();
	g_object_set(entity, "deleted-at", now, NULL);

	/* Bump the version before writing, not after: the UPDATE writes every
	 * column including this one, so touching afterwards would leave the
	 * object one ahead of the row and make the next save look like a
	 * conflict with itself. */
	expected_version = venture_entity_get_version(entity);
	venture_entity_touch(entity);

	if (!venture_database_update(self, entity, expected_version, error))
		return FALSE;

	venture_database_record_audit(self, VENTURE_AUDIT_ACTION_DELETE, entity,
	                              NULL, actor);

	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self), NULL);
		venture_accounting_operation_suspend(self);
		g_signal_emit(self, venture_database_signals[SIGNAL_ENTITY_DELETED], 0,
		              entity);
		venture_accounting_operation_resume(self);
	}

	return TRUE;
}


gboolean
venture_database_restore(
	VentureDatabase		 *self,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	gint64 expected_version;
	g_autoptr(GRecMutexLocker) ledger_lock = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(entity), FALSE);
	if (!venture_accounting_operation_guard_write(self, entity, actor, error))
		return FALSE;
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self), entity, "write", error)) return FALSE;
	if (!venture_pipelines_check_removal(entity, error))
		return FALSE;

	{
		gboolean handled;
		gboolean result = venture_quotes_remove_hook(self, entity, 1, actor, &handled, error);
		if (handled || !result) return result;
	}

	ledger_lock = g_rec_mutex_locker_new(&self->lock);
	if (!venture_ledger_check_write(self, entity, NULL, TRUE, NULL, error))
		return FALSE;
	if (stripe_owned(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Stripe evidence is retained by VentureStripeService");
		return FALSE;
	}
	if (!venture_billing_check_removal(self, entity, error))
		return FALSE;
	if (!check_subsystem_write(self, entity, TRUE, error) ||
		!venture_payables_check_removal(self, entity, error) ||
		!venture_receivables_check_removal(self, entity, error))
		return FALSE;
	if (!venture_sequences_check_removal(entity, error))
		return FALSE;
	if (!venture_periods_check_removal(self, entity, error))
		return FALSE;
	if (!venture_assets_check_removal(self, entity, error))
		return FALSE;
	if (!venture_mail_check_removal(entity, error))
		return FALSE;
	if (!venture_quotes_check_removal(self, entity, error))
		return FALSE;
	{
		gboolean handled;
		gboolean result = venture_pipelines_remove_hook(self, entity, 1, actor, &handled, error);
		if (handled || !result) return result;
	}

	if (!venture_entity_is_deleted(entity))
		return TRUE;

	g_object_set(entity, "deleted-at", NULL, NULL);

	expected_version = venture_entity_get_version(entity);
	venture_entity_touch(entity);

	if (!venture_database_update(self, entity, expected_version, error))
		return FALSE;

	venture_database_record_audit(self, VENTURE_AUDIT_ACTION_UPDATE, entity,
	                              NULL, actor);

	return TRUE;
}

gboolean
venture_database_purge(
	VentureDatabase		 *self,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	g_autofree gchar *sql = NULL;
	g_autofree gchar *table = NULL;
	GList *params = NULL;
	gboolean ok;
	g_autoptr(GRecMutexLocker) ledger_lock = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(entity), FALSE);
	if (!venture_accounting_operation_guard_write(self, entity, actor, error))
		return FALSE;
	if (!venture_access_policy_check_write(venture_database_get_access_policy(self), entity, "delete", error)) return FALSE;
	if (!venture_pipelines_check_removal(entity, error))
		return FALSE;

	{
		gboolean handled;
		gboolean result = venture_quotes_remove_hook(self, entity, 2, actor, &handled, error);
		if (handled || !result) return result;
	}

	ledger_lock = g_rec_mutex_locker_new(&self->lock);
	if (!venture_ledger_check_write(self, entity, NULL, TRUE, NULL, error))
		return FALSE;
	if (stripe_owned(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Stripe evidence is retained by VentureStripeService");
		return FALSE;
	}
	if (!venture_billing_check_removal(self, entity, error))
		return FALSE;
	if (!check_subsystem_write(self, entity, TRUE, error) ||
		!venture_payables_check_removal(self, entity, error) ||
		!venture_receivables_check_removal(self, entity, error))
		return FALSE;
	if (!venture_sequences_check_removal(entity, error))
		return FALSE;
	if (!venture_periods_check_removal(self, entity, error))
		return FALSE;
	if (!venture_assets_check_removal(self, entity, error))
		return FALSE;
	if (!venture_mail_check_removal(entity, error))
		return FALSE;
	if (!venture_quotes_check_removal(self, entity, error))
		return FALSE;
	{
		gboolean handled;
		gboolean result = venture_pipelines_remove_hook(self, entity, 2, actor, &handled, error);
		if (handled || !result) return result;
	}

	if (!venture_entity_is_persisted(entity))
		return TRUE;

	/* Audited before the row goes, since afterwards there is nothing left
	 * to describe. */
	venture_database_record_audit(self, VENTURE_AUDIT_ACTION_DELETE, entity,
	                              NULL, actor);

	table = venture_schema_quote_identifier(
		venture_entity_get_table_name(entity));
	sql = g_strdup_printf("DELETE FROM %s WHERE \"id\" = %s", table,
		(VENTURE_DATABASE_BACKEND_POSTGRES == self->backend) ? "$1" : "?");

	params = g_list_append(params,
		orm_value_new_integer(venture_entity_get_id(entity)));
	ok = venture_database_execute(self, sql, params, error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);

	if (ok)
		venture_entity_set_id(entity, 0);

	return ok;
}

/* --- Ledger -------------------------------------------------------------- */

gboolean
venture_database_save_ledger_transaction(
	VentureDatabase		 *self,
	GPtrArray		 *entries,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureMoney) debits = NULL;
	g_autoptr(VentureMoney) credits = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);
	g_return_val_if_fail(NULL != entries, FALSE);

	if (0 == entries->len)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A ledger transaction needs at least one line");
		return FALSE;
	}

	debits = venture_money_new_zero(NULL);
	credits = venture_money_new_zero(NULL);

	for (i = 0; i < entries->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) total = NULL;
		VentureEntity *entry;
		VentureLedgerSide side;

		entry = g_ptr_array_index(entries, i);

		if (!VENTURE_IS_LEDGER_ENTRY(entry))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A ledger transaction may only contain "
			                    "ledger entries");
			return FALSE;
		}

		g_object_get(entry, "amount", &amount, "side", &side, NULL);

		if (NULL == amount)
			continue;

		if (VENTURE_LEDGER_SIDE_DEBIT == side)
		{
			total = venture_money_add(debits, amount, error);

			if (NULL == total)
				return FALSE;

			g_clear_pointer(&debits, venture_money_free);
			debits = g_steal_pointer(&total);
		}
		else
		{
			total = venture_money_add(credits, amount, error);

			if (NULL == total)
				return FALSE;

			g_clear_pointer(&credits, venture_money_free);
			credits = g_steal_pointer(&total);
		}
	}

	/*
	 * Refuse the whole set rather than write part of it. A half-posted
	 * transaction leaves the books wrong in a way nothing downstream can
	 * detect, which is far worse than an error the operator can see.
	 */
	if (!venture_money_equal(debits, credits))
	{
		g_autofree gchar *debit_text = NULL;
		g_autofree gchar *credit_text = NULL;

		debit_text = venture_money_to_string(debits);
		credit_text = venture_money_to_string(credits);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_BALANCE,
		            "This transaction does not balance: debits total %s "
		            "but credits total %s", debit_text, credit_text);
		return FALSE;
	}

	/* Compatibility callers still cross the same journal posting boundary. */
	return venture_ledger_post_legacy(self, entries, actor, error);
}

/* --- Aggregation --------------------------------------------------------- */

VentureMoney *
venture_database_sum_money(
	VentureDatabase	 *self,
	VentureQuery	 *query,
	const gchar	 *field,
	GError		**error
){
	g_autoptr(OrmResult) result = NULL;
	g_autofree gchar *base_sql = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *amount_column = NULL;
	g_autofree gchar *currency_column = NULL;
	g_autofree gchar *column_base = NULL;
	const gchar *where;
	GList *params = NULL;
	VentureMoney *total = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_QUERY(query), NULL);
	g_return_val_if_fail(NULL != field, NULL);

	column_base = venture_entity_property_to_column(field);
	amount_column = g_strconcat(column_base,
		VENTURE_SCHEMA_MONEY_AMOUNT_SUFFIX, NULL);
	currency_column = g_strconcat(column_base,
		VENTURE_SCHEMA_MONEY_CURRENCY_SUFFIX, NULL);

	/* Reuse the query compiler for the WHERE clause so filters, scoping
	 * and soft deletion behave identically to a normal fetch, then splice
	 * the aggregate over the top. */
	base_sql = venture_query_to_sql(query, venture_database_dialect(self),
	                                TRUE, &params);
	where = strstr(base_sql, " WHERE ");

	{
		g_autoptr(VentureEntity) prototype = NULL;
		g_autofree gchar *table = NULL;
		g_autofree gchar *quoted_amount = NULL;
		g_autofree gchar *quoted_currency = NULL;

		prototype = g_object_new(venture_query_get_entity_type(query), NULL);
		table = venture_schema_quote_identifier(
			venture_entity_get_table_name(prototype));
		quoted_amount = venture_schema_quote_identifier(amount_column);
		quoted_currency = venture_schema_quote_identifier(currency_column);

		/*
		 * Grouped by currency and ordered by row count, so the first
		 * row is the currency most records are in. Adding across
		 * currencies without a rate would produce a confidently wrong
		 * number, so the minority ones are reported separately rather
		 * than folded in.
		 */
		/*
		 * The SUM is cast back to an integer explicitly.
		 *
		 * SQLite returns SUM(integer) as an integer, but PostgreSQL
		 * widens SUM(bigint) to numeric -- so the typed accessor below
		 * asserted on PostgreSQL while every SQLite run passed. Money
		 * is integer minor units on both, and the cast says so once
		 * rather than making the reader of every total wonder which
		 * backend produced it.
		 */
		sql = g_strdup_printf(
			"SELECT CAST(SUM(%s) AS BIGINT), %s, MAX(%s), COUNT(*) "
			"FROM %s%s GROUP BY %s ORDER BY COUNT(*) DESC",
			quoted_amount, quoted_currency,
			venture_schema_quote_identifier(
				g_strconcat(column_base,
				            VENTURE_SCHEMA_MONEY_EXPONENT_SUFFIX,
				            NULL)),
			table,
			(NULL != where) ? where : "",
			quoted_currency);
	}

	result = venture_database_query_raw(self, sql, params, error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);

	if (NULL == result)
		return NULL;

	if (orm_result_next(result))
	{
		OrmRow *row;

		row = orm_result_get_row(result);

		if (!orm_row_is_null(row, 0))
		{
			total = venture_money_new(orm_row_get_integer(row, 0),
			                          orm_row_get_string(row, 1),
			                          (guint8)orm_row_get_integer(row, 2));
		}
	}

	/* An empty set totals to zero rather than to an error: a month with
	 * no sales should report 0.00. */
	if (NULL == total)
		total = venture_money_new_zero(NULL);

	return total;
}

/* --- Migration and seeding ----------------------------------------------- */

gint64
venture_database_get_schema_version(VentureDatabase *self)
{
	g_autoptr(OrmResult) result = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), 0);

	result = venture_database_query_raw(self,
		"SELECT version FROM venture_schema_version LIMIT 1", NULL, NULL);

	if ((NULL == result) || !orm_result_next(result))
		return 0;

	return orm_row_get_integer(orm_result_get_row(result), 0);
}

/*
 * Creates the row a fresh install needs, if the table is empty. Returns the
 * identifier of the record, whether it was just made or already existed.
 */
static gint64
venture_database_seed_default_organization(
	VentureDatabase	 *self,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureOrganization) organization = NULL;

	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	existing = venture_database_find_one(self, query, NULL);

	if (NULL != existing)
		return venture_entity_get_id(existing);

	organization = venture_organization_new();
	g_object_set(organization,
	             "name", "Default",
	             "slug", "default",
	             "kind", VENTURE_ORGANIZATION_KIND_SOLE_PROPRIETOR,
	             "default-currency", venture_money_get_default_currency(),
	             "fiscal-year-start-month", (gint64)1,
	             "is-default", TRUE,
	             "active", TRUE,
	             NULL);

	if (!venture_database_save(self, VENTURE_ENTITY(organization), NULL, error))
		return 0;

	return venture_entity_get_id(VENTURE_ENTITY(organization));
}

/*
 * A minimal chart of accounts. Enough to post a sale and an expense without
 * the operator having to invent an accounting structure before recording
 * anything, and conventional enough that an accountant will recognise it.
 */
static gboolean
venture_database_seed_accounts(
	VentureDatabase	 *self,
	gint64		  organization_id,
	GError		**error
){
	static const struct
	{
		const gchar		*code;
		const gchar		*name;
		VentureAccountKind	 kind;
	} accounts[] = {
		{ "1000", "Cash",                  VENTURE_ACCOUNT_KIND_ASSET },
		{ "1050", "Processor clearing",    VENTURE_ACCOUNT_KIND_ASSET },
		{ "1100", "Accounts receivable",   VENTURE_ACCOUNT_KIND_ASSET },
		{ "1200", "Inventory",             VENTURE_ACCOUNT_KIND_ASSET },
		{ "1300", "Recoverable tax",       VENTURE_ACCOUNT_KIND_ASSET },
		{ "2000", "Accounts payable",      VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "2100", "Sales tax payable",     VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "2200", "Deferred revenue",      VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "2500", "Notes payable",         VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "3000", "Owner's equity",        VENTURE_ACCOUNT_KIND_EQUITY },
		{ "3100", "Owner's draw",          VENTURE_ACCOUNT_KIND_EQUITY },
		{ "4000", "Sales",                 VENTURE_ACCOUNT_KIND_INCOME },
		{ "4100", "Shipping income",       VENTURE_ACCOUNT_KIND_INCOME },
		{ "5000", "Cost of goods sold",    VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6000", "Platform fees",         VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6100", "Shipping expense",      VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6200", "Advertising",           VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6300", "Software and services", VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6400", "Supplies",              VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6500", "Professional fees",     VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6600", "Home office",           VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6700", "Travel",                VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6800", "Bad debt",              VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "6900", "General expenses",      VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "7600", "Exchange gain/loss",    VENTURE_ACCOUNT_KIND_EXPENSE }
	};
	g_autoptr(VentureQuery) query = NULL;
	gint64 existing;
	gsize i;

	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	existing = venture_database_count(self, query, error);

	/* A failed count must stop the seed, not stand in for "none": treating
	 * the error as an empty table would duplicate the whole chart of
	 * accounts on the next successful start. */
	if (existing < 0)
		return FALSE;

	/* Seeding is only for a genuinely empty install; re-running must not
	 * resurrect accounts the operator deliberately removed. */
	if (existing > 0)
		return TRUE;

	for (i = 0; i < G_N_ELEMENTS(accounts); i++)
	{
		g_autoptr(VentureAccount) account = NULL;

		account = venture_account_new();
		g_object_set(account,
		             "code", accounts[i].code,
		             "name", accounts[i].name,
		             "kind", accounts[i].kind,
		             "active", TRUE,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(account),
		                                   organization_id);

		if (!venture_database_save(self, VENTURE_ENTITY(account), NULL, error))
			return FALSE;
	}

	return TRUE;
}

/*
 * Common deduction categories. Every one defaults to REVIEW rather than a
 * deduction percentage, because the categories that are actually
 * contentious -- meals, home office, vehicle -- are exactly the ones where a
 * confident default would be doing the operator a disservice.
 */
static gboolean
venture_database_seed_tax_categories(
	VentureDatabase	 *self,
	gint64		  organization_id,
	GError		**error
){
	static const struct
	{
		const gchar		*code;
		const gchar		*name;
		VentureDeductibility	 deductibility;
		gint64			 business_use;
	} categories[] = {
		{ "ADVERTISING", "Advertising and promotion",
		  VENTURE_DEDUCTIBILITY_FULL, 100 },
		{ "SUPPLIES", "Supplies and materials",
		  VENTURE_DEDUCTIBILITY_FULL, 100 },
		{ "SOFTWARE", "Software and subscriptions",
		  VENTURE_DEDUCTIBILITY_FULL, 100 },
		{ "FEES", "Platform and payment fees",
		  VENTURE_DEDUCTIBILITY_FULL, 100 },
		{ "SHIPPING", "Shipping and postage",
		  VENTURE_DEDUCTIBILITY_FULL, 100 },
		{ "PROFESSIONAL", "Professional services",
		  VENTURE_DEDUCTIBILITY_FULL, 100 },
		{ "EDUCATION", "Education and training",
		  VENTURE_DEDUCTIBILITY_FULL, 100 },
		{ "HOME_OFFICE", "Home office",
		  VENTURE_DEDUCTIBILITY_PARTIAL, 0 },
		{ "VEHICLE", "Vehicle and mileage",
		  VENTURE_DEDUCTIBILITY_PARTIAL, 0 },
		{ "MEALS", "Meals",
		  VENTURE_DEDUCTIBILITY_PARTIAL, 50 },
		{ "TRAVEL", "Travel",
		  VENTURE_DEDUCTIBILITY_REVIEW, 0 },
		{ "EQUIPMENT", "Equipment",
		  VENTURE_DEDUCTIBILITY_CAPITAL, 0 },
		{ "PERSONAL", "Personal, not deductible",
		  VENTURE_DEDUCTIBILITY_NONE, 0 }
	};
	g_autoptr(VentureQuery) query = NULL;
	gint64 existing;
	gsize i;

	query = venture_query_new(VENTURE_TYPE_TAX_CATEGORY);
	existing = venture_database_count(self, query, error);

	if (existing < 0)
		return FALSE;

	if (existing > 0)
		return TRUE;

	for (i = 0; i < G_N_ELEMENTS(categories); i++)
	{
		g_autoptr(VentureTaxCategory) category = NULL;

		category = venture_tax_category_new();
		g_object_set(category,
		             "code", categories[i].code,
		             "name", categories[i].name,
		             "deductibility", categories[i].deductibility,
		             "default-business-use-percent", categories[i].business_use,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(category),
		                                   organization_id);

		if (!venture_database_save(self, VENTURE_ENTITY(category), NULL,
		                           error))
			return FALSE;
	}

	return TRUE;
}

gboolean
venture_database_migrate(
	VentureDatabase *self,
	VentureEntityRegistry *registry,
	GError **error
){
	g_autoptr(GRecMutexLocker) lock = NULL;
	g_autoptr(OrmMigrator) migrator = NULL;
	g_autoptr(GArray) applied = NULL;
	g_autoptr(GArray) pending = NULL;
	gint64 organization_id;

	g_return_val_if_fail(VENTURE_IS_DATABASE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(registry), FALSE);
	lock = g_rec_mutex_locker_new(&self->lock);
	if (self->transaction_depth != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			"Startup migrations require an idle database connection");
		return FALSE;
	}
	migrator = venture_migrations_new(self->connection, self->backend, error);
	/* Refuse changed or unknown history before touching application tables. */
	if (migrator == NULL || !orm_migrator_status(migrator, &applied, &pending, error))
		return FALSE;
	if (!venture_schema_create_all(self->connection, registry, error) ||
		!orm_migrator_up(migrator, 0, error))
		return FALSE;

	organization_id = venture_database_seed_default_organization(self, error);
	if (organization_id == 0 || !venture_database_seed_accounts(self, organization_id, error) ||
		!venture_database_seed_tax_categories(self, organization_id, error) ||
		!venture_setup_seed_defaults(self, organization_id, NULL, error) ||
		!venture_pipelines_migrate(self, error))
		return FALSE;
	return TRUE;
}

/* A one-use permit is consumed before callbacks can re-enter a generic save. */
gboolean
venture_stripe_save_owned(VentureDatabase *database, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	gboolean ok;
	g_autoptr(GRecMutexLocker) locker = g_rec_mutex_locker_new(&database->lock);

	database->stripe_write_permit = entity;
	ok = venture_database_save(database, entity, actor, error);
	database->stripe_write_permit = NULL;
	return ok;
}

VentureAssetService *
venture_asset_service_get(VentureDatabase *database)
{
	if (database->asset_service == NULL)
		database->asset_service = g_object_new(VENTURE_TYPE_ASSET_SERVICE, "database", database, NULL);
	return database->asset_service;
}

VentureAccessPolicy *
venture_database_get_access_policy(VentureDatabase *self)
{
	if (NULL == self->access_policy)
		self->access_policy = venture_access_policy_new(self);
	return self->access_policy;
}

gboolean venture_database_has_transaction(VentureDatabase *self)
{
	gboolean active;
	g_rec_mutex_lock(&self->lock);
	active = self->transaction_depth != 0;
	g_rec_mutex_unlock(&self->lock);
	return active;
}

VentureMailOutbox *venture_database_get_mail_outbox(VentureDatabase *self)
{
	if (!self->mail_outbox)
		self->mail_outbox = g_object_new(VENTURE_TYPE_MAIL_OUTBOX, "database", self, NULL);
	return self->mail_outbox;
}

VentureLeadService *
venture_database_get_lead_service(VentureDatabase *self)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(self), NULL);
	g_rec_mutex_lock(&self->lock);
	if (self->lead_service == NULL)
		self->lead_service = g_object_new(VENTURE_TYPE_LEAD_SERVICE, "database", self, NULL);
	g_rec_mutex_unlock(&self->lock);
	return self->lead_service;
}

VentureActivityService *
venture_database_get_activity_service(VentureDatabase *database)
{
	return database->activities;
}

VenturePayablesService *
venture_database_get_payables_service(VentureDatabase *database)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	if (database->payables == NULL)
		database->payables = g_object_new(VENTURE_TYPE_PAYABLES_SERVICE, "database", database, NULL);
	return database->payables;
}

VentureBankMatchService *
venture_database_get_bank_match_service(VentureDatabase *database)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	if (database->bank_match_service == NULL)
		database->bank_match_service = g_object_new(VENTURE_TYPE_BANK_MATCH_SERVICE,
			"database", database, NULL);
	return database->bank_match_service;
}

VentureDealService *
venture_database_get_deal_service(VentureDatabase *self)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(self), NULL);
	if (NULL == self->deal_service)
		self->deal_service = g_object_new(VENTURE_TYPE_DEAL_SERVICE, "database", self, NULL);
	return self->deal_service;
}

VentureSequenceService *
venture_sequence_service_get(VentureDatabase *database)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	if (database->sequence_service == NULL)
		database->sequence_service = g_object_new(VENTURE_TYPE_SEQUENCE_SERVICE, "database", database, NULL);
	return database->sequence_service;
}

VentureActionRegistry *
venture_database_get_action_registry(VentureDatabase *self)
{
	if (NULL == self->actions)
	{
		self->actions = g_object_new(VENTURE_TYPE_ACTION_REGISTRY, "database", self, NULL);
		venture_journal_actions_register(self);
		venture_cutover_actions_register(self);
		venture_setup_actions_register(self);
		venture_recurring_register_actions(self);
		venture_progress_actions_register(self);
		venture_portal_actions_register(self);
		venture_supplier_portal_actions_register(self);
		venture_backup_actions_register(self);
		venture_tax_filing_actions_register(self);
		venture_dunning_actions_register(self);
		venture_backup_schedule_actions_register(self);
		venture_crm_import_actions_register(self);
		venture_dedupe_actions_register(self);
		venture_mfa_actions_register(self);
		venture_projects_actions_register(self);
		venture_activity_actions_register(self);
	}
	return self->actions;
}

GRecMutexLocker *
venture_database_lock_scope(VentureDatabase *self)
{
	return g_rec_mutex_locker_new(&self->lock);
}
