#include "builtin.h"
#include "exec_cmd.h"
#include "cache.h"
#include "quote.h"

const char git_usage_string[] =
	"git [--version] [--exec-path[=GIT_EXEC_PATH]] [-p|--paginate|--no-pager] [--bare] [--git-dir=GIT_DIR] [--work-tree=GIT_WORK_TREE] [--help] COMMAND [ARGS]";

static int handle_options(const char*** argv, int* argc, int* envchanged)
{
	int handled = 0;

	while (*argc > 0) {
		const char *cmd = (*argv)[0];
		if (cmd[0] != '-')
			break;

		/*
		 * For legacy reasons, the "version" and "help"
		 * commands can be written with "--" prepended
		 * to make them look like flags.
		 */
		if (!strcmp(cmd, "--help") || !strcmp(cmd, "--version"))
			break;

		/*
		 * Check remaining flags.
		 */
		if (!prefixcmp(cmd, "--exec-path")) {
			cmd += 11;
			if (*cmd == '=')
				git_set_argv_exec_path(cmd + 1);
			else {
				puts(git_exec_path());
				exit(0);
			}
		} else if (!strcmp(cmd, "-p") || !strcmp(cmd, "--paginate")) {
			setup_pager();
		} else if (!strcmp(cmd, "--no-pager")) {
			setenv("GIT_PAGER", "cat", 1);
			if (envchanged)
				*envchanged = 1;
		} else if (!strcmp(cmd, "--git-dir")) {
			if (*argc < 2) {
				fprintf(stderr, "No directory given for --git-dir.\n" );
				usage(git_usage_string);
			}
			setenv(GIT_DIR_ENVIRONMENT, (*argv)[1], 1);
			if (envchanged)
				*envchanged = 1;
			(*argv)++;
			(*argc)--;
			handled++;
		} else if (!prefixcmp(cmd, "--git-dir=")) {
			setenv(GIT_DIR_ENVIRONMENT, cmd + 10, 1);
			if (envchanged)
				*envchanged = 1;
		} else if (!strcmp(cmd, "--work-tree")) {
			if (*argc < 2) {
				fprintf(stderr, "No directory given for --work-tree.\n" );
				usage(git_usage_string);
			}
			setenv(GIT_WORK_TREE_ENVIRONMENT, (*argv)[1], 1);
			if (envchanged)
				*envchanged = 1;
			(*argv)++;
			(*argc)--;
		} else if (!prefixcmp(cmd, "--work-tree=")) {
			setenv(GIT_WORK_TREE_ENVIRONMENT, cmd + 12, 1);
			if (envchanged)
				*envchanged = 1;
		} else if (!strcmp(cmd, "--bare")) {
			static char git_dir[PATH_MAX+1];
			is_bare_repository_cfg = 1;
			setenv(GIT_DIR_ENVIRONMENT, getcwd(git_dir, sizeof(git_dir)), 0);
			if (envchanged)
				*envchanged = 1;
		} else {
			fprintf(stderr, "Unknown option: %s\n", cmd);
			usage(git_usage_string);
		}

		(*argv)++;
		(*argc)--;
		handled++;
	}
	return handled;
}

static const char *alias_command;
static char *alias_string;

static int git_alias_config(const char *var, const char *value)
{
	if (!prefixcmp(var, "alias.") && !strcmp(var + 6, alias_command)) {
		alias_string = xstrdup(value);
	}
	return 0;
}

static int split_cmdline(char *cmdline, const char ***argv)
{
	int src, dst, count = 0, size = 16;
	char quoted = 0;

	*argv = xmalloc(sizeof(char*) * size);

	/* split alias_string */
	(*argv)[count++] = cmdline;
	for (src = dst = 0; cmdline[src];) {
		char c = cmdline[src];
		if (!quoted && isspace(c)) {
			cmdline[dst++] = 0;
			while (cmdline[++src]
					&& isspace(cmdline[src]))
				; /* skip */
			if (count >= size) {
				size += 16;
				*argv = xrealloc(*argv, sizeof(char*) * size);
			}
			(*argv)[count++] = cmdline + dst;
		} else if(!quoted && (c == '\'' || c == '"')) {
			quoted = c;
			src++;
		} else if (c == quoted) {
			quoted = 0;
			src++;
		} else {
			if (c == '\\' && quoted != '\'') {
				src++;
				c = cmdline[src];
				if (!c) {
					free(*argv);
					*argv = NULL;
					return error("cmdline ends with \\");
				}
			}
			cmdline[dst++] = c;
			src++;
		}
	}

	cmdline[dst] = 0;

	if (quoted) {
		free(*argv);
		*argv = NULL;
		return error("unclosed quote");
	}

	return count;
}

