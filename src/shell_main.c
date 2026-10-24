/**
 * AiShell main entry point.
 *
 * Supports three execution modes:
 *   1. BusyBox-style multicall (symlink mode):
 *      If argv[0] basename is not "aishell" and matches a registered
 *      command name, execute that command directly and exit.
 *   2. Subcommand multicall mode (argc > 1):
 *      When invoked as "./aishell <cmd> [args...]", execute a single
 *      command and exit. Also supports "help" and "help <cmd>".
 *   3. Interactive REPL loop (argc == 1):
 *      When invoked with no arguments, enter an interactive loop that
 *      reads, tokenizes, and dispatches commands in-process without
 *      fork/exec. "exit" quits; "help [cmd]" shows usage.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration of module registration helpers. */
extern void register_cat_command(void);
extern void register_ls_command(void);
extern void register_stat_command(void);
extern void register_head_command(void);
extern void register_tail_command(void);
extern void register_cp_command(void);
extern void register_mv_command(void);
extern void register_rm_command(void);
extern void register_mkdir_command(void);
extern void register_rmdir_command(void);
extern void register_touch_command(void);
extern void register_pkg_command(void);

/** Helper callback to list registered commands in the format
 *  "  <name> - <summary>". */
static void list_command_cb(const cmd_spec_t *spec, void *userdata)
{
    FILE *out = userdata;

    if (spec == NULL || out == NULL) {
        return;
    }

    fprintf(out, "  %-10s - %s\n",
            spec->name != NULL ? spec->name : "",
            spec->summary != NULL ? spec->summary : "");
}

/** Print the shell banner: initialization message and command list. */
static void print_banner(void)
{
    fputs("AiShell initialized.\n", stdout);
    fputs("Available commands:\n", stdout);
    for_each_command(list_command_cb, stdout);
}

/** Register all built-in commands with the registry.
 *  Must be called before any command lookup or dispatch. */
static void register_all_commands(void)
{
    register_cat_command();
    register_ls_command();
    register_stat_command();
    register_head_command();
    register_tail_command();
    register_cp_command();
    register_mv_command();
    register_rm_command();
    register_mkdir_command();
    register_rmdir_command();
    register_touch_command();
    register_pkg_command();
}

/** Extract the basename from a path argument.
 *  Returns a pointer to the last path component (no allocation). */
static const char *basename_of(const char *path)
{
    const char *base;

    if (path == NULL) {
        return NULL;
    }

    /* strrchr finds the last '/' in the path; everything after it
     * is the basename. If no '/' is found, the whole string is the basename. */
    base = strrchr(path, '/');
    return base != NULL ? base + 1 : path;
}

/** Handle the "help" subcommand.
 *  argc/argv must be the help arguments: argv[0] is "help",
 *  argv[1] (optional) is the command name to look up.
 *  Returns the process exit status. */
static int handle_help(int argc, char **argv)
{
    const cmd_spec_t *cmd;

    if (argc > 2) {
        fputs("Usage: help [command]\n", stderr);
        return 1;
    }

    if (argc == 2) {
        /* "help <command>" — show detailed help for a specific command. */
        cmd = find_command(argv[1]);
        if (cmd == NULL) {
            fprintf(stderr, "aishell: command not found: %s\n", argv[1]);
            return 1;
        }
        printf("%s - %s\n", cmd->name, cmd->summary);
        if (cmd->long_help != NULL && cmd->long_help[0] != '\0') {
            printf("%s\n", cmd->long_help);
        }
        if (cmd->print_usage != NULL) {
            cmd->print_usage(stdout);
        }
        return 0;
    }

    /* "help" with no argument — list all commands. */
    print_banner();
    return 0;
}

/** Tokenize a line into an argv-style array using strtok_r().
 *  On return, *out_argv and *out_argc are set to the token count and
 *  array. The caller must free *out_argv.
 *  Returns 0 on success, -1 on allocation failure.
 *  The tokens point into the original line buffer (not copied). */
static int split_line(char *line, char ***out_argv, int *out_argc)
{
    char *saveptr = NULL;
    char *token;
    char **tokens = NULL;
    int count = 0;
    int capacity = 0;

    /* strtok_r() splits the string on whitespace; it is reentrant
     * (state is stored in saveptr), making it safe for nested or
     * recursive tokenizing. */
    for (token = strtok_r(line, " \t\r\n", &saveptr);
         token != NULL;
         token = strtok_r(NULL, " \t\r\n", &saveptr)) {

        if (count == capacity) {
            capacity = capacity == 0 ? 8 : capacity * 2;
            char **grown = realloc(tokens, capacity * sizeof(*grown));
            if (grown == NULL) {
                free(tokens);
                return -1;
            }
            tokens = grown;
        }
        tokens[count] = token;
        ++count;
    }

    *out_argv = tokens;
    *out_argc = count;
    return 0;
}

