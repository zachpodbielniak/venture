/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-settings.h - One settings API for the workflow settings
 *
 * Each feature registers its own workflow settings from its own file: a
 * key, a type, a default, its bounds or allowed values, a label and help
 * in plain words, who may change it, and where it is stored today. The
 * registry stores nothing itself. Reading a setting reads the record that
 * already holds it (a field on the organization, the email brand, the
 * first sending domain, an auto-reply rule, a web chat); setting one
 * validates the value against the registration and saves that record
 * through venture_database_save(), so the change is the one audit entry
 * that save writes, with its actor and the old and new values.
 *
 * The settings screen, the REST routes, venturectl and the assistant's
 * settings tools all come here. The assistant stages instead of saving
 * (venture_settings_stage()): the same record change, waiting in the
 * confirmation queue, saved by the same call when a person approves it.
 */
#ifndef VENTURE_SETTINGS_H
#define VENTURE_SETTINGS_H
G_BEGIN_DECLS

/**
 * VentureSettingType:
 * @VENTURE_SETTING_BOOLEAN: on or off
 * @VENTURE_SETTING_INTEGER: a whole number between a minimum and a maximum
 * @VENTURE_SETTING_TEXT: words, checked by the registration's check
 * @VENTURE_SETTING_CHOICE: one of a list of allowed values
 *
 * What kind of value a setting holds.
 */
typedef enum
{
	VENTURE_SETTING_BOOLEAN = 0,
	VENTURE_SETTING_INTEGER,
	VENTURE_SETTING_TEXT,
	VENTURE_SETTING_CHOICE
} VentureSettingType;

/**
 * VentureSettingPermission:
 * @VENTURE_SETTING_PERMISSION_ADMIN: an owner or administrator of the
 *   organization
 * @VENTURE_SETTING_PERMISSION_OWNER: an owner of the organization only
 *
 * Who may change a setting.
 */
typedef enum
{
	VENTURE_SETTING_PERMISSION_ADMIN = 0,
	VENTURE_SETTING_PERMISSION_OWNER
} VentureSettingPermission;

/**
 * VentureSettingMember:
 * @id: the record the member setting lives on
 * @label: (nullable): its name, for the screen
 *
 * One member of a setting family, such as one auto-reply rule.
 */
typedef struct
{
	gint64	 id;
	gchar	*label;
} VentureSettingMember;

/**
 * venture_setting_member_new:
 * @id: the record's id
 * @label: (nullable): its name
 *
 * Returns: (transfer full): a member
 */
VentureSettingMember *venture_setting_member_new(gint64 id, const gchar *label);

/**
 * venture_setting_member_free:
 * @member: (nullable): a member
 *
 * Frees a #VentureSettingMember.
 */
void venture_setting_member_free(VentureSettingMember *member);

/**
 * VentureSettingLocate:
 * @context: the context
 * @organization_id: the organization
 * @member: the family member's record id, 0 for a single setting
 * @for_write: whether the caller is about to change it; a storage that
 *   can make its record (the email brand) does so only then, unsaved
 * @error: (out) (optional): why there is nowhere to write it
 *
 * Finds the record that holds a setting. %NULL with no error means there
 * is none yet and the default applies.
 *
 * Returns: (transfer full) (nullable): the record
 */
typedef VentureEntity *(*VentureSettingLocate)(VentureContext *context, gint64 organization_id,
	gint64 member, gboolean for_write, GError **error);

/**
 * VentureSettingMembers:
 * @context: the context
 * @organization_id: the organization
 * @error: (out) (optional): failure
 *
 * Lists a family's members in one organization.
 *
 * Returns: (transfer container) (element-type VentureSettingMember)
 *   (nullable): the members
 */
typedef GPtrArray *(*VentureSettingMembers)(VentureContext *context, gint64 organization_id, GError **error);

/**
 * VentureSettingCheck:
 * @value: the value, already trimmed and of the right type
 * @error: (out) (optional): what is wrong with it, in plain words
 *
 * The registration's own check, beyond type, bounds and choices.
 *
 * Returns: whether @value is acceptable
 */
typedef gboolean (*VentureSettingCheck)(const gchar *value, GError **error);

