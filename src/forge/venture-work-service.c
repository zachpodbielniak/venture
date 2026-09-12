/*
 * venture-work-service.c - The one background thread
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <ai-glib.h>
#include <glib/gstdio.h>

#include <string.h>
#include <stdlib.h>

/*
 * One unit of work, handed to the agent thread.
 *
 * Everything here is a copy. Nothing in this struct is a #VentureEntity or a
 * pointer into one, because the agent thread must not touch the object graph
 * the main thread owns -- see the header for why that is stronger than a
 * data race.
 */
typedef struct
{
	gint64	 run_id;
	gint64	 ticket_id;

	gchar	*workspace;
	gchar	*clone_url;
	gchar	*base_branch;
	gchar	*branch;

	gchar	*provider;
	gchar	*model;
	gchar	*system_prompt;
	gchar	*prompt;

	/* Used only to answer git's credential prompt over HTTPS, and only
	 * through the environment -- never argv, which is world-readable
	 * through /proc, and never the URL, which git writes into
	 * .git/config where it outlives the run. */
	gchar	*token;

	gchar	*git_path;
	gchar	*git_author_name;
	gchar	*git_author_email;
	gchar	*runner_command;
	gchar	*askpass;

	VentureForgeRunner	runner;
	VentureForgeRunOutcome	outcome;

	gint	 max_turns;
	gint	 timeout_seconds;
	gint	 git_timeout;
} VentureWorkJob;

/* Progress coming back the other way. Every field is optional; NULL and -1
 * mean "unchanged", so one shape carries every transition. */
typedef struct
{
	gint64			 run_id;
	VentureForgeRunState	 state;
	gboolean		 has_state;

	gchar			*branch;
	gchar			*summary;
	gchar			*failure_reason;
	gchar			*log;

	gint64			 input_tokens;
	gint64			 output_tokens;
	gint64			 turns;
	gboolean		 finished;
} VentureWorkUpdate;

enum
{
	VENTURE_WORK_SIGNAL_SESSION_OUTPUT,
	VENTURE_WORK_SIGNAL_SESSION_FINISHED,
	VENTURE_WORK_N_SIGNALS
};

static guint venture_work_signals[VENTURE_WORK_N_SIGNALS];

struct _VentureWorkService
{
	GObject parent_instance;

	VentureContext	*context;

	GThread		*thread;
	GMainContext	*agent_context;
	GMainLoop	*agent_loop;
	GAsyncQueue	*started;

	/* Live runs, by run id, holding a GCancellable each. Touched from
	 * both threads, so it has its own lock rather than relying on the
	 * agent context's serialisation. */
	GMutex		 lock;
	GHashTable	*live;

	guint		 max_concurrent;
	gboolean	 stopping;

	/*
	 * Sessions with a turn in flight, by session id, holding a
	 * #GCancellable each. Separate from @live because a session is
	 * interactive and must not be kept waiting behind the unattended
	 * runs, and shares the same lock because both cross threads.
	 */
	GHashTable	*session_live;
};

G_DEFINE_FINAL_TYPE(VentureWorkService, venture_work_service, G_TYPE_OBJECT)

static void
venture_work_job_free(VentureWorkJob *job)
{
	if (NULL == job)
		return;

	g_free(job->workspace);
	g_free(job->clone_url);

	if (NULL != job->token)
	{
		memset(job->token, 0, strlen(job->token));
		g_free(job->token);
	}

	g_free(job->base_branch);
	g_free(job->branch);
	g_free(job->provider);
	g_free(job->model);
	g_free(job->system_prompt);
	g_free(job->prompt);
	g_free(job->git_path);
	g_free(job->git_author_name);
	g_free(job->git_author_email);
	g_free(job->runner_command);
	g_free(job->askpass);
	g_free(job);
}

static void
venture_work_update_free(VentureWorkUpdate *update)
{
	if (NULL == update)
		return;

	g_free(update->branch);
	g_free(update->summary);
	g_free(update->failure_reason);
	g_free(update->log);
	g_free(update);
}

/* --- Applying progress, on the main thread ------------------------------- */

typedef struct
{
	VentureWorkService	*self;
	VentureWorkUpdate	*update;
} ApplyData;

/*
 * Writes an update onto its run record.
 *
 * Runs on the main thread, always. This is the only function in this file
 * that touches the database, and it is reached exclusively through
 * g_main_context_invoke_full() on the default context.
 */
static gboolean
venture_work_service_apply(gpointer user_data)
{
	ApplyData *data = user_data;
	VentureWorkService *self = data->self;
	VentureWorkUpdate *update = data->update;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;

	run = venture_database_get(venture_context_get_database(self->context),
	                           VENTURE_TYPE_FORGE_RUN, update->run_id, NULL);

	if (NULL == run)
		return G_SOURCE_REMOVE;

	if (update->has_state)
		g_object_set(run, "state", update->state, NULL);

	if (NULL != update->branch)
		g_object_set(run, "branch", update->branch, NULL);

	if (NULL != update->summary)
		g_object_set(run, "summary", update->summary, NULL);

	if (NULL != update->failure_reason)
	{
		/*
		 * Git echoes the remote URL on an authentication failure, and
		 * that URL can carry a credential. Redacted before it reaches
		 * a record anybody can read.
		 */
		g_autofree gchar *safe = NULL;

		safe = venture_string_redact_uri(update->failure_reason);
		g_object_set(run, "failure-reason", safe, NULL);
	}

	if (NULL != update->log)
	{
		g_autofree gchar *safe = NULL;

		safe = venture_string_redact_uri(update->log);
		g_object_set(run, "log", safe, NULL);
	}

	if (update->input_tokens > 0)
		g_object_set(run, "input-tokens", update->input_tokens, NULL);

	if (update->output_tokens > 0)
		g_object_set(run, "output-tokens", update->output_tokens, NULL);

	if (update->turns > 0)
		g_object_set(run, "turns", update->turns, NULL);

	if (update->finished)
	{
		now = g_date_time_new_now_utc();
		g_object_set(run, "finished-at", now, NULL);

		g_mutex_lock(&self->lock);
		g_hash_table_remove(self->live, GINT_TO_POINTER(update->run_id));
		g_mutex_unlock(&self->lock);
	}

	actor.kind = VENTURE_ACTOR_KIND_AUTOMATION;
	actor.name = "forge-run";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;

	if (!venture_database_save(venture_context_get_database(self->context),
	                           run, &actor, &error))
	{
		g_warning("venture_work_service: cannot record run #%"
		          G_GINT64_FORMAT ": %s", update->run_id, error->message);
	}

	return G_SOURCE_REMOVE;
}