/** Run a single command with the given argv.
 *  In the REPL, this dispatches in-process (no fork/exec).
 *  argv[0] is the command name.
 *  Returns the command's exit status. */
static int run_command(int argc, char **argv)
{
    const cmd_spec_t *cmd;

    if (argc == 0) {
        return 0;
    }

    if (strcmp(argv[0], "help") == 0) {
        return handle_help(argc, argv);
    }

    if (strcmp(argv[0], "exit") == 0) {
        /* "exit" in REPL context — the REPL loop handles this before
         * reaching here, but guard for safety. */
        return 0;
    }

    cmd = find_command(argv[0]);
    if (cmd == NULL) {
        fprintf(stderr, "aishell: command not found: %s\n", argv[0]);
        return 1;
    }
    if (cmd->run == NULL) {
        fprintf(stderr, "aishell: command cannot be run: %s\n", argv[0]);
        return 1;
    }

    return cmd->run(argc, argv);
}

/** Interactive REPL loop.
 *  Prints "aishell> " prompt to stderr, reads a line with getline(),
 *  tokenizes it, dispatches the command in-process, and repeats until
 *  "exit" or EOF. */
static void repl_loop(void)
{
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    char **cmd_argv = NULL;
    int cmd_argc;

    for (;;) {
        /* Print the shell prompt to stderr so it does not interleave
         * with command output on stdout. */
        fputs("aishell> ", stderr);
        fflush(stderr);

        /* getline() reads an entire line from stdin, allocating the buffer
         * as needed. Returns -1 on EOF or error. */
        line_len = getline(&line, &line_cap, stdin);
        if (line_len == -1) {
            /* Distinguish EOF from error. */
            if (feof(stdin)) {
                fputs("\n", stderr);
            } else {
                perror("getline");
            }
            break;
        }

        /* Remove the trailing newline that getline includes. */
        if (line_len > 0 && line[line_len - 1] == '\n') {
            line[line_len - 1] = '\0';
            --line_len;
        }

        /* Skip empty lines. */
        if (line_len == 0) {
            continue;
        }

        /* Tokenize the input line into an argv array. */
        if (split_line(line, &cmd_argv, &cmd_argc) != 0) {
            fputs("aishell: failed to parse input\n", stderr);
            continue;
        }

        if (cmd_argc == 0) {
            free(cmd_argv);
            cmd_argv = NULL;
            continue;
        }

        /* Handle "exit" specially to break out of the loop. */
        if (strcmp(cmd_argv[0], "exit") == 0) {
            free(cmd_argv);
            cmd_argv = NULL;
            break;
        }

        /* Dispatch the command in-process. */
        (void)run_command(cmd_argc, cmd_argv);

        free(cmd_argv);
        cmd_argv = NULL;
    }

    free(line);
    free(cmd_argv);
}

int main(int argc, char **argv)
{
    const cmd_spec_t *cmd;
    const char *prog_name;

    register_all_commands();

    /* Determine the invocation name from argv[0].
     * basename_of() extracts the last path component so that
     * symlinks like "./ls" or "aishell" are correctly identified. */
    prog_name = basename_of(argv[0]);

    /* Mode 1: BusyBox-style multicall (symlink mode).
     * If argv[0] is not "aishell" and matches a registered command,
     * execute that command directly and exit. */
    if (prog_name != NULL && strcmp(prog_name, "aishell") != 0) {
        cmd = find_command(prog_name);
        if (cmd != NULL && cmd->run != NULL) {
            return cmd->run(argc, argv);
        }
    }

    /* Mode 2: Subcommand multicall (argc > 1).
     * Execute a single command and exit. */
    if (argc > 1) {
        if (strcmp(argv[1], "help") == 0) {
            /* "help" or "help <cmd>" — pass argv[1..] as the help args. */
            return handle_help(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "exit") == 0) {
            return 0;
        }

        cmd = find_command(argv[1]);
        if (cmd == NULL) {
            fprintf(stderr, "aishell: command not found: %s\n", argv[1]);
            return 1;
        }
        if (cmd->run == NULL) {
            fprintf(stderr, "aishell: command cannot be run: %s\n", argv[1]);
            return 1;
        }

        return cmd->run(argc - 1, argv + 1);
    }

    /* Mode 3: Interactive REPL loop (argc == 1).
     * Print the banner, then enter the read-eval-print loop.
     * When stdin is not a TTY (e.g., piped input or /dev/null), the
     * loop reads until EOF and exits naturally — this allows both
     * interactive use and scripted input (e.g., "cmds | ./aishell"). */
    print_banner();
    repl_loop();

    return 0;
}