static int handle_alias(int *argcp, const char ***argv)
{
	int nongit = 0, envchanged = 0, ret = 0, saved_errno = errno;
	const char *subdir;
	int count, option_count;
	const char** new_argv;

	subdir = setup_git_directory_gently(&nongit);

	alias_command = (*argv)[0];
	git_config(git_alias_config);
	if (alias_string) {
		if (alias_string[0] == '!') {
			if (*argcp > 1) {
				struct strbuf buf;

				strbuf_init(&buf, PATH_MAX);
				strbuf_addstr(&buf, alias_string);
				sq_quote_argv(&buf, (*argv) + 1, *argcp - 1, PATH_MAX);
				free(alias_string);
				alias_string = buf.buf;
			}
			trace_printf("trace: alias to shell cmd: %s => %s\n",
				     alias_command, alias_string + 1);
			ret = system(alias_string + 1);
			if (ret >= 0 && WIFEXITED(ret) &&
			    WEXITSTATUS(ret) != 127)
				exit(WEXITSTATUS(ret));
			die("Failed to run '%s' when expanding alias '%s'\n",
			    alias_string + 1, alias_command);
		}
		count = split_cmdline(alias_string, &new_argv);
		option_count = handle_options(&new_argv, &count, &envchanged);
		if (envchanged)
			die("alias '%s' changes environment variables\n"
				 "You can use '!git' in the alias to do this.",
				 alias_command);
		memmove(new_argv - option_count, new_argv,
				count * sizeof(char *));
		new_argv -= option_count;

		if (count < 1)
			die("empty alias for %s", alias_command);

		if (!strcmp(alias_command, new_argv[0]))
			die("recursive alias: %s", alias_command);

		trace_argv_printf(new_argv, count,
				  "trace: alias expansion: %s =>",
				  alias_command);

		new_argv = xrealloc(new_argv, sizeof(char*) *
				    (count + *argcp + 1));
		/* insert after command name */
		memcpy(new_argv + count, *argv + 1, sizeof(char*) * *argcp);
		new_argv[count+*argcp] = NULL;

		*argv = new_argv;
		*argcp += count - 1;

		ret = 1;
	}

	if (subdir)
		chdir(subdir);

	errno = saved_errno;

	return ret;
}

const char git_version_string[] = GIT_VERSION;

#define RUN_SETUP	(1<<0)
#define USE_PAGER	(1<<1)
/*
 * require working tree to be present -- anything uses this needs
 * RUN_SETUP for reading from the configuration file.
 */
#define NEED_WORK_TREE	(1<<2)

struct cmd_struct {
	const char *cmd;
	int (*fn)(int, const char **, const char *);
	int option;
};

static int run_command(struct cmd_struct *p, int argc, const char **argv)
{
	int status;
	struct stat st;
	const char *prefix;

	prefix = NULL;
	if (p->option & RUN_SETUP)
		prefix = setup_git_directory();
	if (p->option & USE_PAGER)
		setup_pager();
	if (p->option & NEED_WORK_TREE) {
		const char *work_tree = get_git_work_tree();
		const char *git_dir = get_git_dir();
		if (!is_absolute_path(git_dir))
			set_git_dir(make_absolute_path(git_dir));
		if (!work_tree || chdir(work_tree))
			die("%s must be run in a work tree", p->cmd);
	}
	trace_argv_printf(argv, argc, "trace: built-in: git");

	status = p->fn(argc, argv, prefix);
	if (status)
		return status & 0xff;

	/* Somebody closed stdout? */
	if (fstat(fileno(stdout), &st))
		return 0;
	/* Ignore write errors for pipes and sockets.. */
	if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
		return 0;

	/* Check for ENOSPC and EIO errors.. */
	if (fflush(stdout))
		die("write failure on standard output: %s", strerror(errno));
	if (ferror(stdout))
		die("unknown write failure on standard output");
	if (fclose(stdout))
		die("close failed on standard output: %s", strerror(errno));
	return 0;
}