static void
venture_work_service_apply_free(gpointer user_data)
{
	ApplyData *data = user_data;

	g_clear_object(&data->self);
	venture_work_update_free(data->update);
	g_free(data);
}

/*
 * Posts an update to the main thread.
 *
 * Called from the agent thread. Ownership of @update transfers.
 */
static void
venture_work_service_post(
	VentureWorkService	*self,
	VentureWorkUpdate	*update
){
	ApplyData *data;

	data = g_new0(ApplyData, 1);
	data->self = g_object_ref(self);
	data->update = update;

	g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT,
	                           venture_work_service_apply, data,
	                           venture_work_service_apply_free);
}

static VentureWorkUpdate *
venture_work_update_new(
	gint64			 run_id,
	VentureForgeRunState	 state
){
	VentureWorkUpdate *update;

	update = g_new0(VentureWorkUpdate, 1);
	update->run_id = run_id;
	update->state = state;
	update->has_state = TRUE;

	return update;
}

/* --- git, on the agent thread -------------------------------------------- */

/*
 * Runs one git invocation and waits for it, on the agent context.
 *
 * argv, never a command line. Branch names and repository names on this path
 * can originate in a webhook payload, and a command line would be
 * word-split -- which turns a branch called `--upload-pack=…` into an option
 * git obeys. An argv array has no such reading.
 */
static gboolean
venture_work_git(
	const VentureWorkJob	 *job,
	const gchar		 *cwd,
	const gchar *const	 *args,
	gchar			**out_output,
	GError			**error
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = NULL;
	g_autofree gchar *output = NULL;
	gsize i;

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, job->git_path);

	for (i = 0; NULL != args[i]; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);

	g_ptr_array_add(argv, NULL);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_MERGE);

	if (NULL != cwd)
		g_subprocess_launcher_set_cwd(launcher, cwd);

	/*
	 * A git that cannot find a credential must fail rather than block
	 * forever on a terminal read, and the server's own ~/.gitconfig and
	 * ~/.ssh must not leak into a run.
	 */
	g_subprocess_launcher_setenv(launcher, "GIT_TERMINAL_PROMPT", "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "SSH_ASKPASS", "/bin/true", TRUE);

	/*
	 * Over HTTPS git needs the access token, and there are only bad
	 * places to put it: argv is readable by every process on the host
	 * through /proc, and a token embedded in the remote URL is written
	 * into .git/config, where it outlives the run and lands in any copy
	 * of the tree. An askpass helper reads it from this process's
	 * environment instead, which is 0400 to the same uid.
	 *
	 * Over SSH none of this applies -- the key agent answers -- and the
	 * helper is simply never called.
	 */
	if (!venture_string_is_empty(job->askpass) &&
	    !venture_string_is_empty(job->token))
	{
		g_subprocess_launcher_setenv(launcher, "GIT_ASKPASS", job->askpass,
		                             TRUE);
		g_subprocess_launcher_setenv(launcher, "VENTURE_FORGE_TOKEN",
		                             job->token, TRUE);
	}
	else
	{
		g_subprocess_launcher_setenv(launcher, "GIT_ASKPASS", "/bin/true",
		                             TRUE);
	}
	g_subprocess_launcher_setenv(launcher, "GIT_CONFIG_NOSYSTEM", "1", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_CONFIG_GLOBAL", "/dev/null",
	                             TRUE);
	g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_AUTHOR_NAME",
	                             job->git_author_name, TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_AUTHOR_EMAIL",
	                             job->git_author_email, TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_COMMITTER_NAME",
	                             job->git_author_name, TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_COMMITTER_EMAIL",
	                             job->git_author_email, TRUE);

	process = g_subprocess_launcher_spawnv(launcher,
	                                       (const gchar *const *)argv->pdata,
	                                       error);

	if (NULL == process)
		return FALSE;

	if (!g_subprocess_communicate_utf8(process, NULL, NULL, &output, NULL,
	                                   error))
		return FALSE;

	if (NULL != out_output)
		*out_output = g_strdup((NULL != output) ? output : "");

	if (!g_subprocess_get_successful(process))
	{
		g_autofree gchar *safe = NULL;

		safe = venture_string_redact_uri((NULL != output) ? output : "");

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "git %s failed: %s", args[0], safe);
		return FALSE;
	}

	return TRUE;
}

/*
 * Prepares the checkout a run works in.
 *
 * A plain clone of the one branch, not a mirror: a run touches one branch
 * and a shallow single-branch clone is the cheapest thing that can still
 * produce a mergeable commit.
 */
static gboolean
venture_work_prepare(
	VentureWorkJob	 *job,
	GError		**error
){
	const gchar *clone_args[8];
	const gchar *branch_args[4];
	gsize n = 0;

	if (0 != g_mkdir_with_parents(job->workspace, 0700))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Cannot create the workspace at %s", job->workspace);
		return FALSE;
	}

	/*
	 * The askpass helper, written beside the checkout at 0700.
	 *
	 * A file rather than an inline -c option because every alternative
	 * puts the token somewhere durable: argv, .git/config, or a
	 * credential store. This one is readable only by this uid and is
	 * removed with the workspace.
	 */
	if (!venture_string_is_empty(job->token))
	{
		g_autoptr(GError) local_error = NULL;
		static const gchar *const helper =
			"#!/bin/sh\n"
			"printf '%s\\n' \"$VENTURE_FORGE_TOKEN\"\n";

		g_clear_pointer(&job->askpass, g_free);
		job->askpass = g_build_filename(job->workspace, ".venture-askpass",
		                                NULL);

		if (!g_file_set_contents(job->askpass, helper, -1, &local_error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
			            "Cannot write the credential helper: %s",
			            local_error->message);
			return FALSE;
		}

		g_chmod(job->askpass, 0700);
	}

	clone_args[n++] = "clone";
	clone_args[n++] = "--single-branch";

	if (!venture_string_is_empty(job->base_branch))
	{
		clone_args[n++] = "--branch";
		clone_args[n++] = job->base_branch;
	}

	clone_args[n++] = "--";
	clone_args[n++] = job->clone_url;
	clone_args[n++] = job->workspace;
	clone_args[n] = NULL;

	if (!venture_work_git(job, NULL, clone_args, NULL, error))
		return FALSE;

	branch_args[0] = "checkout";
	branch_args[1] = "-b";
	branch_args[2] = job->branch;
	branch_args[3] = NULL;

	return venture_work_git(job, job->workspace, branch_args, NULL, error);
}

/*
 * Removes a workspace.
 *
 * A hand-written recursive delete rather than `rm -rf` through a subprocess:
 * this runs on unwind paths where spawning may itself be what failed, and a
 * path-construction bug that reached `rm -rf` would be unrecoverable. The
 * guard refuses any path that is not under the configured workspace root.
 */