/**
 * VentureSettingInfo:
 * @key: the setting's name, for example `telephony.quiet-hours`; for a
 *   family the prefix, each member being `KEY.ID`
 * @workflow: the group it is shown in, for example "Phone and text"
 * @module: (nullable): the module that must be on for it to be offered
 * @label: a short name in plain words
 * @help: what it does, in plain words
 * @type: the kind of value
 * @default_value: the value when nothing is set, as text
 * @minimum: an integer's least value
 * @maximum: an integer's greatest value
 * @choices: (nullable): a choice's allowed values, comma separated
 * @permission: who may change it
 * @stored_in: where it lives today, in words, for the screen and the docs
 * @field: the property on the record that holds it
 * @empty_value: (nullable): what the field holds when nothing is set (an
 *   empty string, usually); reading it gives the default, and resetting
 *   writes it. %NULL means the field always holds a value and resetting
 *   writes the default
 * @locate: (nullable): finds the record; %NULL is the organization itself
 * @members: (nullable): lists a family's members; %NULL for one setting
 * @check: (nullable): the extra check
 *
 * One registered setting. Registrations are static tables owned by the
 * feature that registers them.
 */
typedef struct
{
	const gchar			*key;
	const gchar			*workflow;
	const gchar			*module;
	const gchar			*label;
	const gchar			*help;
	VentureSettingType		 type;
	const gchar			*default_value;
	gint64				 minimum;
	gint64				 maximum;
	const gchar			*choices;
	VentureSettingPermission	 permission;
	const gchar			*stored_in;
	const gchar			*field;
	const gchar			*empty_value;
	VentureSettingLocate		 locate;
	VentureSettingMembers		 members;
	VentureSettingCheck		 check;
} VentureSettingInfo;

/**
 * VentureSettingValue:
 * @info: the registration
 * @key: the full key (a family member's ends in `.ID`)
 * @member: the family member's record id, 0 for a single setting
 * @member_label: (nullable): the member's name
 * @value: the value in force, as text
 * @stored: (nullable): what the record holds, as text; %NULL with no record
 * @is_default: whether @value is the default because nothing is set, or
 *   because what is set equals it
 *
 * A setting as it stands in one organization.
 */
typedef struct
{
	const VentureSettingInfo	*info;
	gchar				*key;
	gint64				 member;
	gchar				*member_label;
	gchar				*value;
	gchar				*stored;
	gboolean			 is_default;
} VentureSettingValue;

/**
 * venture_setting_value_free:
 * @value: (nullable): a value
 *
 * Frees a #VentureSettingValue.
 */
void venture_setting_value_free(VentureSettingValue *value);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSettingValue, venture_setting_value_free)

/**
 * venture_settings_register:
 * @info: a registration that outlives the process
 *
 * Adds a setting. Registering the same key again is ignored.
 */
void venture_settings_register(const VentureSettingInfo *info);

/**
 * venture_settings_registered:
 *
 * Every registration, in the order they were registered, whatever
 * module is on.
 *
 * Returns: (transfer container) (element-type VentureSettingInfo): the
 *   registrations
 */
GPtrArray *venture_settings_registered(void);

/**
 * venture_settings_lookup:
 * @key: a setting's key, or a family member's `KEY.ID`
 * @member: (out) (optional): the member's id, 0 for a single setting
 *
 * Returns: (transfer none) (nullable): the registration
 */
const VentureSettingInfo *venture_settings_lookup(const gchar *key, gint64 *member);

/**
 * venture_settings_validate:
 * @info: the registration
 * @value: the proposed value as text
 * @normalised: (out) (optional) (transfer full): the value as it will be
 *   stored: trimmed, `true`/`false` for a switch, a plain number
 * @error: (out) (optional): what is wrong, in plain words
 *
 * Checks a value against its registration: type, bounds, allowed values
 * and the registration's own check.
 *
 * Returns: whether it is acceptable
 */
gboolean venture_settings_validate(const VentureSettingInfo *info, const gchar *value, gchar **normalised,
	GError **error);

/**
 * venture_settings_list:
 * @context: the context
 * @organization_id: the organization
 * @error: (out) (optional): failure
 *
 * Every setting whose module is on, with family members expanded, in
 * registration order.
 *
 * Returns: (transfer container) (element-type VentureSettingValue)
 *   (nullable): the settings
 */
GPtrArray *venture_settings_list(VentureContext *context, gint64 organization_id, GError **error);

/**
 * venture_settings_get:
 * @context: the context
 * @organization_id: the organization
 * @key: the key
 * @error: (out) (optional): %VENTURE_ERROR_NOT_FOUND for no such setting,
 *   or one whose module is off
 *
 * Returns: (transfer full) (nullable): the setting
 */
