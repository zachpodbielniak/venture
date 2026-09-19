/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The second-factor service: the one place that reads a TOTP secret,
 * verifies a code, spends a recovery code or turns the factor off. The
 * web pages, the login flow, the REST action and the CLI all call in here;
 * a generic write to a user_mfa or mfa_recovery_code row is refused with an
 * error naming this service.
 */
#include "venture.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>

#define MFA_SECRET_BYTES 20
#define MFA_CIPHER_PREFIX "gcm1$"
#define MFA_RECOVERY_ALPHABET "abcdefghjkmnpqrstuvwxyz23456789"
#define MFA_RECOVERY_LENGTH 10
#define MFA_RECOVERY_PREFIX_LENGTH 4
#define MFA_AUDIT_SOURCE "mfa"

struct _VentureMfaService
{
	GObject parent_instance;
	VentureDatabase	*database;
	gchar		*key;
	GDateTime	*fixed_time;
	VentureEntity	*permit;
	gboolean	 actions;
};

enum { PROP_0, PROP_DATABASE, PROP_FIXED_TIME, N_PROPS };
static GParamSpec *properties[N_PROPS];

G_DEFINE_FINAL_TYPE(VentureMfaService, venture_mfa_service, G_TYPE_OBJECT)

/* --- Boxed enrolment ------------------------------------------------------ */

VentureMfaEnrolment *
venture_mfa_enrolment_copy(const VentureMfaEnrolment *self)
{
	VentureMfaEnrolment *copy;
	if (NULL == self)
		return NULL;
	copy = g_new0(VentureMfaEnrolment, 1);
	copy->secret = g_strdup(self->secret);
	copy->uri = g_strdup(self->uri);
	copy->svg = g_strdup(self->svg);
	return copy;
}

void
venture_mfa_enrolment_free(VentureMfaEnrolment *self)
{
	if (NULL == self)
		return;
	g_free(self->secret);
	g_free(self->uri);
	g_free(self->svg);
	g_free(self);
}

G_DEFINE_BOXED_TYPE(VentureMfaEnrolment, venture_mfa_enrolment, venture_mfa_enrolment_copy, venture_mfa_enrolment_free)

/* --- GObject ---------------------------------------------------------------- */

static void
service_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureMfaService *self = VENTURE_MFA_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE:
		self->database = g_value_get_object(value);
		break;
	case PROP_FIXED_TIME:
		g_clear_pointer(&self->fixed_time, g_date_time_unref);
		self->fixed_time = g_value_dup_boxed(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}

static void
service_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureMfaService *self = VENTURE_MFA_SERVICE(object);
	switch (id)
	{
	case PROP_DATABASE:
		g_value_set_object(value, self->database);
		break;
	case PROP_FIXED_TIME:
		g_value_set_boxed(value, self->fixed_time);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}

static void
service_finalize(GObject *object)
{
	VentureMfaService *self = VENTURE_MFA_SERVICE(object);
	g_clear_pointer(&self->key, g_free);
	g_clear_pointer(&self->fixed_time, g_date_time_unref);
	G_OBJECT_CLASS(venture_mfa_service_parent_class)->finalize(object);
}

static void
venture_mfa_service_class_init(VentureMfaServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->set_property = service_set_property;
	object_class->get_property = service_get_property;
	object_class->finalize = service_finalize;
	/**
	 * VentureMfaService:database:
	 *
	 * The database owning the records. The database owns the service, so
	 * this is not a reference.
	 */
	properties[PROP_DATABASE] = g_param_spec_object("database", "Database", "Owning database",
		VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * VentureMfaService:fixed-time:
	 *
	 * When set, every verification and window calculation uses this
	 * instant instead of the wall clock. For tests; %NULL in production.
	 */
	properties[PROP_FIXED_TIME] = g_param_spec_boxed("fixed-time", "Fixed time", "Clock override for tests",
		G_TYPE_DATE_TIME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
venture_mfa_service_init(VentureMfaService *self)
{
	(void)self;
}

/* --- Helpers ------------------------------------------------------------------ */

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, code, message);
	return FALSE;
}

static gboolean
module_on(void)
{
	return venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "user_mfa");
}

GDateTime *
venture_mfa_service_now(VentureMfaService *self)
{
	g_return_val_if_fail(VENTURE_IS_MFA_SERVICE(self), NULL);
	if (NULL != self->fixed_time)
		return g_date_time_ref(self->fixed_time);
	return venture_time_now();
}

/* Writes through this service carry a permit; anything else is refused. */
static gboolean
service_only_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	VentureMfaService *self = data;
	(void)db;
	(void)previous;
	if (self->permit == entity)
		return TRUE;
	return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
		"Second-factor rows are written by VentureMfaService; enrol, verify or reset through it");
}