static void
venture_work_remove_tree(
	const gchar	*path,
	const gchar	*root
){
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileEnumerator) children = NULL;
	g_autofree gchar *bounded = NULL;

	if (venture_string_is_empty(path) || venture_string_is_empty(root))
		return;

	bounded = g_strconcat(root, G_DIR_SEPARATOR_S, NULL);

	if (!g_str_has_prefix(path, bounded))
	{
		g_warning("venture_work_service: refusing to remove \"%s\", which is "
		          "outside the workspace root", path);
		return;
	}

	file = g_file_new_for_path(path);
	children = g_file_enumerate_children(file,
		G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);

	if (NULL != children)
	{
		while (TRUE)
		{
			g_autoptr(GFileInfo) info = NULL;
			g_autofree gchar *child = NULL;

			info = g_file_enumerator_next_file(children, NULL, NULL);

			if (NULL == info)
				break;

			child = g_build_filename(path, g_file_info_get_name(info), NULL);

			if (G_FILE_TYPE_DIRECTORY == g_file_info_get_file_type(info))
				venture_work_remove_tree(child, root);
			else
				g_unlink(child);
		}

		g_file_enumerator_close(children, NULL, NULL);
	}

	g_rmdir(path);
}

/* --- The run, on the agent thread ---------------------------------------- */

typedef struct
{
	VentureWorkService	*self;
	VentureWorkJob		*job;
	GCancellable		*cancellable;
} RunData;

/*
 * Builds the provider a run uses.
 *
 * Through ai-glib's factory rather than a switch of our own: it already
 * knows every provider name, including the CLI-backed claude-code, opencode
 * and grok-build, and it refuses a name it does not recognise instead of
 * quietly answering Claude. A run on the wrong model is worse than a run
 * that did not start.
 */
static AiProvider *
venture_work_provider(
	const VentureWorkJob	 *job,
	GError			**error
){
	GObject *object;

	object = ai_provider_factory_new_from_string(job->provider, NULL, error);

	if (NULL == object)
		return NULL;

	if (!venture_string_is_empty(job->model) && AI_IS_CLIENT(object))
		ai_client_set_model(AI_CLIENT(object), job->model);

	/*
	 * The CLI runner's whole distinction: the subprocess is started in
	 * the checkout, so its own file tools operate there without VENTURE
	 * providing any.
	 */
	if (AI_IS_CLI_CLIENT(object))
	{
		ai_cli_client_set_working_directory(AI_CLI_CLIENT(object),
		                                    job->workspace);
	}

	return AI_PROVIDER(object);
}

/*
 * Does the work, on the agent thread.
 *
 * Written as one straight-line function because that is what it is: prepare,
 * run, record. Each step posts its state before it starts, so a run that
 * hangs is visible as the step it hung in rather than as silence.
 */
static gboolean
venture_work_run(gpointer user_data)
{
	RunData *data = user_data;
	VentureWorkService *self = data->self;
	VentureWorkJob *job = data->job;
	g_autoptr(AiProvider) provider = NULL;
	g_autoptr(AiToolExecutor) executor = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *workspace_root = NULL;
	VentureWorkUpdate *update;
	GList *messages = NULL;

	workspace_root = g_path_get_dirname(job->workspace);

	venture_work_service_post(self,
		venture_work_update_new(job->run_id, VENTURE_FORGE_RUN_STATE_RUNNING));

	/* Prepare. */
	if (!venture_work_prepare(job, &error))
	{
		update = venture_work_update_new(job->run_id,
		                                 VENTURE_FORGE_RUN_STATE_FAILED);
		update->failure_reason = g_strdup(error->message);
		update->finished = TRUE;
		venture_work_service_post(self, update);
		venture_work_remove_tree(job->workspace, workspace_root);
		return G_SOURCE_REMOVE;
	}

	{
		VentureWorkUpdate *branch_update;

		branch_update = g_new0(VentureWorkUpdate, 1);
		branch_update->run_id = job->run_id;
		branch_update->branch = g_strdup(job->branch);
		venture_work_service_post(self, branch_update);
	}

	provider = venture_work_provider(job, &error);

	if (NULL == provider)
	{
		update = venture_work_update_new(job->run_id,
		                                 VENTURE_FORGE_RUN_STATE_FAILED);
		update->failure_reason = g_strdup(error->message);
		update->finished = TRUE;
		venture_work_service_post(self, update);
		venture_work_remove_tree(job->workspace, workspace_root);
		return G_SOURCE_REMOVE;
	}

	/*
	 * Empty, then VENTURE's own confined tools -- and only for the
	 * in-process runner. ai_tool_executor_new() would bring ai-glib's
	 * built-ins, whose file tools accept absolute paths unchanged and
	 * whose bash tool is a shell in the server's process. The CLI runner
	 * gets an empty executor and nothing else: its subprocess carries its
	 * own tools, and anything registered here would be a grant nobody
	 * decided to make.
	 */
	executor = ai_tool_executor_new_empty();

	if (VENTURE_FORGE_RUNNER_AGENT == job->runner)
		venture_work_tools_register(executor, job->workspace);

	messages = g_list_append(messages, ai_message_new_user(job->prompt));

	reply = ai_tool_executor_run(executor, provider, messages,
	                             job->system_prompt, 4096,
	                             data->cancellable, &error);

	g_list_free_full(messages, g_object_unref);

	if (g_cancellable_is_cancelled(data->cancellable))
	{
		update = venture_work_update_new(job->run_id,
		                                 VENTURE_FORGE_RUN_STATE_CANCELLED);
		update->finished = TRUE;
		venture_work_service_post(self, update);
		venture_work_remove_tree(job->workspace, workspace_root);
		return G_SOURCE_REMOVE;
	}

	if (NULL == reply)
	{
		update = venture_work_update_new(job->run_id,
		                                 VENTURE_FORGE_RUN_STATE_FAILED);
		update->failure_reason = g_strdup((NULL != error) ? error->message
		                                                  : "the run failed");
		update->finished = TRUE;
		venture_work_service_post(self, update);
		return G_SOURCE_REMOVE;
	}

	/* Commit whatever changed. Nothing changing is a result, not a
	 * failure: a model that read the code and concluded there was
	 * nothing to do has answered the question. */
	{
		g_autofree gchar *status = NULL;
		static const gchar *const status_args[] = {
			"status", "--porcelain", NULL
		};

		if (venture_work_git(job, job->workspace, status_args, &status, NULL) &&
		    !venture_string_is_empty(status))
		{
			g_autofree gchar *subject = NULL;
			const gchar *add_args[] = { "add", "-A", NULL };
			const gchar *commit_args[6];

			subject = g_strdup_printf("%.72s", reply);
			g_strdelimit(subject, "\n\r", ' ');

			venture_work_git(job, job->workspace, add_args, NULL, NULL);

			commit_args[0] = "commit";
			commit_args[1] = "-m";
			commit_args[2] = subject;
			commit_args[3] = NULL;

			venture_work_git(job, job->workspace, commit_args, NULL, NULL);

			if (VENTURE_FORGE_RUN_OUTCOME_PUSH_BRANCH == job->outcome ||
			    VENTURE_FORGE_RUN_OUTCOME_DRAFT_PR == job->outcome)
			{
				const gchar *push_args[] = {
					"push", "-u", "origin", job->branch, NULL
				};

				venture_work_git(job, job->workspace, push_args, NULL, NULL);
			}
		}
	}

	update = venture_work_update_new(job->run_id,
	                                 VENTURE_FORGE_RUN_STATE_SUCCEEDED);
	update->summary = g_strdup(reply);
	update->finished = TRUE;
	venture_work_service_post(self, update);

	/* The tree is kept when the run failed and removed when it did not:
	 * the usual reason to look at a checkout is that something went
	 * wrong with it. */
	venture_work_remove_tree(job->workspace, workspace_root);

	return G_SOURCE_REMOVE;
}