VentureSettingValue *venture_settings_get(VentureContext *context, gint64 organization_id, const gchar *key,
	GError **error);

/**
 * venture_settings_require_manager:
 * @database: the database
 * @info: the registration
 * @organization_id: the organization
 * @error: (out) (optional): %VENTURE_ERROR_PERMISSION_DENIED
 *
 * Whether the acting principal (the access policy's) may change @info in
 * @organization_id. Internal work, with no principal, may.
 *
 * Returns: whether it may
 */
gboolean venture_settings_require_manager(VentureDatabase *database, const VentureSettingInfo *info,
	gint64 organization_id, GError **error);

/**
 * venture_settings_set:
 * @context: the context
 * @organization_id: the organization
 * @key: the key
 * @value: the new value as text
 * @actor: (nullable): who is changing it, for the audit entry
 * @error: (out) (optional): a refusal in plain words
 *
 * Validates, checks permission, and saves the record that holds the
 * setting: one audit entry. Setting the value it already has writes
 * nothing.
 *
 * Returns: (transfer full) (nullable): the setting as it now stands
 */
VentureSettingValue *venture_settings_set(VentureContext *context, gint64 organization_id, const gchar *key,
	const gchar *value, const VentureActor *actor, GError **error);

/**
 * venture_settings_reset:
 * @context: the context
 * @organization_id: the organization
 * @key: the key
 * @actor: (nullable): who is resetting it
 * @error: (out) (optional): a refusal
 *
 * Puts a setting back to its default, as venture_settings_set() would.
 *
 * Returns: (transfer full) (nullable): the setting as it now stands
 */
VentureSettingValue *venture_settings_reset(VentureContext *context, gint64 organization_id, const gchar *key,
	const VentureActor *actor, GError **error);

/**
 * venture_settings_stage:
 * @context: the context
 * @organization_id: the organization
 * @key: the key
 * @value: (nullable): the new value; %NULL resets it
 * @origin: who asked, and for the assistant the prompt
 * @error: (out) (optional): a refusal
 *
 * Like venture_settings_set(), but the record change waits in the
 * confirmation queue. Approving it saves the record with the same call,
 * and approval also needs somebody allowed to change the setting.
 *
 * Returns: (transfer none) (nullable): the waiting change
 */
VentureConfirmation *venture_settings_stage(VentureContext *context, gint64 organization_id, const gchar *key,
	const gchar *value, const VentureActor *origin, GError **error);

/**
 * venture_settings_check_approval:
 * @database: the database
 * @staged: the staged record
 * @via: the confirmation's `via`, `settings:KEY`
 * @error: (out) (optional): %VENTURE_ERROR_PERMISSION_DENIED
 *
 * Called by the confirmation queue before it saves a staged settings
 * change: the approver must be allowed to change that setting.
 *
 * Returns: whether the approval may go ahead
 */
gboolean venture_settings_check_approval(VentureDatabase *database, VentureEntity *staged, const gchar *via,
	GError **error);

/**
 * venture_settings_only_change:
 * @previous: (nullable): the row as stored
 * @row: the row about to be saved
 * @field: a property name
 *
 * Whether saving @row changes nothing but @field. A feature whose records
 * are written only by its own service lets its settings through with
 * this, after venture_settings_require_manager().
 *
 * Returns: whether @field is the only change
 */
gboolean venture_settings_only_change(VentureEntity *previous, VentureEntity *row, const gchar *field);

/**
 * venture_setting_value_to_json:
 * @value: a setting
 *
 * The wire form: key, workflow, label, help, type, value, default,
 * is_default, bounds or choices, permission and where it is stored.
 *
 * Returns: (transfer full): a JSON object
 */
JsonNode *venture_setting_value_to_json(const VentureSettingValue *value);

/**
 * venture_settings_value_from_json:
 * @node: (nullable): a JSON boolean, number or string
 *
 * Returns: (transfer full) (nullable): the value as text, %NULL for
 *   anything else
 */
gchar *venture_settings_value_from_json(JsonNode *node);

/* Each feature's registrations, in the feature's own file. The registry
 * asks for them once, the way the mail preview asks for its kinds. */
void venture_mail_preview_settings_register(void);
void venture_email_design_settings_register(void);
void venture_marketing_settings_register(void);
void venture_lead_ai_score_settings_register(void);
void venture_telephony_settings_register(void);
void venture_inbox_settings_register(void);
void venture_ai_messages_settings_register(void);

G_END_DECLS
#endif /* VENTURE_SETTINGS_H */