static gboolean
policy_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MFA_POLICY);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	(void)previous;
	(void)data;
	if (venture_entity_get_organization_id(entity) <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "An MFA policy belongs to one organization");
	venture_query_set_organization(query, venture_entity_get_organization_id(entity));
	venture_query_set_limit(query, 0);
	rows = venture_database_find(db, query, error);
	if (NULL == rows)
		return FALSE;
	for (i = 0; i < rows->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(rows, i)) != venture_entity_get_id(entity))
			return refuse(error, VENTURE_ERROR_ALREADY_EXISTS, "This organization already has an MFA policy; edit it");
	return TRUE;
}

static gboolean
save_permitted(VentureMfaService *self, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->permit = entity;
	ok = venture_database_save(self->database, entity, actor, error);
	self->permit = NULL;
	return ok;
}

VentureMfaService *
venture_mfa_service_get(VentureDatabase *database)
{
	VentureMfaService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-mfa-service");
	if (NULL == self)
	{
		self = g_object_new(VENTURE_TYPE_MFA_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-mfa-service", self, g_object_unref);
		venture_database_add_save_validator(database, VENTURE_TYPE_USER_MFA, service_only_validate, self, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_MFA_RECOVERY_CODE, service_only_validate, self, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_MFA_POLICY, policy_validate, self, NULL);
	}
	return self;
}

void
venture_mfa_service_configure(VentureMfaService *self, VentureConfig *config)
{
	const gchar *key;
	g_return_if_fail(VENTURE_IS_MFA_SERVICE(self));
	g_return_if_fail(VENTURE_IS_CONFIG(config));
	key = venture_config_get_secret(config, "security-mfa-key-env");
	if (venture_string_is_empty(key))
		key = venture_config_get_secret(config, "security-session-secret-env");
	g_clear_pointer(&self->key, g_free);
	self->key = venture_string_is_empty(key) ? NULL : g_strdup(key);
}

/* --- Encryption at rest --------------------------------------------------------- */

static gboolean
derive_key(VentureMfaService *self, guchar *out, GError **error)
{
	gsize length = 32;
	g_autoptr(GChecksum) checksum = NULL;
	if (NULL == self->key)
		return refuse(error, VENTURE_ERROR_CONFIG,
			"No key to encrypt second-factor secrets: set the variable named by security.mfa_key_env "
			"(or security.session_secret_env)");
	checksum = g_checksum_new(G_CHECKSUM_SHA256);
	g_checksum_update(checksum, (const guchar *)self->key, (gssize)strlen(self->key));
	g_checksum_get_digest(checksum, out, &length);
	return TRUE;
}

static gchar *
encrypt_secret(VentureMfaService *self, const guchar *plain, gsize length, GError **error)
{
	guchar key[32];
	guchar iv[12];
	guchar tag[16];
	g_autofree guchar *cipher = g_malloc0(length + 16);
	g_autofree gchar *iv_text = NULL;
	g_autofree gchar *cipher_text = NULL;
	g_autofree gchar *tag_text = NULL;
	EVP_CIPHER_CTX *ctx;
	gint out_length = 0;
	gint total = 0;
	gboolean ok;
	if (!derive_key(self, key, error))
		return NULL;
	if (1 != RAND_bytes(iv, sizeof(iv)))
	{
		refuse(error, VENTURE_ERROR_FAILED, "The random generator refused to produce a nonce");
		return NULL;
	}
	ctx = EVP_CIPHER_CTX_new();
	ok = 1 == EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) &&
	     1 == EVP_EncryptUpdate(ctx, cipher, &out_length, plain, (gint)length);
	total = out_length;
	ok = ok && 1 == EVP_EncryptFinal_ex(ctx, cipher + total, &out_length);
	total += out_length;
	ok = ok && 1 == EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag);
	EVP_CIPHER_CTX_free(ctx);
	memset(key, 0, sizeof(key));
	if (!ok)
	{
		refuse(error, VENTURE_ERROR_FAILED, "Encrypting the second-factor secret failed");
		return NULL;
	}
	iv_text = g_base64_encode(iv, sizeof(iv));
	cipher_text = g_base64_encode(cipher, (gsize)total);
	tag_text = g_base64_encode(tag, sizeof(tag));
	return g_strdup_printf(MFA_CIPHER_PREFIX "%s$%s$%s", iv_text, cipher_text, tag_text);
}