static void
venture_work_run_free(gpointer user_data)
{
	RunData *data = user_data;

	g_clear_object(&data->self);
	g_clear_object(&data->cancellable);
	venture_work_job_free(data->job);
	g_free(data);
}

/* --- The thread ---------------------------------------------------------- */

static gpointer
venture_work_service_thread(gpointer user_data)
{
	VentureWorkService *self = user_data;

	g_main_context_push_thread_default(self->agent_context);
	g_async_queue_push(self->started, GINT_TO_POINTER(1));
	g_main_loop_run(self->agent_loop);
	g_main_context_pop_thread_default(self->agent_context);

	return NULL;
}

/* --- Lifecycle ----------------------------------------------------------- */

/*
 * Reconciles runs left in flight by a previous process.
 *
 * Nothing is resumed. A run whose outcome nobody observed may have pushed a
 * branch, opened a pull request, or done nothing at all, and re-running it
 * could repeat whichever of those it managed. Interrupted is its own state
 * for exactly that reason -- it is not a failure, it is an absence of
 * knowledge, and the operator decides what to do about it.
 */
static void
venture_work_service_reconcile(VentureWorkService *self)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) runs = NULL;
	guint i;

	query = venture_query_new(VENTURE_TYPE_FORGE_RUN);

	if (!venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_IN,
	                                     "queued", NULL))
		return;

	runs = venture_database_find(venture_context_get_database(self->context),
	                             query, NULL);

	if (NULL == runs)
		return;

	for (i = 0; i < runs->len; i++)
	{
		VentureEntity *run = g_ptr_array_index(runs, i);
		VentureActor actor;

		g_object_set(run, "state", VENTURE_FORGE_RUN_STATE_INTERRUPTED,
		             "failure-reason",
		             "The server stopped while this run was in progress",
		             NULL);

		actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
		actor.name = "startup";
		actor.prompt = NULL;
		actor.request_id = NULL;
		actor.approved_by = NULL;

		venture_database_save(venture_context_get_database(self->context),
		                      run, &actor, NULL);
	}
}

static void
venture_work_service_finalize(GObject *object)
{
	VentureWorkService *self = VENTURE_WORK_SERVICE(object);

	self->stopping = TRUE;

	if (NULL != self->agent_loop)
		g_main_loop_quit(self->agent_loop);

	if (NULL != self->thread)
	{
		g_thread_join(self->thread);
		self->thread = NULL;
	}

	g_clear_pointer(&self->agent_loop, g_main_loop_unref);
	g_clear_pointer(&self->agent_context, g_main_context_unref);
	g_clear_pointer(&self->started, g_async_queue_unref);
	g_clear_pointer(&self->live, g_hash_table_unref);
	g_clear_pointer(&self->session_live, g_hash_table_unref);
	g_mutex_clear(&self->lock);
	g_clear_object(&self->context);

	G_OBJECT_CLASS(venture_work_service_parent_class)->finalize(object);
}

static void
venture_work_service_class_init(VentureWorkServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_work_service_finalize;

	/**
	 * VentureWorkService::session-output:
	 * @self: the service
	 * @session_id: which session
	 * @text: a fragment of what the agent is saying
	 *
	 * Emitted on the main thread as a session's turn produces output.
	 *
	 * A turn takes minutes and a CLI agent narrates the whole way; a
	 * harness that showed nothing until it finished would be
	 * indistinguishable from one that had hung. The fragments are not
	 * stored -- the finished turn is, once -- so a subscriber that
	 * misses some has lost nothing but the watching.
	 */
	venture_work_signals[VENTURE_WORK_SIGNAL_SESSION_OUTPUT] =
		g_signal_new("session-output", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 2, G_TYPE_INT64, G_TYPE_STRING);

	/**
	 * VentureWorkService::session-finished:
	 * @self: the service
	 * @session_id: which session
	 * @failure: (nullable): why it failed, or %NULL if it did not
	 *
	 * Emitted on the main thread when a turn is over and its record is
	 * written.
	 */
	venture_work_signals[VENTURE_WORK_SIGNAL_SESSION_FINISHED] =
		g_signal_new("session-finished", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 2, G_TYPE_INT64, G_TYPE_STRING);
}

static void
venture_work_service_init(VentureWorkService *self)
{
	g_mutex_init(&self->lock);
	self->live = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                   g_object_unref);
	self->session_live = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                           NULL, g_object_unref);
	self->max_concurrent = 1;
}

VentureWorkService *
venture_work_service_new(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(VentureWorkService) self = NULL;
	gboolean runs_enabled = FALSE;
	gint64 concurrency = 1;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	g_object_get(venture_context_get_config(context),
	             "forge-runs-enabled", &runs_enabled,
	             "forge-run-concurrency", &concurrency, NULL);

	if (!venture_context_module_enabled(context, "forge"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "The forge module is disabled in configuration");
		return NULL;
	}

	if (!runs_enabled)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Coding runs are turned off");
		return NULL;
	}

	self = g_object_new(VENTURE_TYPE_WORK_SERVICE, NULL);
	self->context = g_object_ref(context);
	self->max_concurrent = (concurrency > 0) ? (guint)concurrency : 1;

	/* Before the thread exists, so nothing races with it. */
	venture_work_service_reconcile(self);

	self->agent_context = g_main_context_new();
	self->agent_loop = g_main_loop_new(self->agent_context, FALSE);
	self->started = g_async_queue_new();
	self->thread = g_thread_new("venture-work", venture_work_service_thread,
	                            self);

	/* Wait for the loop to be running before accepting work, so an early
	 * job cannot be attached to a context nobody is iterating. */
	g_async_queue_pop(self->started);

	return g_steal_pointer(&self);
}

