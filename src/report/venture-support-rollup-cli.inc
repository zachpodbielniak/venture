/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venturectl support rollup --from DATE --to DATE [--sort [-]COLUMN]
 *                           [min_tickets=N] [company=ID] [product=NAME]
 *                           [group_by=product] [organization_id=N]
 *
 * The support rollup for an inclusive span of days, through the same
 * /api/v1/reports/support_rollup route the web page and the CSV export
 * use: the verb only spells the span and hands over to the report command,
 * so -f csv and -f json behave exactly as `report` does.
 */
static gint
venture_cli_command_support(
	VentureCli	 *cli,
	gchar		**args,
	const gchar	 *from,
	const gchar	 *to,
	const gchar	 *sort,
	GError		**error
){
	g_autoptr(GPtrArray) forwarded = NULL;
	guint i;

	if ((g_strv_length(args) < 2) || (0 != g_strcmp0(args[1], "rollup")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: support rollup --from DATE --to DATE [--sort [-]COLUMN] "
			"[min_tickets=N] [company=ID] [product=NAME] [group_by=product] "
			"[organization_id=N]");
		return -1;
	}

	if (venture_string_is_empty(from) || venture_string_is_empty(to))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"support rollup needs both --from DATE and --to DATE (inclusive, "
			"YYYY-MM-DD)");
		return -1;
	}

	forwarded = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(forwarded, g_strdup("report"));
	g_ptr_array_add(forwarded, g_strdup("support_rollup"));
	g_ptr_array_add(forwarded, g_strdup_printf("%s..%s", from, to));

	if (!venture_string_is_empty(sort))
		g_ptr_array_add(forwarded, g_strdup_printf("sort=%s", sort));

	for (i = 2; NULL != args[i]; i++)
		g_ptr_array_add(forwarded, g_strdup(args[i]));

	g_ptr_array_add(forwarded, NULL);

	return venture_cli_command_report(cli, (gchar **)forwarded->pdata, error);
}