static guchar *
decrypt_secret(VentureMfaService *self, const gchar *stored, gsize *out_length, GError **error)
{
	guchar key[32];
	g_auto(GStrv) parts = NULL;
	g_autofree guchar *iv = NULL;
	g_autofree guchar *cipher = NULL;
	g_autofree guchar *tag = NULL;
	g_autofree guchar *plain = NULL;
	gsize iv_length = 0, cipher_length = 0, tag_length = 0;
	EVP_CIPHER_CTX *ctx;
	gint out = 0;
	gint total = 0;
	gboolean ok;
	*out_length = 0;
	if (venture_string_is_empty(stored) || !g_str_has_prefix(stored, MFA_CIPHER_PREFIX))
	{
		refuse(error, VENTURE_ERROR_FAILED, "The stored second-factor secret is not in a known format");
		return NULL;
	}
	if (!derive_key(self, key, error))
		return NULL;
	parts = g_strsplit(stored + strlen(MFA_CIPHER_PREFIX), "$", 3);
	if (NULL == parts[0] || NULL == parts[1] || NULL == parts[2])
	{
		refuse(error, VENTURE_ERROR_FAILED, "The stored second-factor secret is truncated");
		return NULL;
	}
	iv = g_base64_decode(parts[0], &iv_length);
	cipher = g_base64_decode(parts[1], &cipher_length);
	tag = g_base64_decode(parts[2], &tag_length);
	if (12 != iv_length || 16 != tag_length)
	{
		refuse(error, VENTURE_ERROR_FAILED, "The stored second-factor secret is malformed");
		return NULL;
	}
	plain = g_malloc0(cipher_length + 1);
	ctx = EVP_CIPHER_CTX_new();
	ok = 1 == EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) &&
	     1 == EVP_DecryptUpdate(ctx, plain, &out, cipher, (gint)cipher_length);
	total = out;
	ok = ok && 1 == EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (gint)tag_length, tag) &&
	     1 == EVP_DecryptFinal_ex(ctx, plain + total, &out);
	total += out;
	EVP_CIPHER_CTX_free(ctx);
	memset(key, 0, sizeof(key));
	if (!ok)
	{
		refuse(error, VENTURE_ERROR_CONFIG,
			"The second-factor secret does not decrypt with the configured key; was security.mfa_key_env changed?");
		return NULL;
	}
	*out_length = (gsize)total;
	return g_steal_pointer(&plain);
}

/* --- Rows ------------------------------------------------------------------------- */

VentureUserMfa *
venture_mfa_service_get_row(VentureMfaService *self, gint64 user_id)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	VentureEntity *row;
	g_return_val_if_fail(VENTURE_IS_MFA_SERVICE(self), NULL);
	if (user_id <= 0 || !module_on())
		return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	query = venture_query_new(VENTURE_TYPE_USER_MFA);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user_id, NULL);
	row = venture_database_find_one(self->database, query, NULL);
	return NULL != row ? VENTURE_USER_MFA(row) : NULL;
}

gboolean
venture_mfa_service_is_enabled(VentureMfaService *self, gint64 user_id)
{
	g_autoptr(VentureUserMfa) row = venture_mfa_service_get_row(self, user_id);
	gboolean enabled = FALSE;
	if (NULL == row)
		return FALSE;
	g_object_get(row, "enabled", &enabled, NULL);
	return enabled;
}

static gint64
user_organization(VentureMfaService *self, gint64 user_id)
{
	g_autoptr(VentureEntity) user = venture_database_get(self->database, VENTURE_TYPE_USER, user_id, NULL);
	gint64 org = NULL != user ? venture_entity_get_organization_id(user) : 0;
	if (org <= 0)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) organization = NULL;
		venture_query_add_filter_string(query, "is-default", VENTURE_FILTER_OP_EQ, "true", NULL);
		organization = venture_database_find_one(self->database, query, NULL);
		org = NULL != organization ? venture_entity_get_id(organization) : 1;
	}
	return org;
}

/* An audit row of its own for each second-factor event, alongside the
 * redacted row diffs the repository records. */
static void
audit(VentureMfaService *self, const gchar *event, VentureEntity *target, const VentureActor *actor,
	const gchar *remote_address, VentureAuditAction action)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) diff = NULL;
	g_autoptr(VentureAuditEntry) entry = NULL;
	g_autoptr(GError) error = NULL;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "event");
	json_builder_add_string_value(builder, event);
	json_builder_end_object(builder);
	diff = json_builder_get_root(builder);
	entry = venture_audit_entry_new_for_change(action,
		NULL != actor ? actor->kind : VENTURE_ACTOR_KIND_SYSTEM,
		NULL != actor ? actor->name : NULL, target, diff);
	g_object_set(entry, "source", MFA_AUDIT_SOURCE, NULL);
	if (!venture_string_is_empty(remote_address))
		g_object_set(entry, "ip-address", remote_address, NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(entry), NULL, &error))
		g_warning("Cannot record the %s audit entry: %s", event, error->message);
}