guint
venture_work_service_count_live(VentureWorkService *self)
{
	guint count;

	g_return_val_if_fail(VENTURE_IS_WORK_SERVICE(self), 0);

	g_mutex_lock(&self->lock);
	count = g_hash_table_size(self->live);
	g_mutex_unlock(&self->lock);

	return count;
}

gboolean
venture_work_service_cancel(
	VentureWorkService	*self,
	gint64			 run_id
){
	GCancellable *cancellable;

	g_return_val_if_fail(VENTURE_IS_WORK_SERVICE(self), FALSE);

	g_mutex_lock(&self->lock);
	cancellable = g_hash_table_lookup(self->live, GINT_TO_POINTER(run_id));

	if (NULL != cancellable)
		g_cancellable_cancel(cancellable);

	g_mutex_unlock(&self->lock);

	return (NULL != cancellable);
}

gint64
venture_work_service_start_for_ticket(
	VentureWorkService	 *self,
	gint64			  ticket_id,
	const gchar		 *requested_by,
	GError			**error
){
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) repo = NULL;
	g_autoptr(VentureForgeRule) rule = NULL;
	g_autoptr(VentureForgeRun) run = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *description = NULL;
	g_autofree gchar *repo_name = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *default_branch = NULL;
	g_autofree gchar *repo_clone_url = NULL;
	g_autofree gchar *forge_clone_base = NULL;
	g_autofree gchar *forge_base_url = NULL;
	g_autofree gchar *forge_token = NULL;
	g_autoptr(VentureEntity) forge = NULL;
	g_autofree gchar *rule_prompt = NULL;
	g_autofree gchar *state_dir_setting = NULL;
	VentureWorkJob *job;
	RunData *data;
	VentureActor actor;
	VentureIssueType issue_type = VENTURE_ISSUE_TYPE_TASK;
	VentureForgeRunner runner = VENTURE_FORGE_RUNNER_AGENT;
	VentureForgeRunOutcome outcome = VENTURE_FORGE_RUN_OUTCOME_DRAFT_PR;
	gint64 repo_id = 0;
	gint64 forge_id = 0;
	gint64 max_turns = 0;

	g_return_val_if_fail(VENTURE_IS_WORK_SERVICE(self), 0);

	g_mutex_lock(&self->lock);

	if (g_hash_table_size(self->live) >= self->max_concurrent)
	{
		g_mutex_unlock(&self->lock);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "Too many runs are already going");
		return 0;
	}

	g_mutex_unlock(&self->lock);

	ticket = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_TICKET, ticket_id, error);

	if (NULL == ticket)
		return 0;

	g_object_get(ticket, "repo-id", &repo_id, "issue-type", &issue_type,
	             "title", &title, "description", &description, NULL);

	if (0 == repo_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "That ticket names no repository");
		return 0;
	}

	/* The budget is asked before the rule: a run refused for money is
	 * refused whatever the rule says, and the refusal names the budget. */
	if (!venture_factory_budget_allows_run(self->context, repo_id, error))
		return 0;

	rule = venture_forge_rule_resolve(venture_context_get_database(self->context),
	                                  repo_id, issue_type, NULL);

	if (NULL == rule)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "No rule covers that ticket, so nothing runs");
		return 0;
	}

	g_object_get(rule, "runner", &runner, "outcome", &outcome,
	             "max-turns", &max_turns, "prompt", &rule_prompt, NULL);

	repo = venture_database_get(venture_context_get_database(self->context),
	                            VENTURE_TYPE_FORGE_REPO, repo_id, error);

	if (NULL == repo)
		return 0;

	g_object_get(repo, "name", &repo_name, "branch-prefix", &prefix,
	             "default-branch", &default_branch,
	             "clone-url", &repo_clone_url, "forge-id", &forge_id, NULL);

	forge = venture_database_get(venture_context_get_database(self->context),
	                             VENTURE_TYPE_FORGE, forge_id, error);

	if (NULL == forge)
		return 0;

	g_object_get(forge, "clone-base-url", &forge_clone_base,
	             "base-url", &forge_base_url, "token", &forge_token, NULL);

	now = g_date_time_new_now_utc();

	run = venture_forge_run_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(run),
		venture_entity_get_organization_id(ticket));
	g_object_set(run,
	             "ticket-id", ticket_id,
	             "rule-id", venture_entity_get_id(VENTURE_ENTITY(rule)),
	             "state", VENTURE_FORGE_RUN_STATE_QUEUED,
	             "runner", runner,
	             "outcome", outcome,
	             "started-at", now,
	             NULL);

	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = requested_by;
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(run), &actor, error))
		return 0;

	job = g_new0(VentureWorkJob, 1);
	job->run_id = venture_entity_get_id(VENTURE_ENTITY(run));
	job->ticket_id = ticket_id;
	job->runner = runner;
	job->outcome = outcome;
	job->max_turns = (gint)max_turns;

	g_object_get(venture_context_get_config(self->context),
	             "forge-git-path", &job->git_path,
	             "forge-git-author-name", &job->git_author_name,
	             "forge-git-author-email", &job->git_author_email,
	             "state-dir", &state_dir_setting,
	             NULL);

	job->branch = venture_forge_branch_name(NULL, prefix, issue_type,
	                                        ticket_id, 0, title);
	job->base_branch = g_strdup(default_branch);

	/*
	 * Where git clones from, which is routinely not where the API lives:
	 * a Forgejo behind a reverse proxy answers HTTPS on one host and SSH
	 * on another. Composed from the repository's own URL if it has one,
	 * then the forge's clone base, then the API base.
	 */
	job->clone_url = venture_forge_clone_url(repo_clone_url, forge_clone_base,
	                                         forge_base_url, repo_name);

	if (NULL == job->clone_url)
	{
		venture_work_job_free(job);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Cannot work out where to clone \"%s\" from; set a "
		            "clone base on the forge or a clone URL on the "
		            "repository", repo_name);
		return 0;
	}

	/* Only for an HTTPS clone: over SSH the key agent answers and the
	 * helper is never called. */
	if (NULL != strstr(job->clone_url, "://"))
		job->token = g_strdup(forge_token);
	job->workspace = g_build_filename(
		venture_config_get_state_dir(venture_context_get_config(self->context)),
		"forge", "work", job->branch, NULL);

	job->provider = g_strdup("claude");
	job->model = NULL;
	job->system_prompt = g_strdup(
		"You are working in a git checkout. Make the smallest change that "
		"fixes the problem described, and explain what you changed in one "
		"line first.");
	job->prompt = g_strdup_printf("%s%s%s\n\n%s",
	                              (NULL != rule_prompt) ? rule_prompt : "",
	                              (NULL != rule_prompt) ? "\n\n" : "",
	                              (NULL != title) ? title : "",
	                              (NULL != description) ? description : "");

	data = g_new0(RunData, 1);
	data->self = g_object_ref(self);
	data->job = job;
	data->cancellable = g_cancellable_new();

	g_mutex_lock(&self->lock);
	g_hash_table_insert(self->live, GINT_TO_POINTER(job->run_id),
	                    g_object_ref(data->cancellable));
	g_mutex_unlock(&self->lock);

	/* Onto the agent thread. Everything after this point happens there. */
	g_main_context_invoke_full(self->agent_context, G_PRIORITY_DEFAULT,
	                           venture_work_run, data, venture_work_run_free);

	return job->run_id;
}


