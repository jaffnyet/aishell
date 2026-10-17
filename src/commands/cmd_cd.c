/**
 * cd command for AiShell.
 * Changes the current working directory using POSIX chdir().
 *
 * Design notes:
 * - argtable3 drives parsing and usage; no manual argument handling.
 * - chdir() changes the process working directory, affecting all subsequent
 *   relative path resolution in the shell.
 * - The directory operand is optional; with no argument, cd changes to
 *   the user's home directory (from $HOME), matching POSIX cd behavior.
 * - On error, chdir() returns -1 with errno set; perror() reports it.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** argtable3 argument table for cd.
 *  Layout: [help, dir, end, NULL] — the end marker must be last. */
typedef struct cd_argtable {
    arg_lit_t *help;  /* -h/--help flag: prints usage and exits 0 */
    arg_file_t *dir;  /* Optional directory operand (defaults to $HOME) */
    arg_end_t *end;   /* argtable3 terminator; collects parse errors */
    void *table[4];   /* Array of argtable3 pointers terminated by NULL */
} cd_argtable_t;

/** Build the argtable3 table for cd.
 *  Allocates the help literal, directory operand, and end marker.
 *  Returns 0 on success, -1 on allocation failure.
 *  The caller must free the table with arg_freetable(). */
static int build_cd_argtable(cd_argtable_t *args)
{
    /* Zero-initialize so partial failure does not leave dangling pointers. */
    *args = (cd_argtable_t){0};

    args->help = arg_lit0("h", "help", "show usage and description");
    args->table[0] = args->help;

    /* arg_filen with mincount=0 allows zero or one directory operand.
     * datatype "<dir>" describes the expected token in usage output. */
    args->dir = arg_file0(NULL, NULL, "<dir>", "directory to change to");
    args->table[1] = args->dir;

    args->end = arg_end(20);
    args->table[2] = args->end;

    /* NULL sentinel — arg_parse() traverses table until this terminator. */
    args->table[3] = NULL;

    if (args->help == NULL || args->dir == NULL || args->end == NULL) {
        arg_freetable(args->table, sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }

    return 0;
}

/** Print cd usage derived from argtable3 definitions.
 *  Drives arg_print_syntax and arg_print_glossary so usage never
 *  diverges from the actual argument definitions. */
static void print_usage_with_argtable(FILE *out, void **table)
{
    fputs("Usage: cd ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

/** Public usage printer — called by AiShell's help system and standalone binaries. */
void cd_print_usage(FILE *out)
{
    cd_argtable_t args;

    if (out == NULL) {
        return;
    }

    if (build_cd_argtable(&args) != 0) {
        fputs("cd: failed to allocate argument table\n", stderr);
        return;
    }

    print_usage_with_argtable(out, args.table);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/** Main execution entry point for the cd command.
 *  Returns 0 on success, 1 on any error. */
int cd_run(int argc, char **argv)
{
    cd_argtable_t args;
    int nerrors;
    const char *target;

    if (build_cd_argtable(&args) != 0) {
        fputs("cd: failed to allocate argument table\n", stderr);
        return 1;
    }

    /* arg_parse() processes argv against the argtable3 definitions;
     * returns the count of parse errors encountered. */
    nerrors = arg_parse(argc, argv, args.table);

    /* Help flag takes priority over syntax errors. */
    if (args.help->count > 0) {
        print_usage_with_argtable(stdout, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 0;
    }

    /* On syntax errors, report to stderr and exit non-zero. */
    if (nerrors > 0) {
        arg_print_errors(stderr, args.end, argv[0]);
        print_usage_with_argtable(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Determine the target directory.
     * If no operand was given, fall back to $HOME for POSIX-like behavior.
     * getenv() searches the environment; returns NULL if HOME is unset. */
    if (args.dir->count > 0) {
        target = args.dir->filename[0];
    } else {
        target = getenv("HOME");
        if (target == NULL || target[0] == '\0') {
            fputs("cd: HOME not set\n", stderr);
            arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
            return 1;
        }
    }

    /* chdir() changes the process's current working directory.
     * This affects all subprocesses and subsequent relative path resolution.
     * Returns 0 on success, -1 on failure (errno is set). */
    if (chdir(target) != 0) {
        perror(target);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Flush stdout to ensure any buffered output is written before we
     * return; a failed flush could mask downstream errors. */
    if (fflush(stdout) == EOF) {
        perror("cd: stdout");
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return 0;
}

cmd_spec_t cmd_cd_spec = {
    "cd",
    "change the current directory",
    "Change the current working directory to DIR (default: $HOME). Use chdir(2) to change the process working directory.",
    cd_run,
    cd_print_usage
};

void register_cd_command(void)
{
    register_command(&cmd_cd_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_cd_spec.run(argc, argv);
}
#endif