/* --- Recovery codes --------------------------------------------------------------- */

static gchar *
generate_recovery_code(void)
{
	g_autofree gchar *hex = venture_generate_token(MFA_RECOVERY_LENGTH);
	gchar code[MFA_RECOVERY_LENGTH + 2];
	guint i, j = 0;
	gsize alphabet = strlen(MFA_RECOVERY_ALPHABET);
	for (i = 0; i < MFA_RECOVERY_LENGTH; i++)
	{
		guint byte = (guint)(g_ascii_xdigit_value(hex[i * 2]) << 4) | (guint)g_ascii_xdigit_value(hex[i * 2 + 1]);
		if (i == MFA_RECOVERY_LENGTH / 2)
			code[j++] = '-';
		code[j++] = MFA_RECOVERY_ALPHABET[byte % alphabet];
	}
	code[j] = '\0';
	return g_strdup(code);
}

static gchar *
normalise_recovery_code(const gchar *typed)
{
	GString *out = g_string_new(NULL);
	const gchar *p;
	for (p = typed; NULL != p && '\0' != *p; p++)
		if (g_ascii_isalnum(*p))
			g_string_append_c(out, g_ascii_tolower(*p));
	return g_string_free(out, FALSE);
}

static gboolean
spend_unused_codes(VentureMfaService *self, gint64 user_id, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MFA_RECOVERY_CODE);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user_id, NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, error);
	if (NULL == rows)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) used = NULL;
		g_object_get(row, "used-at", &used, NULL);
		if (NULL != used)
			continue;
		g_object_set(row, "used-at", now, NULL);
		if (!save_permitted(self, row, actor, error))
			return FALSE;
	}
	return TRUE;
}

static GStrv
issue_recovery_codes(VentureMfaService *self, gint64 user_id, gint64 org, const VentureActor *actor, GError **error)
{
	g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
	guint i;
	for (i = 0; i < VENTURE_MFA_RECOVERY_CODE_COUNT; i++)
	{
		g_autofree gchar *code = generate_recovery_code();
		g_autofree gchar *normalised = normalise_recovery_code(code);
		g_autofree gchar *prefix = g_strndup(normalised, MFA_RECOVERY_PREFIX_LENGTH);
		g_autofree gchar *hash = venture_hash_password(normalised, 100000, error);
		g_autoptr(VentureEntity) row = NULL;
		if (NULL == hash)
			return NULL;
		row = g_object_new(VENTURE_TYPE_MFA_RECOVERY_CODE, "organization-id", org,
			"user-id", user_id, "prefix", prefix, "code-hash", hash, NULL);
		if (!save_permitted(self, row, actor, error))
			return NULL;
		g_strv_builder_add(builder, code);
	}
	return g_strv_builder_end(builder);
}

/* --- Enrolment ----------------------------------------------------------------------- */

static VentureMfaEnrolment *
render_enrolment(const guchar *secret, gsize length, const gchar *issuer, const gchar *account, GError **error)
{
	g_autoptr(VentureMfaEnrolment) enrolment = g_new0(VentureMfaEnrolment, 1);
	enrolment->secret = venture_base32_encode(secret, length);
	enrolment->uri = venture_totp_uri(issuer, account, enrolment->secret);
	enrolment->svg = venture_qr_svg_render(enrolment->uri, 0, error);
	if (NULL == enrolment->svg)
		return NULL;
	return g_steal_pointer(&enrolment);
}