/* ==========================================================================
 * Harness sessions
 *
 * The other half of the same machinery. A run is one shot and unattended;
 * a session is the same providers, on the same thread, driven a turn at a
 * time by somebody reading the output.
 *
 * Every turn rebuilds everything -- the provider, the executor and the
 * message list, from the turns stored on the record. Nothing about a
 * session lives in memory between turns except a #GCancellable, which is
 * what makes a session survive a restart: there is no state to lose. It
 * also means the two threads exchange nothing but plain data, which is
 * rule three at the top of this file and the one that keeps the agent
 * thread away from the database.
 * ========================================================================== */

/* One stored turn, crossing to the agent thread as plain data. */
typedef struct
{
	gboolean	 from_user;
	gchar		*body;
} SessionMessage;

static void
session_message_free(gpointer data)
{
	SessionMessage *message = data;

	if (NULL == message)
		return;

	g_free(message->body);
	g_free(message);
}

/* Everything one turn needs, assembled on the main thread. */
typedef struct
{
	VentureWorkService	*self;
	gint64			 session_id;
	gchar			*provider;
	gchar			*model;
	gchar			*workspace;
	gchar			*system_prompt;
	GPtrArray		*history;
	GCancellable		*cancellable;
	gint			 max_turns;
} SessionTurn;

static void
session_turn_free(gpointer data)
{
	SessionTurn *turn = data;

	if (NULL == turn)
		return;

	g_clear_object(&turn->self);
	g_clear_object(&turn->cancellable);
	g_free(turn->provider);
	g_free(turn->model);
	g_free(turn->workspace);
	g_free(turn->system_prompt);
	g_clear_pointer(&turn->history, g_ptr_array_unref);
	g_free(turn);
}

/* What comes back: a fragment while it runs, or the finished turn. */
typedef struct
{
	VentureWorkService	*self;
	gint64			 session_id;
	gchar			*text;
	gchar			*failure;
	gint64			 input_tokens;
	gint64			 output_tokens;
	gboolean		 finished;
} SessionUpdate;

static void
session_update_free(gpointer data)
{
	SessionUpdate *update = data;

	if (NULL == update)
		return;

	g_clear_object(&update->self);
	g_free(update->text);
	g_free(update->failure);
	g_free(update);
}

/*
 * Applies a session update. On the main thread, always -- this and
 * venture_work_service_apply() are the only functions here that touch a
 * record.
 */
static gboolean
venture_work_session_apply(gpointer user_data)
{
	SessionUpdate *update = user_data;
	VentureWorkService *self = update->self;
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(GDateTime) now = NULL;
	VentureActor actor;

	/* A fragment is only ever watched, never stored. */
	if (!update->finished)
	{
		g_signal_emit(self,
		              venture_work_signals[VENTURE_WORK_SIGNAL_SESSION_OUTPUT],
		              0, update->session_id, update->text);
		return G_SOURCE_REMOVE;
	}

	session = venture_database_get(venture_context_get_database(self->context),
	                               VENTURE_TYPE_AGENT_SESSION,
	                               update->session_id, NULL);

	if (NULL == session)
		return G_SOURCE_REMOVE;

	now = venture_time_now();
	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = "harness";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;

	/* The reply, stored as the turn it is. A turn that failed leaves no
	 * assistant turn: there is nothing it said. */
	if (venture_string_is_empty(update->failure) &&
	    !venture_string_is_empty(update->text))
	{
		g_autoptr(VentureAgentTurn) reply = NULL;

		reply = venture_agent_turn_new();
		g_object_set(reply, "session-id", update->session_id,
		             "role", VENTURE_CHAT_ROLE_ASSISTANT,
		             "body", update->text, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(reply),
			venture_entity_get_organization_id(session));
		venture_database_save(venture_context_get_database(self->context),
		                      VENTURE_ENTITY(reply), &actor, NULL);
	}

	{
		gint64 turns = 0;
		gint64 input = 0;
		gint64 output = 0;

		g_object_get(session, "turns", &turns, "input-tokens", &input,
		             "output-tokens", &output, NULL);

		g_object_set(session,
			"state", venture_string_is_empty(update->failure)
				? VENTURE_AGENT_SESSION_STATE_IDLE
				: VENTURE_AGENT_SESSION_STATE_FAILED,
			"turns", turns + 1,
			"input-tokens", input + update->input_tokens,
			"output-tokens", output + update->output_tokens,
			"last-activity-at", now, NULL);

		if (!venture_string_is_empty(update->failure))
			g_object_set(session, "failure-reason", update->failure, NULL);
	}

	venture_database_save(venture_context_get_database(self->context),
	                      VENTURE_ENTITY(session), &actor, NULL);

	g_mutex_lock(&self->lock);
	g_hash_table_remove(self->session_live,
	                    GINT_TO_POINTER(update->session_id));
	g_mutex_unlock(&self->lock);

	g_signal_emit(self,
	              venture_work_signals[VENTURE_WORK_SIGNAL_SESSION_FINISHED],
	              0, update->session_id, update->failure);

	return G_SOURCE_REMOVE;
}

static void
venture_work_session_post(
	VentureWorkService	*self,
	SessionUpdate		*update
){
	update->self = g_object_ref(self);
	g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT,
	                           venture_work_session_apply, update,
	                           session_update_free);
}