static void handle_internal_command(int argc, const char **argv)
{
	const char *cmd = argv[0];
	static struct cmd_struct commands[] = {
		{ "add", cmd_add, RUN_SETUP | NEED_WORK_TREE },
		{ "annotate", cmd_annotate, RUN_SETUP },
		{ "apply", cmd_apply },
		{ "archive", cmd_archive },
		{ "blame", cmd_blame, RUN_SETUP },
		{ "branch", cmd_branch, RUN_SETUP },
		{ "bundle", cmd_bundle },
		{ "cat-file", cmd_cat_file, RUN_SETUP },
		{ "checkout-index", cmd_checkout_index,
			RUN_SETUP | NEED_WORK_TREE},
		{ "check-ref-format", cmd_check_ref_format },
		{ "check-attr", cmd_check_attr, RUN_SETUP | NEED_WORK_TREE },
		{ "cherry", cmd_cherry, RUN_SETUP },
		{ "cherry-pick", cmd_cherry_pick, RUN_SETUP | NEED_WORK_TREE },
		{ "commit-tree", cmd_commit_tree, RUN_SETUP },
		{ "config", cmd_config },
		{ "count-objects", cmd_count_objects, RUN_SETUP },
		{ "describe", cmd_describe, RUN_SETUP },
		{ "diff", cmd_diff },
		{ "diff-files", cmd_diff_files },
		{ "diff-index", cmd_diff_index, RUN_SETUP },
		{ "diff-tree", cmd_diff_tree, RUN_SETUP },
		{ "fetch", cmd_fetch, RUN_SETUP },
		{ "fetch-pack", cmd_fetch_pack, RUN_SETUP },
		{ "fetch--tool", cmd_fetch__tool, RUN_SETUP },
		{ "fmt-merge-msg", cmd_fmt_merge_msg, RUN_SETUP },
		{ "for-each-ref", cmd_for_each_ref, RUN_SETUP },
		{ "format-patch", cmd_format_patch, RUN_SETUP },
		{ "fsck", cmd_fsck, RUN_SETUP },
		{ "fsck-objects", cmd_fsck, RUN_SETUP },
		{ "gc", cmd_gc, RUN_SETUP },
		{ "get-tar-commit-id", cmd_get_tar_commit_id },
		{ "grep", cmd_grep, RUN_SETUP | USE_PAGER },
		{ "help", cmd_help },
#ifndef NO_CURL
		{ "http-fetch", cmd_http_fetch, RUN_SETUP },
#endif
		{ "init", cmd_init_db },
		{ "init-db", cmd_init_db },
		{ "log", cmd_log, RUN_SETUP | USE_PAGER },
		{ "ls-files", cmd_ls_files, RUN_SETUP },
		{ "ls-tree", cmd_ls_tree, RUN_SETUP },
		{ "mailinfo", cmd_mailinfo },
		{ "mailsplit", cmd_mailsplit },
		{ "merge-base", cmd_merge_base, RUN_SETUP },
		{ "merge-file", cmd_merge_file },
		{ "mv", cmd_mv, RUN_SETUP | NEED_WORK_TREE },
		{ "name-rev", cmd_name_rev, RUN_SETUP },
		{ "pack-objects", cmd_pack_objects, RUN_SETUP },
		{ "pickaxe", cmd_blame, RUN_SETUP },
		{ "prune", cmd_prune, RUN_SETUP },
		{ "prune-packed", cmd_prune_packed, RUN_SETUP },
		{ "push", cmd_push, RUN_SETUP },
		{ "read-tree", cmd_read_tree, RUN_SETUP },
		{ "reflog", cmd_reflog, RUN_SETUP },
		{ "repo-config", cmd_config },
		{ "rerere", cmd_rerere, RUN_SETUP },
		{ "reset", cmd_reset, RUN_SETUP },
		{ "rev-list", cmd_rev_list, RUN_SETUP },
		{ "rev-parse", cmd_rev_parse, RUN_SETUP },
		{ "revert", cmd_revert, RUN_SETUP | NEED_WORK_TREE },
		{ "rm", cmd_rm, RUN_SETUP | NEED_WORK_TREE },
		{ "runstatus", cmd_runstatus, RUN_SETUP | NEED_WORK_TREE },
		{ "shortlog", cmd_shortlog, RUN_SETUP | USE_PAGER },
		{ "show-branch", cmd_show_branch, RUN_SETUP },
		{ "show", cmd_show, RUN_SETUP | USE_PAGER },
		{ "stripspace", cmd_stripspace },
		{ "symbolic-ref", cmd_symbolic_ref, RUN_SETUP },
		{ "tag", cmd_tag, RUN_SETUP },
		{ "tar-tree", cmd_tar_tree },
		{ "unpack-objects", cmd_unpack_objects, RUN_SETUP },
		{ "update-index", cmd_update_index, RUN_SETUP },
		{ "update-ref", cmd_update_ref, RUN_SETUP },
		{ "upload-archive", cmd_upload_archive },
		{ "verify-tag", cmd_verify_tag, RUN_SETUP },
		{ "version", cmd_version },
		{ "whatchanged", cmd_whatchanged, RUN_SETUP | USE_PAGER },
		{ "write-tree", cmd_write_tree, RUN_SETUP },
		{ "verify-pack", cmd_verify_pack },
		{ "show-ref", cmd_show_ref, RUN_SETUP },
		{ "pack-refs", cmd_pack_refs, RUN_SETUP },
	};
	int i;

#ifdef STRIP_EXTENSION
	i = strlen(argv[0]) - strlen(STRIP_EXTENSION);
	if (i > 0 && !strcmp(argv[0] + i, STRIP_EXTENSION)) {
		char *argv0 = strdup(argv[0]);
		argv[0] = cmd = argv0;
		argv0[i] = '\0';
	}
#endif

	/* Turn "git cmd --help" into "git help cmd" */
	if (argc > 1 && !strcmp(argv[1], "--help")) {
		argv[1] = argv[0];
		argv[0] = cmd = "help";
	}

	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		struct cmd_struct *p = commands+i;
		if (strcmp(p->cmd, cmd))
			continue;
		exit(run_command(p, argc, argv));
	}
}