VentureMfaEnrolment *
venture_mfa_service_begin_enrolment(VentureMfaService *self, gint64 user_id,
	const gchar *issuer, const gchar *account, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureUserMfa) row = NULL;
	g_autofree gchar *hex = NULL;
	g_autofree gchar *stored = NULL;
	guchar secret[MFA_SECRET_BYTES];
	VentureMfaEnrolment *enrolment;
	gboolean enabled = FALSE;
	guint i;
	g_return_val_if_fail(VENTURE_IS_MFA_SERVICE(self), NULL);
	if (!module_on())
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "The mfa module is disabled on this install (modules.mfa.enabled)");
		return NULL;
	}
	if (user_id <= 0)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "A second factor belongs to a user account");
		return NULL;
	}
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	row = venture_mfa_service_get_row(self, user_id);
	if (NULL != row)
		g_object_get(row, "enabled", &enabled, NULL);
	if (enabled)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "A second factor is already enabled; disable it before enrolling again");
		return NULL;
	}
	hex = venture_generate_token(MFA_SECRET_BYTES);
	for (i = 0; i < MFA_SECRET_BYTES; i++)
		secret[i] = (guchar)((g_ascii_xdigit_value(hex[i * 2]) << 4) | g_ascii_xdigit_value(hex[i * 2 + 1]));
	stored = encrypt_secret(self, secret, sizeof(secret), error);
	if (NULL == stored)
		return NULL;
	enrolment = render_enrolment(secret, sizeof(secret), issuer, account, error);
	memset(secret, 0, sizeof(secret));
	if (NULL == enrolment)
		return NULL;
	if (NULL == row)
		row = g_object_new(VENTURE_TYPE_USER_MFA, "organization-id", user_organization(self, user_id), "user-id", user_id, NULL);
	g_object_set(row, "secret-ref", stored, "enabled", FALSE, "enrolled-at", NULL,
		"last-used-counter", (gint64)0, "failure-count", (gint64)0, "failure-window-started-at", NULL, NULL);
	if (!save_permitted(self, VENTURE_ENTITY(row), actor, error))
	{
		venture_mfa_enrolment_free(enrolment);
		return NULL;
	}
	audit(self, "mfa_enrolment_started", VENTURE_ENTITY(row), actor, NULL, VENTURE_AUDIT_ACTION_UPDATE);
	return enrolment;
}

static guchar *
row_secret(VentureMfaService *self, VentureUserMfa *row, gsize *length, GError **error)
{
	g_autofree gchar *stored = NULL;
	g_object_get(row, "secret-ref", &stored, NULL);
	return decrypt_secret(self, stored, length, error);
}

VentureMfaEnrolment *
venture_mfa_service_pending_enrolment(VentureMfaService *self, gint64 user_id,
	const gchar *issuer, const gchar *account, GError **error)
{
	g_autoptr(VentureUserMfa) row = NULL;
	g_autofree guchar *secret = NULL;
	gsize length = 0;
	gboolean enabled = FALSE;
	VentureMfaEnrolment *enrolment;
	g_return_val_if_fail(VENTURE_IS_MFA_SERVICE(self), NULL);
	row = venture_mfa_service_get_row(self, user_id);
	if (NULL != row)
		g_object_get(row, "enabled", &enabled, NULL);
	if (NULL == row || enabled)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "No enrolment is pending");
		return NULL;
	}
	secret = row_secret(self, row, &length, error);
	if (NULL == secret)
		return NULL;
	enrolment = render_enrolment(secret, length, issuer, account, error);
	memset(secret, 0, length);
	return enrolment;
}

GStrv
venture_mfa_service_confirm_enrolment(VentureMfaService *self, gint64 user_id,
	const gchar *code, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureUserMfa) row = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree guchar *secret = NULL;
	GStrv codes = NULL;
	gsize length = 0;
	guint64 counter = 0;
	gboolean enabled = FALSE;
	gboolean verified;
	g_return_val_if_fail(VENTURE_IS_MFA_SERVICE(self), NULL);
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	row = venture_mfa_service_get_row(self, user_id);
	if (NULL != row)
		g_object_get(row, "enabled", &enabled, NULL);
	if (NULL == row || enabled)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "No enrolment is pending; start one first");
		return NULL;
	}
	secret = row_secret(self, row, &length, error);
	if (NULL == secret)
		return NULL;
	now = venture_mfa_service_now(self);
	verified = venture_totp_verify(secret, length, code, now, 1, &counter);
	memset(secret, 0, length);
	if (!verified)
	{
		audit(self, "mfa_enrolment_failed", VENTURE_ENTITY(row), actor, NULL, VENTURE_AUDIT_ACTION_UPDATE);
		refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "That code did not match; check the app's clock and try the next one");
		return NULL;
	}
	if (!venture_database_begin(self->database, error))
		return NULL;
	g_object_set(row, "enabled", TRUE, "enrolled-at", now, "last-used-counter", (gint64)counter,
		"last-used-at", now, "failure-count", (gint64)0, "failure-window-started-at", NULL, NULL);
	if (!save_permitted(self, VENTURE_ENTITY(row), actor, error) ||
	    !spend_unused_codes(self, user_id, now, actor, error) ||
	    NULL == (codes = issue_recovery_codes(self, user_id, venture_entity_get_organization_id(VENTURE_ENTITY(row)), actor, error)))
	{
		venture_database_rollback(self->database);
		return NULL;
	}
	audit(self, "mfa_enrolled", VENTURE_ENTITY(row), actor, NULL, VENTURE_AUDIT_ACTION_UPDATE);
	if (!venture_database_commit(self->database, error))
	{
		g_strfreev(codes);
		return NULL;
	}
	return codes;
}