/* Each fragment the provider emits, on its way to the browser. */
static void
venture_work_session_on_event(
	AiEventSource	*source,
	AiEvent		*event,
	gpointer	 user_data
){
	SessionTurn *turn = user_data;
	SessionUpdate *update;
	const gchar *text;

	(void)source;

	switch (ai_event_get_kind(event))
	{
	case AI_EVENT_TEXT_DELTA:
		text = ai_event_get_text(event);
		break;

	case AI_EVENT_TOOL_STARTED:
	{
		AiToolUse *use;

		use = ai_event_get_tool_use(event);
		text = (NULL != use) ? ai_tool_use_get_name(use) : NULL;

		if (venture_string_is_empty(text))
			return;

		update = g_new0(SessionUpdate, 1);
		update->session_id = turn->session_id;
		update->text = g_strdup_printf("\n[%s]\n", text);
		venture_work_session_post(turn->self, update);
		return;
	}

	default:
		return;
	}

	if (venture_string_is_empty(text))
		return;

	update = g_new0(SessionUpdate, 1);
	update->session_id = turn->session_id;
	update->text = g_strdup(text);
	venture_work_session_post(turn->self, update);
}

/*
 * One turn, on the agent thread.
 *
 * The provider is built here and thrown away here. A CLI provider is a
 * subprocess either way, and an API provider costs a struct; keeping
 * either between turns would be state on this thread that a restart
 * silently loses, and the stored turns are the truth about a session.
 */
static gboolean
venture_work_session_run(gpointer user_data)
{
	SessionTurn *turn = user_data;
	g_autoptr(AiProvider) provider = NULL;
	g_autoptr(AiToolExecutor) executor = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reply = NULL;
	SessionUpdate *update;
	GList *messages = NULL;
	GObject *object;
	gulong handler = 0;
	guint i;

	object = ai_provider_factory_new_from_string(turn->provider, NULL, &error);

	if (NULL == object)
	{
		update = g_new0(SessionUpdate, 1);
		update->session_id = turn->session_id;
		update->finished = TRUE;
		update->failure = g_strdup((NULL != error) ? error->message
		                                           : "no such provider");
		venture_work_session_post(turn->self, update);
		return G_SOURCE_REMOVE;
	}

	provider = AI_PROVIDER(object);

	if (!venture_string_is_empty(turn->model) && AI_IS_CLIENT(object))
		ai_client_set_model(AI_CLIENT(object), turn->model);

	/*
	 * A CLI provider is started in the workspace and brings its own
	 * tools; an API provider gets VENTURE's confined ones, rooted there.
	 * Neither gets ai-glib's built-ins, whose file tools take absolute
	 * paths and whose bash tool is a shell in this process.
	 */
	executor = ai_tool_executor_new_empty();

	if (AI_IS_CLI_CLIENT(object))
	{
		if (!venture_string_is_empty(turn->workspace))
			ai_cli_client_set_working_directory(AI_CLI_CLIENT(object),
			                                    turn->workspace);
	}
	else if (!venture_string_is_empty(turn->workspace))
	{
		venture_work_tools_register(executor, turn->workspace);
	}

	/* Streamed, so the browser sees the agent working rather than a
	 * blank panel for the length of a turn. */
	ai_tool_executor_set_stream(executor, TRUE);
	handler = g_signal_connect(executor, "event",
	                           G_CALLBACK(venture_work_session_on_event),
	                           turn);

	for (i = 0; i < turn->history->len; i++)
	{
		SessionMessage *message;

		message = g_ptr_array_index(turn->history, i);

		if (venture_string_is_empty(message->body))
			continue;

		messages = g_list_append(messages, message->from_user
			? ai_message_new_user(message->body)
			: ai_message_new_assistant(message->body));
	}

	reply = ai_tool_executor_run(executor, provider, messages,
	                             turn->system_prompt, 8192,
	                             turn->cancellable, &error);

	g_list_free_full(messages, g_object_unref);
	g_signal_handler_disconnect(executor, handler);

	update = g_new0(SessionUpdate, 1);
	update->session_id = turn->session_id;
	update->finished = TRUE;

	if (g_cancellable_is_cancelled(turn->cancellable))
		update->failure = g_strdup("Stopped");
	else if (NULL == reply)
		update->failure = g_strdup((NULL != error) ? error->message
		                                           : "the provider failed");
	else
		update->text = g_steal_pointer(&reply);

	venture_work_session_post(turn->self, update);

	return G_SOURCE_REMOVE;
}

/*
 * Whether @path is somewhere a session may work.
 *
 * Under one of the configured roots, compared as resolved paths so that
 * a symlink or a `..` cannot walk out of one. An empty list means no
 * host path is allowed at all, which is the default: running a coding
 * agent with write access to a tree on this machine is a grant an
 * operator makes deliberately.
 */
static gboolean
venture_work_session_path_allowed(
	VentureWorkService	 *self,
	const gchar		 *path,
	gchar			**out_resolved,
	GError			**error
){
	g_auto(GStrv) roots = NULL;
	g_autofree gchar *resolved = NULL;
	gsize i;

	g_object_get(venture_context_get_config(self->context),
	             "forge-workspace-roots", &roots, NULL);

	if ((NULL == roots) || (NULL == roots[0]))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_PERMISSION_DENIED,
		                    "No workspace roots are configured, so a session "
		                    "may only work in a checkout it clones. Set "
		                    "forge.workspace_roots to allow a directory on "
		                    "this machine.");
		return FALSE;
	}

	if (!g_path_is_absolute(path))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A workspace must be an absolute path");
		return FALSE;
	}

	resolved = realpath(path, NULL);

	if (NULL == resolved)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no directory at \"%s\"", path);
		return FALSE;
	}

	for (i = 0; NULL != roots[i]; i++)
	{
		g_autofree gchar *root = NULL;

		root = realpath(roots[i], NULL);

		if (NULL == root)
			continue;

		/*
		 * The separator matters: without it "/srv/venture-secrets"
		 * counts as being under "/srv/venture".
		 */
		if ((0 == g_strcmp0(resolved, root)) ||
		    (g_str_has_prefix(resolved, root) &&
		     (G_DIR_SEPARATOR == resolved[strlen(root)])))
		{
			if (NULL != out_resolved)
				*out_resolved = g_steal_pointer(&resolved);

			return TRUE;
		}
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
	            "\"%s\" is not under any configured workspace root", path);

	return FALSE;
}

