/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_deal(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *path = NULL;
	gchar *end;
	gint64 deal_id, stage_id;
	if (NULL == args[1] || 0 != g_strcmp0(args[1], "move") || NULL == args[2] || NULL == args[3])
		goto invalid;
	deal_id = g_ascii_strtoll(args[2], &end, 10);
	if (deal_id <= 0 || '\0' != *end)
		goto invalid;
	stage_id = g_ascii_strtoll(args[3], &end, 10);
	if (stage_id <= 0 || '\0' != *end)
		goto invalid;
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "stage_id");
	json_builder_add_int_value(builder, stage_id);
	if (NULL != args[4])
	{
		json_builder_set_member_name(builder, "note");
		json_builder_add_string_value(builder, args[4]);
	}
	json_builder_end_object(builder);
	body = json_builder_get_root(builder);
	path = g_strdup_printf("/api/v1/deals/%" G_GINT64_FORMAT "/move", deal_id);
	result = venture_cli_request(cli, "POST", path, body, error);
	if (NULL == result)
		return -1;
	venture_cli_output(cli, result);
	return 0;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		"usage: venturectl deal move ID STAGE [NOTE]");
	return -1;
}