/* --- Verification ------------------------------------------------------------------ */

static gboolean
window_open(VentureUserMfa *row, GDateTime *now)
{
	g_autoptr(GDateTime) started = NULL;
	g_object_get(row, "failure-window-started-at", &started, NULL);
	return NULL != started &&
		g_date_time_difference(now, started) < (VENTURE_MFA_FAILURE_WINDOW_SECONDS * G_TIME_SPAN_SECOND);
}

static gboolean
record_failure(VentureMfaService *self, VentureUserMfa *row, GDateTime *now, const gchar *event,
	const VentureActor *actor, const gchar *remote_address, GError **error)
{
	gint64 count = 0;
	if (window_open(row, now))
		g_object_get(row, "failure-count", &count, NULL);
	else
		g_object_set(row, "failure-window-started-at", now, NULL);
	g_object_set(row, "failure-count", count + 1, NULL);
	if (!save_permitted(self, VENTURE_ENTITY(row), actor, error))
		return FALSE;
	audit(self, event, VENTURE_ENTITY(row), actor, remote_address, VENTURE_AUDIT_ACTION_LOGIN);
	return TRUE;
}

static gboolean
try_recovery_code(VentureMfaService *self, gint64 user_id, const gchar *typed, GDateTime *now,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *normalised = normalise_recovery_code(typed);
	g_autofree gchar *prefix = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	if (strlen(normalised) != MFA_RECOVERY_LENGTH)
		return FALSE;
	prefix = g_strndup(normalised, MFA_RECOVERY_PREFIX_LENGTH);
	query = venture_query_new(VENTURE_TYPE_MFA_RECOVERY_CODE);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user_id, NULL);
	venture_query_add_filter_string(query, "prefix", VENTURE_FILTER_OP_EQ, prefix, NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, NULL);
	for (i = 0; NULL != rows && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) used = NULL;
		g_autofree gchar *hash = NULL;
		g_object_get(row, "used-at", &used, "code-hash", &hash, NULL);
		/* A spent code is compared anyway and then refused, so the time
		 * taken does not reveal whether it was ever valid. */
		if (!venture_verify_password(normalised, hash))
			continue;
		if (NULL != used)
			return FALSE;
		g_object_set(row, "used-at", now, NULL);
		return save_permitted(self, row, actor, error);
	}
	return FALSE;
}