gint64
venture_work_service_session_open(
	VentureWorkService		 *self,
	const VentureAgentSessionSpec	 *spec,
	GError				**error
){
	g_autoptr(VentureAgentSession) session = NULL;
	g_autofree gchar *workspace = NULL;
	g_autoptr(GDateTime) now = NULL;
	VentureActor actor;
	gboolean cloned;

	g_return_val_if_fail(VENTURE_IS_WORK_SERVICE(self), 0);
	g_return_val_if_fail(NULL != spec, 0);

	if (venture_string_is_empty(spec->provider))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A session needs a provider");
		return 0;
	}

	cloned = FALSE;

	/*
	 * Where it works, settled here rather than on the agent thread:
	 * deciding it reads configuration and a repository record, and
	 * neither is that thread's to touch.
	 */
	if (!venture_string_is_empty(spec->workspace))
	{
		if (!venture_work_session_path_allowed(self, spec->workspace,
		                                       &workspace, error))
			return 0;
	}
	else if (0 != spec->repo_id)
	{
		g_autoptr(VentureEntity) repo = NULL;
		g_autofree gchar *repo_name = NULL;
		g_autofree gchar *slug = NULL;

		repo = venture_database_get(venture_context_get_database(self->context),
		                            VENTURE_TYPE_FORGE_REPO, spec->repo_id,
		                            error);

		if (NULL == repo)
			return 0;

		g_object_get(repo, "name", &repo_name, NULL);
		slug = venture_slugify(!venture_string_is_empty(repo_name)
			? repo_name : "repo");
		workspace = g_build_filename(
			venture_config_get_state_dir(
				venture_context_get_config(self->context)),
			"forge", "sessions", slug, NULL);
		cloned = TRUE;
	}

	now = venture_time_now();
	session = venture_agent_session_new();
	g_object_set(session,
	             "name", !venture_string_is_empty(spec->name)
	                     ? spec->name : "Session",
	             "state", VENTURE_AGENT_SESSION_STATE_IDLE,
	             "provider", spec->provider,
	             "model", spec->model,
	             "workspace", workspace,
	             "cloned", cloned,
	             "repo-id", spec->repo_id,
	             "ticket-id", spec->ticket_id,
	             "user-id", spec->user_id,
	             "started-at", now,
	             "last-activity-at", now,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(session),
		venture_context_get_default_organization_id(self->context));

	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = "harness";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;

	if (!venture_database_save(venture_context_get_database(self->context),
	                           VENTURE_ENTITY(session), &actor, error))
		return 0;

	return venture_entity_get_id(VENTURE_ENTITY(session));
}

gboolean
venture_work_service_session_send(
	VentureWorkService	 *self,
	gint64			  session_id,
	const gchar		 *prompt,
	GError			**error
){
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) stored = NULL;
	g_autoptr(GDateTime) now = NULL;
	VentureAgentSessionState state;
	SessionTurn *turn;
	VentureActor actor;
	guint i;

	g_return_val_if_fail(VENTURE_IS_WORK_SERVICE(self), FALSE);

	if (venture_string_is_empty(prompt))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Say what the agent should do");
		return FALSE;
	}

	session = venture_database_get(venture_context_get_database(self->context),
	                               VENTURE_TYPE_AGENT_SESSION, session_id,
	                               error);

	if (NULL == session)
		return FALSE;

	g_object_get(session, "state", &state, NULL);

	if (VENTURE_AGENT_SESSION_STATE_CLOSED == state)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "That session is closed");
		return FALSE;
	}

	g_mutex_lock(&self->lock);

	if (g_hash_table_contains(self->session_live, GINT_TO_POINTER(session_id)))
	{
		g_mutex_unlock(&self->lock);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "That session is still working on the last one");
		return FALSE;
	}

	g_mutex_unlock(&self->lock);

	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = "harness";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;

	/* Stored before the agent sees it, so a turn that never comes back
	 * still says what was asked. */
	{
		g_autoptr(VentureAgentTurn) asked = NULL;

		asked = venture_agent_turn_new();
		g_object_set(asked, "session-id", session_id,
		             "role", VENTURE_CHAT_ROLE_USER,
		             "body", prompt, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(asked),
			venture_entity_get_organization_id(session));

		if (!venture_database_save(venture_context_get_database(self->context),
		                           VENTURE_ENTITY(asked), &actor, error))
			return FALSE;
	}

	turn = g_new0(SessionTurn, 1);
	turn->self = g_object_ref(self);
	turn->session_id = session_id;
	turn->cancellable = g_cancellable_new();
	turn->history = g_ptr_array_new_with_free_func(session_message_free);
	turn->system_prompt = g_strdup(
		"You are an agent working for an operator who is reading your "
		"output as it arrives. Say what you are about to do in one line, "
		"do it, then say what changed. Make the smallest change that "
		"achieves what was asked.");

	g_object_get(session, "provider", &turn->provider, "model", &turn->model,
	             "workspace", &turn->workspace, NULL);

	/* The whole conversation, oldest first, as plain data. */
	query = venture_query_new(VENTURE_TYPE_AGENT_TURN);
	venture_query_add_filter_int(query, "session-id", VENTURE_FILTER_OP_EQ,
	                             session_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	stored = venture_database_find(venture_context_get_database(self->context),
	                               query, NULL);

	for (i = 0; (NULL != stored) && (i < stored->len); i++)
	{
		SessionMessage *message;
		VentureChatRole role;

		message = g_new0(SessionMessage, 1);
		g_object_get(g_ptr_array_index(stored, i), "role", &role,
		             "body", &message->body, NULL);
		message->from_user = (VENTURE_CHAT_ROLE_USER == role);
		g_ptr_array_add(turn->history, message);
	}

	now = venture_time_now();
	g_object_set(session, "state", VENTURE_AGENT_SESSION_STATE_WORKING,
	             "last-activity-at", now, NULL);
	venture_database_save(venture_context_get_database(self->context),
	                      VENTURE_ENTITY(session), &actor, NULL);

	g_mutex_lock(&self->lock);
	g_hash_table_insert(self->session_live, GINT_TO_POINTER(session_id),
	                    g_object_ref(turn->cancellable));
	g_mutex_unlock(&self->lock);

	/* Onto the agent thread. Everything after this happens there. */
	g_main_context_invoke_full(self->agent_context, G_PRIORITY_DEFAULT,
	                           venture_work_session_run, turn,
	                           session_turn_free);

	return TRUE;
}

void
venture_work_service_session_close(
	VentureWorkService	*self,
	gint64			 session_id
){
	g_autoptr(VentureEntity) session = NULL;
	GCancellable *cancellable;
	VentureActor actor;

	g_return_if_fail(VENTURE_IS_WORK_SERVICE(self));

	g_mutex_lock(&self->lock);
	cancellable = g_hash_table_lookup(self->session_live,
	                                  GINT_TO_POINTER(session_id));

	if (NULL != cancellable)
		g_cancellable_cancel(cancellable);

	g_mutex_unlock(&self->lock);

	session = venture_database_get(venture_context_get_database(self->context),
	                               VENTURE_TYPE_AGENT_SESSION, session_id,
	                               NULL);

	if (NULL == session)
		return;

	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = "harness";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;

	g_object_set(session, "state", VENTURE_AGENT_SESSION_STATE_CLOSED, NULL);
	venture_database_save(venture_context_get_database(self->context),
	                      VENTURE_ENTITY(session), &actor, NULL);
}