int main(int argc, const char **argv)
{
	const char *cmd = argv[0] ? argv[0] : "git-help";
	char *slash = strrchr(cmd, '/');
	const char *cmd_path = NULL;
	int done_alias = 0;

	/*
	 * Take the basename of argv[0] as the command
	 * name, and the dirname as the default exec_path
	 * if we don't have anything better.
	 */
#ifdef __MINGW32__
	if (!slash)
		slash = strrchr(cmd, '\\');
#endif
	if (slash) {
		*slash++ = 0;
		cmd_path = cmd;
		cmd = slash;
	}

	/*
	 * "git-xxxx" is the same as "git xxxx", but we obviously:
	 *
	 *  - cannot take flags in between the "git" and the "xxxx".
	 *  - cannot execute it externally (since it would just do
	 *    the same thing over again)
	 *
	 * So we just directly call the internal command handler, and
	 * die if that one cannot handle it.
	 */
	if (!prefixcmp(cmd, "git-")) {
		cmd += 4;
		argv[0] = cmd;
		handle_internal_command(argc, argv);
		die("cannot handle %s internally", cmd);
	}

	/* Look for flags.. */
	argv++;
	argc--;
	handle_options(&argv, &argc, NULL);
	if (argc > 0) {
		if (!prefixcmp(argv[0], "--"))
			argv[0] += 2;
	} else {
		/* The user didn't specify a command; give them help */
		printf("usage: %s\n\n", git_usage_string);
		list_common_cmds_help();
		exit(1);
	}
	cmd = argv[0];

	/*
	 * We use PATH to find git commands, but we prepend some higher
	 * precidence paths: the "--exec-path" option, the GIT_EXEC_PATH
	 * environment, and the $(gitexecdir) from the Makefile at build
	 * time.
	 */
	setup_path(cmd_path);

	while (1) {
		/* See if it's an internal command */
		handle_internal_command(argc, argv);

		/* .. then try the external ones */
		execv_git_cmd(argv);

		/* It could be an alias -- this works around the insanity
		 * of overriding "git log" with "git show" by having
		 * alias.log = show
		 */
		if (done_alias || !handle_alias(&argc, &argv))
			break;
		done_alias = 1;
	}

	if (errno == ENOENT) {
		if (done_alias) {
			fprintf(stderr, "Expansion of alias '%s' failed; "
				"'%s' is not a git-command\n",
				cmd, argv[0]);
			exit(1);
		}
		help_unknown_cmd(cmd);
	}

	fprintf(stderr, "Failed to run command '%s': %s\n",
		cmd, strerror(errno));

	return 1;
}