gboolean
venture_mfa_service_verify(VentureMfaService *self, gint64 user_id, const gchar *code,
	const VentureActor *actor, const gchar *remote_address, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureUserMfa) row = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree guchar *secret = NULL;
	gsize length = 0;
	gint64 count = 0;
	gint64 last_counter = 0;
	guint64 counter = 0;
	gboolean enabled = FALSE;
	gboolean by_totp;
	const gchar *event = "mfa_failure";
	g_return_val_if_fail(VENTURE_IS_MFA_SERVICE(self), FALSE);
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	row = venture_mfa_service_get_row(self, user_id);
	if (NULL != row)
		g_object_get(row, "enabled", &enabled, NULL);
	if (NULL == row || !enabled)
		return refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "No second factor is enrolled for this account");
	now = venture_mfa_service_now(self);
	if (!venture_database_begin(self->database, error))
		return FALSE;
	g_object_get(row, "failure-count", &count, "last-used-counter", &last_counter, NULL);
	if (count >= VENTURE_MFA_FAILURE_LIMIT && window_open(row, now))
	{
		audit(self, "mfa_rate_limited", VENTURE_ENTITY(row), actor, remote_address, VENTURE_AUDIT_ACTION_LOGIN);
		venture_database_commit(self->database, NULL);
		return refuse(error, VENTURE_ERROR_UNAUTHENTICATED,
			"Too many wrong codes; wait fifteen minutes and try again");
	}
	secret = row_secret(self, row, &length, error);
	if (NULL == secret)
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	by_totp = venture_totp_verify(secret, length, code, now, 1, &counter);
	memset(secret, 0, length);
	if (by_totp && (gint64)counter <= last_counter)
	{
		/* The same step twice is somebody who watched the first one. */
		by_totp = FALSE;
		event = "mfa_replay_refused";
	}
	if (by_totp)
	{
		g_object_set(row, "last-used-counter", (gint64)counter, "last-used-at", now,
			"failure-count", (gint64)0, "failure-window-started-at", NULL, NULL);
		event = "mfa_success";
	}
	else if (0 == g_strcmp0(event, "mfa_failure") && try_recovery_code(self, user_id, code, now, actor, error))
	{
		g_object_set(row, "last-used-at", now, "failure-count", (gint64)0, "failure-window-started-at", NULL, NULL);
		event = "mfa_recovery_success";
	}
	if (0 == g_strcmp0(event, "mfa_success") || 0 == g_strcmp0(event, "mfa_recovery_success"))
	{
		if (!save_permitted(self, VENTURE_ENTITY(row), actor, error))
		{
			venture_database_rollback(self->database);
			return FALSE;
		}
		audit(self, event, VENTURE_ENTITY(row), actor, remote_address, VENTURE_AUDIT_ACTION_LOGIN);
		return venture_database_commit(self->database, error);
	}
	if (!record_failure(self, row, now, event, actor, remote_address, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	if (!venture_database_commit(self->database, error))
		return FALSE;
	return refuse(error, VENTURE_ERROR_UNAUTHENTICATED, "That code is not valid");
}

/* --- Disable, reset, regenerate ------------------------------------------------------ */

static gboolean
turn_off(VentureMfaService *self, gint64 user_id, const gchar *event, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureUserMfa) row = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_return_val_if_fail(VENTURE_IS_MFA_SERVICE(self), FALSE);
	if (!module_on())
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "The mfa module is disabled on this install (modules.mfa.enabled)");
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	row = venture_mfa_service_get_row(self, user_id);
	if (NULL != row)
	{
		g_autofree gchar *stored = NULL;
		gboolean enabled = FALSE;
		g_object_get(row, "enabled", &enabled, "secret-ref", &stored, NULL);
		/* A pending enrolment is cleared too; an already-off factor is
		 * nothing to turn off. */
		if (!enabled && venture_string_is_empty(stored))
			g_clear_object(&row);
	}
	if (NULL == row)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "No second factor is enrolled for this account");
	now = venture_mfa_service_now(self);
	if (!venture_database_begin(self->database, error))
		return FALSE;
	g_object_set(row, "enabled", FALSE, "secret-ref", NULL, "enrolled-at", NULL,
		"last-used-counter", (gint64)0, "failure-count", (gint64)0, "failure-window-started-at", NULL, NULL);
	if (!save_permitted(self, VENTURE_ENTITY(row), actor, error) ||
	    !spend_unused_codes(self, user_id, now, actor, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	audit(self, event, VENTURE_ENTITY(row), actor, NULL, VENTURE_AUDIT_ACTION_UPDATE);
	return venture_database_commit(self->database, error);
}

gboolean
venture_mfa_service_disable(VentureMfaService *self, gint64 user_id, const VentureActor *actor, GError **error)
{
	return turn_off(self, user_id, "mfa_disabled", actor, error);
}

gboolean
venture_mfa_service_reset(VentureMfaService *self, gint64 user_id, const VentureActor *actor, GError **error)
{
	return turn_off(self, user_id, "mfa_reset", actor, error);
}

GStrv
venture_mfa_service_regenerate_recovery_codes(VentureMfaService *self, gint64 user_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureUserMfa) row = NULL;
	g_autoptr(GDateTime) now = NULL;
	gboolean enabled = FALSE;
	GStrv codes;
	g_return_val_if_fail(VENTURE_IS_MFA_SERVICE(self), NULL);
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	row = venture_mfa_service_get_row(self, user_id);
	if (NULL != row)
		g_object_get(row, "enabled", &enabled, NULL);
	if (NULL == row || !enabled)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Recovery codes belong to an enabled second factor");
		return NULL;
	}
	now = venture_mfa_service_now(self);
	if (!venture_database_begin(self->database, error))
		return NULL;
	if (!spend_unused_codes(self, user_id, now, actor, error) ||
	    NULL == (codes = issue_recovery_codes(self, user_id, venture_entity_get_organization_id(VENTURE_ENTITY(row)), actor, error)))
	{
		venture_database_rollback(self->database);
		return NULL;
	}
	audit(self, "mfa_recovery_codes_regenerated", VENTURE_ENTITY(row), actor, NULL, VENTURE_AUDIT_ACTION_UPDATE);
	if (!venture_database_commit(self->database, error))
	{
		g_strfreev(codes);
		return NULL;
	}
	return codes;
}

/* --- Policy ------------------------------------------------------------------------------ */

gboolean
venture_mfa_service_requires_enrolment(VentureMfaService *self, const VentureAuthPrincipal *principal)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) policies = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureQuery) memberships = NULL;
	g_autoptr(GPtrArray) members = NULL;
	guint i, j;
	g_return_val_if_fail(VENTURE_IS_MFA_SERVICE(self), FALSE);
	if (NULL == principal || !principal->authenticated || principal->user_id <= 0 || !module_on())
		return FALSE;
	if (venture_mfa_service_is_enabled(self, principal->user_id))
		return FALSE;
	internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	policies = venture_query_new(VENTURE_TYPE_MFA_POLICY);
	venture_query_add_filter_string(policies, "require-mfa-for-admins", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_set_limit(policies, 0);
	rows = venture_database_find(self->database, policies, NULL);
	if (NULL == rows || 0 == rows->len)
		return FALSE;
	/* A global owner or admin administers every organization, so any
	 * organization's requirement reaches them. */
	if (VENTURE_USER_ROLE_OWNER == principal->role || VENTURE_USER_ROLE_ADMIN == principal->role)
		return TRUE;
	memberships = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	venture_query_add_filter_int(memberships, "user-id", VENTURE_FILTER_OP_EQ, principal->user_id, NULL);
	venture_query_add_filter_string(memberships, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_set_limit(memberships, 0);
	members = venture_database_find(self->database, memberships, NULL);
	for (i = 0; NULL != members && i < members->len; i++)
	{
		VentureEntity *member = g_ptr_array_index(members, i);
		gint role = 0;
		g_object_get(member, "role", &role, NULL);
		if (VENTURE_ORGANIZATION_ROLE_OWNER != role && VENTURE_ORGANIZATION_ROLE_ADMIN != role)
			continue;
		for (j = 0; j < rows->len; j++)
			if (venture_entity_get_organization_id(g_ptr_array_index(rows, j)) == venture_entity_get_organization_id(member))
				return TRUE;
	}
	return FALSE;
}

gboolean
venture_mfa_web_gate(VentureContext *context, HtmxContext *http, const VentureAuthPrincipal *principal)
{
	VentureMfaService *self;
	HtmxRequest *request;
	HtmxResponse *response;
	const gchar *path;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != http, FALSE);
	if (NULL == principal || !principal->authenticated || principal->user_id <= 0)
		return FALSE;
	if (!venture_context_module_enabled(context, "mfa"))
		return FALSE;
	request = htmx_context_get_request(http);
	path = htmx_request_get_path(request);
	/* The routes a person needs to get out of the gate: enrolment itself,
	 * signing out, and their own account page. */
	if (g_str_has_prefix(path, "/account/mfa") || 0 == g_strcmp0(path, "/account") ||
	    0 == g_strcmp0(path, "/logout") || 0 == g_strcmp0(path, "/login") ||
	    g_str_has_prefix(path, "/login/") || 0 == g_strcmp0(path, "/look"))
		return FALSE;
	self = venture_mfa_service_get(venture_context_get_database(context));
	if (!venture_mfa_service_requires_enrolment(self, principal))
		return FALSE;
	if (g_str_has_prefix(path, "/api/") || principal->token_id > 0)
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *body = NULL;
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"This organization requires a second factor for owners and admins; enrol at /account/mfa/enrol");
		body = venture_json_error_to_string(error, FALSE);
		response = htmx_response_new_with_content(body);
		htmx_response_set_content_type(response, "application/json; charset=utf-8");
		htmx_response_set_status(response, 403);
	}
	else
	{
		response = htmx_response_new();
		htmx_response_set_status(response, 302);
		htmx_response_add_header(response, "Location", "/account/mfa/enrol");
	}
	htmx_context_set_response(http, response);
	return TRUE;
}

/* --- The break-glass action ----------------------------------------------------------------- */

static gboolean
reset_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;
	if (!module_on())
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "The mfa module is disabled on this install (modules.mfa.enabled)");
	if (NULL == entity || !VENTURE_IS_USER(entity))
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "mfa_reset acts on a user");
	return TRUE;
}

static VentureEntity *
reset_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureMfaService *self = venture_action_get_data(action);
	(void)params;
	if (!venture_mfa_service_reset(self, venture_entity_get_id(entity), actor, error))
		return NULL;
	return g_object_ref(entity);
}

void
venture_mfa_actions_register(VentureDatabase *database)
{
	VentureMfaService *self = venture_mfa_service_get(database);
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	if (self->actions)
		return;
	self->actions = TRUE;
	action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "user", "name", "mfa_reset",
		"label", "Reset second factor", "description", "Break glass: turn off this account's second factor and spend its recovery codes",
		"parameters", parameters, "stageable", FALSE, "type-level", FALSE, "service-transaction", TRUE,
		"roles", VENTURE_USER_ROLE_OWNER, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		reset_allowed, reset_invoke, self, NULL, &error))
		g_error("MFA action registration: %s", error->message);
}
