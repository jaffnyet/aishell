/**
 * pkg command for AiShell.
 * A minimal package manager for installing, listing, inspecting, and
 * removing shell packages into the ~/.mysh/ directory tree.
 *
 * Storage layout:
 *   ~/.mysh/pkgs/<name>-<version>/   - extracted package contents
 *   ~/.mysh/bin/                     - executables exposed on PATH
 *   ~/.mysh/pkgdb.txt                - flat record of installed packages
 *
 * Design notes:
 * - Argument parsing is handled entirely by argtable3, which builds an
 *   argument table that drives both the `run()` parser and the
 *   `print_usage` output generator. This ensures usage text never drifts
 *   from actual parsing.
 * - The first positional operand is the subcommand (install, list, info,
 *   remove). A second arg_filen captures the remaining operands (e.g., the
 *   tarball for install, or the package name for info/remove). This mirrors
 *   the cat module's pattern but adds a leading subcommand token.
 * - pkg.json is parsed with a minimal line-oriented scanner that extracts
 *   only the fields this implementation needs (name, version, description,
 *   files). This avoids a heavy JSON dependency while remaining robust for
 *   the documented schema.
 * - Archive extraction is performed via fork()/execvp() of the system
 *   `tar` utility, consistent with how a shell delegates to external
 *   programs. Every fork/exec failure is reported with perror().
 * - The package database is a plain-text file with tab-separated fields:
 *     <name>\t<version>\t<comma-list-of-owned-binaries>\t<pkgdir>
 *   Lines beginning with '#' are treated as comments.
 * - All system calls that can fail are checked; errors are reported to
 *   stderr and cause pkg_run() to return status 1.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmd_spec.h"
#include "argtable3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* mkdir(), chmod() */
#include <sys/types.h>
#include <sys/wait.h> /* waitpid() */
#include <dirent.h>   /* opendir(), readdir(), closedir() */
#include <unistd.h>   /* fork(), execvp(), getenv(), access() */

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

/* Default base directory under the user's home. */
#define MISH_HOME_ENV  "HOME"
#define MISH_DIR_NAME  ".mysh"
#define MISH_PKGS_DIR  ".mysh/pkgs"
#define MISH_BIN_DIR   ".mysh/bin"
#define MISH_PKGDB     ".mysh/pkgdb.txt"

/* Buffer sizes. */
#define PATH_MAX_PKT 4096
#define LINE_MAX_PKT 1024
#define DB_MAX_FIELDS 4

/* ------------------------------------------------------------------ */
/* argtable3 argument table for pkg                                   */
/* Layout: [help, subcmd, operands, end, NULL]                        */
/* ------------------------------------------------------------------ */

typedef struct pkg_argtable {
    arg_lit_t *help;     /* -h/--help flag: prints usage and exits 0 */
    arg_file_t *subcmd;  /* First positional: the subcommand name */
    arg_file_t *operands;/* Remaining positionals (tarball/name/etc.) */
    arg_end_t *end;      /* argtable3 terminator; collects parse errors */
    void *table[5];      /* Array of argtable3 pointers terminated by NULL */
} pkg_argtable_t;

/**
 * Build the argtable3 table for pkg.
 * Allocates the help literal, the subcommand operand, the remaining operands,
 * and the end marker.
 * Returns 0 on success, -1 on allocation failure.
 * The caller must free the table with arg_freetable().
 */
static int build_pkg_argtable(pkg_argtable_t *args)
{
    /* Zero-initialize so partial failure does not leave dangling pointers. */
    *args = (pkg_argtable_t){0};

    /* -h/--help: a literal (on/off) flag, 0..1 occurrences. */
    args->help = arg_lit0("h", "help", "show usage and description");
    args->table[0] = args->help;

    /*
     * The subcommand (install|list|info|remove) is the first positional.
     * mincount=1, maxcount=1 forces exactly one subcommand to be given.
     */
    args->subcmd = arg_filen(NULL, NULL, "<subcommand>", 1, 1,
                             "subcommand to run (install, list, info, remove)");
    args->table[1] = args->subcmd;

    /*
     * Remaining positional operands (e.g. the tarball path for install,
     * or the package name for info/remove). 0..100 allowed.
     */
    args->operands = arg_filen(NULL, NULL, "<arg>", 0, 100,
                               "additional arguments for the subcommand");
    args->table[2] = args->operands;

    /* arg_end must be present to collect parse errors. */
    args->end = arg_end(20);
    args->table[3] = args->end;

    /* NULL sentinel: arg_parse() traverses table until this terminator. */
    args->table[4] = NULL;

    if (args->help == NULL || args->subcmd == NULL ||
        args->operands == NULL || args->end == NULL) {
        arg_freetable(args->table,
                      sizeof(args->table) / sizeof(args->table[0]));
        return -1;
    }

    return 0;
}

/**
 * Generate and print usage text to the given stream.
 * Drives arg_print_syntax and arg_print_glossary so the format never
 * diverges from the parsed argument definitions.
 */
static void print_usage_with_argtable(FILE *out, void **table)
{
    fputs("Usage: pkg ", out);
    arg_print_syntax(out, table, "\n");
    fputs("Options:\n", out);
    arg_print_glossary(out, table, "  %-25s %s\n");
}

/**
 * Public usage printer — called by AiShell's help system and the
 * standalone binary entry point.
 */
void pkg_print_usage(FILE *out)
{
    pkg_argtable_t args;

    if (out == NULL) {
        return;
    }

    if (build_pkg_argtable(&args) != 0) {
        fputs("pkg: failed to allocate argument table\n", stderr);
        return;
    }

    print_usage_with_argtable(out, args.table);
    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                       */
/* ------------------------------------------------------------------ */

/**
 * Write the full path of a file under ~/.mysh into `buf` (size `sz`).
 * Uses getenv("HOME"); returns -1 if HOME is unset or the path is too long.
 * Caller provides buf/sz; no allocation is performed.
 */
static int mish_path(char *buf, size_t sz, const char *suffix)
{
    const char *home;

    if (buf == NULL || suffix == NULL) {
        return -1;
    }

    /* getenv() searches the environment; returns NULL if HOME is unset. */
    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        fputs("pkg: HOME is not set; cannot determine install base directory\n",
              stderr);
        return -1;
    }

    /* snprintf returns the number of chars that would have been written;
     * if >= sz the buffer was too small and the path was truncated. */
    if (snprintf(buf, sz, "%s/%s%s", home, MISH_DIR_NAME, suffix) >= (int)sz) {
        fprintf(stderr, "pkg: path too long for suffix '%s'\n", suffix);
        return -1;
    }

    return 0;
}

/**
 * Ensure the directory at `path` exists, creating parents as needed.
 * Returns 0 on success or if the directory already exists; -1 on failure.
 * Uses mkdir() with mode 0755 and checks errno == EEXIST to tolerate a
 * pre-existing directory.
 */
static int ensure_dir(const char *path)
{
    struct stat st;

    if (path == NULL) {
        return -1;
    }

    /* stat() checks existence and type; 0 return means it exists. */
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        errno = ENOTDIR;
        perror(path);
        return -1;
    }

    /* errno != ENOENT means some other failure; report it. */
    if (errno != ENOENT) {
        perror(path);
        return -1;
    }

    /* ENOENT: recurse to create the parent first (mkdir -p semantics). */
    {
        char parent[PATH_MAX_PKT];
        char *slash;

        /* Copy path into a mutable buffer so we can truncate to the parent. */
        if (snprintf(parent, sizeof(parent), "%s", path) >=
            (int)sizeof(parent)) {
            errno = ENAMETOOLONG;
            perror(path);
            return -1;
        }

        /* Find the last '/' to derive the parent path. */
        slash = strrchr(parent, '/');
        if (slash != NULL) {
            *slash = '\0'; /* truncate at last slash -> parent path */
            if (slash == parent) {
                /* Parent is "/" or root; nothing to create above. */
                *slash = '/';
            }
        }

        if (parent[0] != '\0' && strcmp(parent, path) != 0) {
            if (ensure_dir(parent) != 0) {
                return -1;
            }
        }
    }

    /* mkdir() creates the final component with mode 0755 (rwxr-xr-x). */
    if (mkdir(path, 0755) != 0) {
        if (errno == EEXIST) {
            return 0;
        }
        perror(path);
        return -1;
    }

    return 0;
}

/**
 * Ensure the base ~/.mysh directories (pkgs/ and bin/) exist.
 * Returns 0 on success, -1 on failure.
 */
static int ensure_mish_dirs(void)
{
    char buf[PATH_MAX_PKT];

    if (mish_path(buf, sizeof(buf), "") != 0) {
        return -1;
    }
    if (ensure_dir(buf) != 0) {
        return -1;
    }

    if (mish_path(buf, sizeof(buf), "/" MISH_PKGS_DIR + sizeof(MISH_DIR_NAME)) != 0) {
        /* mish_path already formats ~/.mysh/<suffix>; reuse with the full
         * relative suffix. Simpler: build directly. */
        const char *home = getenv("HOME");
        if (home == NULL) {
            fputs("pkg: HOME is not set\n", stderr);
            return -1;
        }
        if (snprintf(buf, sizeof(buf), "%s/%s", home, MISH_PKGS_DIR) >=
            (int)sizeof(buf)) {
            fputs("pkg: pkgs path too long\n", stderr);
            return -1;
        }
    }
    if (ensure_dir(buf) != 0) {
        return -1;
    }

    if (mish_path(buf, sizeof(buf), "/" MISH_BIN_DIR + sizeof(MISH_DIR_NAME)) != 0) {
        const char *home = getenv("HOME");
        if (home == NULL) {
            fputs("pkg: HOME is not set\n", stderr);
            return -1;
        }
        if (snprintf(buf, sizeof(buf), "%s/%s", home, MISH_BIN_DIR) >=
            (int)sizeof(buf)) {
            fputs("pkg: bin path too long\n", stderr);
            return -1;
        }
    }
    if (ensure_dir(buf) != 0) {
        return -1;
    }

    return 0;
}

/**
 * Return the full pkgdb path in `buf` (size `sz`). Returns 0 on success.
 */
static int pkgdb_path(char *buf, size_t sz)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        fputs("pkg: HOME is not set\n", stderr);
        return -1;
    }
    if (snprintf(buf, sz, "%s/%s", home, MISH_PKGDB) >= (int)sz) {
        fputs("pkg: pkgdb path too long\n", stderr);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal pkg.json parsing                                           */
/* ------------------------------------------------------------------ */

/**
 * A parsed package manifest. Only the fields used by pkg are populated.
 * `files` is a dynamically allocated array of strings (file paths), and
 * `file_count` is the number of entries. Caller frees `files` with
 * free_pkg_manifest().
 */
typedef struct pkg_manifest {
    char name[256];
    char version[128];
    char description[512];
    char **files;
    int file_count;
} pkg_manifest_t;

/** Free a manifest previously populated by parse_pkg_json(). */
static void free_pkg_manifest(pkg_manifest_t *m)
{
    if (m == NULL) {
        return;
    }
    if (m->files != NULL) {
        int i;
        for (i = 0; i < m->file_count; ++i) {
            free(m->files[i]);
        }
        free(m->files);
        m->files = NULL;
        m->file_count = 0;
    }
}

/**
 * Trim leading and trailing whitespace from `s` in place.
 * Returns a pointer to the (possibly advanced) start of the trimmed string.
 */
static char *trim(char *s)
{
    char *end;

    if (s == NULL) {
        return NULL;
    }

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        return s;
    }

    /* strchr is used indirectly via the loop; find the last non-space. */
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return s;
}

/**
 * Extract the string value for a given JSON key from a single line of the
 * form:  "key": "value"
 * or     "key": ["a", "b", "c"]
 * The line must already be trimmed of leading whitespace.
 * Returns a heap-allocated copy of the value (without quotes), or NULL.
 */
static char *extract_string_value(const char *line, const char *key)
{
    size_t klen = strlen(key);
    const char *p;
    char *val;
    char *close_quote;

    /* Look for "key" in the line. */
    p = strstr(line, key);
    while (p != NULL) {
        /* Verify the character before `key` is a quote boundary and that
         * the preceding char (if any) is a quote or colon-space. */
        if (p >= line && (p == line || p[-1] == '"' || p[-1] == ' ' ||
                          p[-1] == '\t')) {
            /* Check that the match is "key" (with surrounding quotes). */
            if (p > line && p[-1] != '"') {
                /* Not a quoted key match; try the next occurrence. */
                p = strstr(p + klen, key);
                continue;
            }
            break;
        }
        p = strstr(p + klen, key);
    }
    if (p == NULL) {
        return NULL;
    }

    /* Advance past the key to the colon. */
    p += klen;
    while (*p != '\0' && *p != ':') {
        p++;
    }
    if (*p != ':') {
        return NULL;
    }
    p++; /* skip colon */

    /* Skip whitespace. */
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '"') {
        return NULL;
    }
    p++; /* skip opening quote */

    /* Find the closing quote. */
    val = strdup(p);
    if (val == NULL) {
        return NULL;
    }
    close_quote = strchr(val, '"');
    if (close_quote == NULL) {
        free(val);
        return NULL;
    }
    *close_quote = '\0';
    return val;
}

/**
 * Parse a pkg.json file into `m`. Returns 0 on success, -1 on failure.
 * The manifest's `files` array is allocated here; caller frees via
 * free_pkg_manifest().
 */
static int parse_pkg_json(const char *jsonpath, pkg_manifest_t *m)
{
    FILE *fp;
    char line[LINE_MAX_PKT];
    char buf[LINE_MAX_PKT];

    if (jsonpath == NULL || m == NULL) {
        return -1;
    }

    memset(m, 0, sizeof(*m));

    /* fopen with "r" opens for reading; NULL return means failure. */
    fp = fopen(jsonpath, "r");
    if (fp == NULL) {
        perror(jsonpath);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *trimmed = trim(line);
        char *val;

        /* Look for the "files" array line(s). We handle the simple case
         * where files appears on one line: "files": ["a", "b", "c"]      */
        if (strncmp(trimmed, "\"files\"", 6) == 0) {
            char *start;
            /* Re-find the colon and everything after it. */
            char *colon = strchr(trimmed, ':');
            if (colon == NULL) {
                continue;
            }
            start = colon + 1;
            /* Skip whitespace. */
            while (*start == ' ' || *start == '\t') {
                start++;
            }

            /* If the array is on one line, parse it directly. */
            if (*start == '[') {
                char *pend = strchr(start, ']');
                if (pend != NULL) {
                    /* Null-terminate the array contents. */
                    char *arr = strndup(start + 1, (size_t)(pend - start - 1));
                    if (arr != NULL) {
                        /* Walk the comma-separated quoted strings. */
                        char *tok = arr;
                        while (tok != NULL && *tok != '\0') {
                            char *q1 = strchr(tok, '"');
                            if (q1 == NULL) {
                                break;
                            }
                            char *q2 = strchr(q1 + 1, '"');
                            if (q2 == NULL) {
                                break;
                            }
                            /* Extract the string between the quotes. */
                            char *entry = strndup(q1 + 1,
                                                  (size_t)(q2 - q1 - 1));
                            if (entry != NULL) {
                                /* Grow the files array. */
                                char **newfiles = realloc(m->files,
                                    (size_t)(m->file_count + 1) * sizeof(char *));
                                if (newfiles == NULL) {
                                    free(entry);
                    if (arr) free(arr);
                                    break;
                                }
                                m->files = newfiles;
                                m->files[m->file_count++] = entry;
                                tok = q2 + 1;
                            } else {
                                tok = NULL;
                            }
                        }
                        free(arr);
                    }
                }
            }
            continue;
        }

        /* name, version, description are simple string fields. */
        val = extract_string_value(trimmed, "name");
        if (val != NULL) {
            if (m->name[0] == '\0') {
                strncpy(m->name, val, sizeof(m->name) - 1);
                m->name[sizeof(m->name) - 1] = '\0';
            }
            free(val);
            continue;
        }

        val = extract_string_value(trimmed, "version");
        if (val != NULL) {
            if (m->version[0] == '\0') {
                strncpy(m->version, val, sizeof(m->version) - 1);
                m->version[sizeof(m->version) - 1] = '\0';
            }
            free(val);
            continue;
        }

        val = extract_string_value(trimmed, "description");
        if (val != NULL) {
            if (m->description[0] == '\0') {
                strncpy(m->description, val, sizeof(m->description) - 1);
                m->description[sizeof(m->description) - 1] = '\0';
            }
            free(val);
            continue;
        }
    }

    if (ferror(fp)) {
        perror("pkg: reading pkg.json");
        fclose(fp);
        free_pkg_manifest(m);
        return -1;
    }
    fclose(fp);

    if (m->name[0] == '\0') {
        fputs("pkg: pkg.json is missing the 'name' field\n", stderr);
        free_pkg_manifest(m);
        return -1;
    }
    if (m->version[0] == '\0') {
        fputs("pkg: pkg.json is missing the 'version' field\n", stderr);
        free_pkg_manifest(m);
        return -1;
    }

    /* Suppress unused-variable warning for buf (kept for future use). */
    (void)buf;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Package database operations                                        */
/* ------------------------------------------------------------------ */

/**
 * Append a single installed-file path to the comma-separated list in `dst`,
 * allocating or growing `*list` as needed. `path` must not contain a comma.
 */
static int db_append_file(char **list, const char *path)
{
    size_t len;
    char *newlist;
    char *comma_pos;

    if (list == NULL || path == NULL) {
        return -1;
    }

    len = *list != NULL ? strlen(*list) : 0;

    /* Disallow commas in paths to keep the format parseable. */
    if (strchr(path, ',') != NULL) {
        fprintf(stderr, "pkg: path contains a comma: %s\n", path);
        return -1;
    }

    if (len == 0) {
        newlist = strdup(path);
    } else {
        /* Allocate space for "existing,path". */
        newlist = malloc(len + 1 + strlen(path) + 1);
        if (newlist != NULL) {
            strcpy(newlist, *list);
            strcat(newlist, ",");
            strcat(newlist, path);
        }
    }

    if (newlist == NULL) {
        return -1;
    }

    free(*list);
    *list = newlist;
    (void)comma_pos;
    return 0;
}

/**
 * Append a record to pkgdb.txt: name<TAB>version<TAB>files<TAB>pkgdir
 * Each field is a single line; commas separate owned files.
 * Returns 0 on success, -1 on failure.
 */
static int db_append_record(const char *name, const char *version,
                            const char *files, const char *pkgdir)
{
    FILE *fp;
    char dbpath[PATH_MAX_PKT];

    if (name == NULL || version == NULL || pkgdir == NULL || files == NULL) {
        return -1;
    }

    if (pkgdb_path(dbpath, sizeof(dbpath)) != 0) {
        return -1;
    }

    /* fopen "a" opens for append, creating the file if needed. */
    fp = fopen(dbpath, "a");
    if (fp == NULL) {
        perror(dbpath);
        return -1;
    }

    fprintf(fp, "%s\t%s\t%s\t%s\n", name, version, files, pkgdir);
    if (fflush(fp) == EOF) {
        perror("pkg: writing pkgdb");
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/**
 * Look up a package by name in pkgdb.txt. On success, fills `out_name`,
 * `out_version`, `out_files`, and `out_pkgdir` (all heap-allocated strings
 * that the caller must free). Returns 0 if found, 1 if not found, -1 on
 * error.
 */
static int db_lookup(const char *name,
                     char **out_name, char **out_version,
                     char **out_files, char **out_pkgdir)
{
    FILE *fp;
    char dbpath[PATH_MAX_PKT];
    char line[LINE_MAX_PKT];

    if (name == NULL || out_name == NULL || out_version == NULL ||
        out_files == NULL || out_pkgdir == NULL) {
        return -1;
    }

    *out_name = *out_version = *out_files = *out_pkgdir = NULL;

    if (pkgdb_path(dbpath, sizeof(dbpath)) != 0) {
        return -1;
    }

    /* fopen "r" opens for reading; ENOENT is fine (not installed yet). */
    fp = fopen(dbpath, "r");
    if (fp == NULL) {
        if (errno == ENOENT) {
            return 1; /* not installed */
        }
        perror(dbpath);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = trim(line);
        char *fields[DB_MAX_FIELDS];
        char *saveptr = NULL;
        int nfields = 0;
        char *tok;

        /* Skip blank lines and comments. */
        if (*p == '\0' || *p == '#') {
            continue;
        }

        /* Split on tabs. */
        tok = strtok_r(p, "\t", &saveptr);
        while (tok != NULL && nfields < DB_MAX_FIELDS) {
            fields[nfields++] = tok;
            tok = strtok_r(NULL, "\t", &saveptr);
        }

        if (nfields >= DB_MAX_FIELDS && strcmp(fields[0], name) == 0) {
            *out_name    = strdup(fields[0]);
            *out_version = strdup(fields[1]);
            *out_files   = strdup(fields[2]);
            *out_pkgdir  = strdup(fields[3]);
            fclose(fp);

            if (*out_name == NULL || *out_version == NULL ||
                *out_files == NULL || *out_pkgdir == NULL) {
                free(*out_name);
                free(*out_version);
                free(*out_files);
                free(*out_pkgdir);
                return -1;
            }
            return 0;
        }
    }

    fclose(fp);
    return 1; /* not found */
}

/**
 * Remove a package record from pkgdb.txt by rewriting the file without the
 * matching entry. Returns 0 on success, -1 on failure.
 */
static int db_remove_record(const char *name)
{
    FILE *in;
    FILE *out;
    char dbpath[PATH_MAX_PKT];
    char tmppath[PATH_MAX_PKT];
    char line[LINE_MAX_PKT];
    int removed = 0;

    if (name == NULL) {
        return -1;
    }

    if (pkgdb_path(dbpath, sizeof(dbpath)) != 0) {
        return -1;
    }
    if (snprintf(tmppath, sizeof(tmppath), "%s.tmp", dbpath) >=
        (int)sizeof(tmppath)) {
        fputs("pkg: pkgdb temp path too long\n", stderr);
        return -1;
    }

    in = fopen(dbpath, "r");
    if (in == NULL) {
        if (errno == ENOENT) {
            return 0; /* nothing to remove */
        }
        perror(dbpath);
        return -1;
    }

    out = fopen(tmppath, "w");
    if (out == NULL) {
        perror(tmppath);
        fclose(in);
        return -1;
    }

    /* Copy all non-matching lines to the temp file. */
    while (fgets(line, sizeof(line), in) != NULL) {
        char *p = trim(line);
        char *saveptr = NULL;
        char *first = strtok_r(p, "\t", &saveptr);

        /* Skip comment/blank lines (preserved as-is). */
        if (*p == '\0' || *p == '#') {
            fputs(line, out);
            continue;
        }

        if (first != NULL && strcmp(first, name) == 0) {
            removed = 1;
            continue; /* drop this line */
        }
        fputs(line, out);
    }

    if (ferror(in)) {
        perror("pkg: reading pkgdb");
        fclose(in);
        fclose(out);
        unlink(tmppath);
        return -1;
    }
    fclose(in);

    if (fflush(out) == EOF) {
        perror("pkg: writing pkgdb temp");
        fclose(out);
        unlink(tmppath);
        return -1;
    }
    fclose(out);

    /* rename() atomically replaces the old pkgdb with the filtered one. */
    if (rename(tmppath, dbpath) != 0) {
        perror("pkg: renaming pkgdb temp");
        unlink(tmppath);
        return -1;
    }

    (void)removed;
    return 0;
}

/**
 * Print all installed packages from pkgdb.txt.
 * Returns 0 on success, -1 on failure.
 */
static int db_list_all(void)
{
    FILE *fp;
    char dbpath[PATH_MAX_PKT];
    char line[LINE_MAX_PKT];
    int count = 0;

    if (pkgdb_path(dbpath, sizeof(dbpath)) != 0) {
        return -1;
    }

    fp = fopen(dbpath, "r");
    if (fp == NULL) {
        if (errno == ENOENT) {
            fputs("No packages installed.\n", stdout);
            return 0;
        }
        perror(dbpath);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = trim(line);
        char name[256];
        char version[128];
        char *saveptr = NULL;

        if (*p == '\0' || *p == '#') {
            continue;
        }

        /* Parse the four tab-separated fields manually for pretty output. */
        char *tok = strtok_r(p, "\t", &saveptr);
        if (tok != NULL) {
            strncpy(name, tok, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';

            tok = strtok_r(NULL, "\t", &saveptr);
            if (tok != NULL) {
                strncpy(version, tok, sizeof(version) - 1);
                version[sizeof(version) - 1] = '\0';
                printf("  %s-%s\n", name, version);
                count++;
            }
        }
    }

    fclose(fp);
    if (count == 0) {
        fputs("No packages installed.\n", stdout);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Subcommand: install                                                */
/* ------------------------------------------------------------------ */

/**
 * Copy a regular file from `src` to `dst`, then ensure it is executable.
 * Returns 0 on success, -1 on failure.
 * Uses fopen/fread/fwrite for portability (no sendfile dependency).
 */
static int copy_file_to_bin(const char *src, const char *dst)
{
    FILE *in;
    FILE *out;
    char buffer[4096];
    size_t n;

    if (src == NULL || dst == NULL) {
        return -1;
    }

    in = fopen(src, "rb"); /* "rb" for binary-safe read. */
    if (in == NULL) {
        perror(src);
        return -1;
    }

    out = fopen(dst, "wb"); /* "wb" for binary-safe write. */
    if (out == NULL) {
        perror(dst);
        fclose(in);
        return -1;
    }

    /* Stream in 4KB chunks to avoid loading large files into memory. */
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) {
            perror("pkg: writing to bin");
            fclose(in);
            fclose(out);
            unlink(dst);
            return -1;
        }
    }

    if (ferror(in)) {
        perror("pkg: reading package file");
        fclose(in);
        fclose(out);
        unlink(dst);
        return -1;
    }

    fclose(in);
    if (fclose(out) != 0) {
        perror("pkg: closing copied file");
        unlink(dst);
        return -1;
    }

    /* chmod() ensures the copied file is executable (0755). */
    if (chmod(dst, 0755) != 0) {
        perror(dst);
        unlink(dst);
        return -1;
    }

    return 0;
}

/**
 * Resolve a file path relative to the package directory.
 * Returns a heap-allocated absolute path that the caller must free,
 * or NULL on allocation failure.
 */
static char *resolves_to_pkgpath(const char *pkgdir, const char *rel)
{
    char *full;

    if (pkgdir == NULL || rel == NULL) {
        return NULL;
    }

    /* If rel is already absolute, dup it directly. */
    if (rel[0] == '/') {
        full = malloc(PATH_MAX_PKT);
        if (full == NULL) {
            return NULL;
        }
        strncpy(full, rel, PATH_MAX_PKT - 1);
        full[PATH_MAX_PKT - 1] = '\0';
        return full;
    }

    full = malloc(PATH_MAX_PKT);
    if (full == NULL) {
        return NULL;
    }
    if (snprintf(full, PATH_MAX_PKT, "%s/%s", pkgdir, rel) >=
        (int)PATH_MAX_PKT) {
        free(full);
        return NULL;
    }
    return full;
}

/**
 * Extract a .tar.gz archive into the destination directory using the
 * external `tar` utility via fork()/execvp(). Waits for the child and
 * returns its exit status (0 = success).
 */
static int extract_tarball(const char *tarball, const char *destdir)
{
    pid_t pid;
    int status;

    if (tarball == NULL || destdir == NULL) {
        return -1;
    }

    /* fork() creates a child process; returns 0 in the child, >0 (child PID)
     * in the parent, -1 on failure. */
    pid = fork();
    if (pid < 0) {
        perror("pkg: fork");
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect nothing special; just exec tar. */
        /* execlp searches PATH for "tar", running: tar -xzf <tarball> -C <dest> */
        execlp("tar", "tar", "-xzf", tarball, "-C", destdir, (char *)NULL);
        /* If execlp returns, it failed (e.g., tar not found in PATH). */
        perror("pkg: execlp tar");
        _exit(127);
    }

    /* Parent: wait for the child to finish. */
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            perror("pkg: waitpid");
            return -1;
        }
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

/**
 * Install a package from a .tar.gz tarball.
 * argv[0] = "pkg", argv[1] = "install", argv[2] = <tarball path>
 */
static int pkg_install(int argc, char **argv)
{
    pkg_manifest_t manifest;
    char pkgdir[PATH_MAX_PKT];
    char *files_list = NULL;
    int i;
    const char *tarball;

    if (argc < 1 || argv == NULL) {
        fputs("pkg: install requires a tarball path\n", stderr);
        return 1;
    }
    tarball = argv[0];

    if (ensure_mish_dirs() != 0) {
        return 1;
    }

    /*
     * Stage 1: extract the tarball into a temporary pkg/ subdirectory.
     * We first extract, then read pkg.json to learn the real name/version.
     */
    {
        char tmppkgdir[PATH_MAX_PKT];
        const char *home = getenv("HOME");
        if (home == NULL) {
            fputs("pkg: HOME is not set\n", stderr);
            return 1;
        }
        if (snprintf(tmppkgdir, sizeof(tmppkgdir), "%s/%s/tmp_pkg.XXXXXX",
                     home, MISH_DIR_NAME) >= (int)sizeof(tmppkgdir)) {
            fputs("pkg: temp pkg path too long\n", stderr);
            return 1;
        }

        /* mkdtemp() creates a unique directory from the template; it
         * modifies tmppkgdir in place and returns a pointer to it. */
        if (mkdtemp(tmppkgdir) == NULL) {
            perror("pkg: mkdtemp");
            return 1;
        }

        /* tar -xzf <tarball> -C <tmppkgdir> */
        if (extract_tarball(tarball, tmppkgdir) != 0) {
            fprintf(stderr, "pkg: extraction failed for %s\n", tarball);
            unlink(tmppkgdir);
            rmdir(tmppkgdir);
            return 1;
        }

        /* Look for pkg.json inside the extracted tree. It may be at the
         * root or inside a single subdirectory. */
        {
            char jsonpath[PATH_MAX_PKT];
            int found = 0;

            /* Build <tmppkgdir>/pkg.json; check for truncation. */
            if (snprintf(jsonpath, sizeof(jsonpath), "%s/pkg.json",
                         tmppkgdir) >= (int)sizeof(jsonpath)) {
                fputs("pkg: package path too long for pkg.json\n", stderr);
                free_pkg_manifest(&manifest);
                unlink(tmppkgdir);
                rmdir(tmppkgdir);
                return 1;
            }
            if (access(jsonpath, R_OK) == 0) {
                found = 1;
            } else {
                /* Scan immediate subdirectories for pkg.json. */
                DIR *d = opendir(tmppkgdir);
                if (d != NULL) {
                    struct dirent *de;
                    while ((de = readdir(d)) != NULL) {
                        char sub[PATH_MAX_PKT];
                        /* Build <tmppkgdir>/<entry>; check truncation. */
                        if (snprintf(sub, sizeof(sub), "%s/%s", tmppkgdir,
                                     de->d_name) >= (int)sizeof(sub)) {
                            continue; /* skip overly long names */
                        }
                        /* Skip "." and "..". */
                        if (strcmp(de->d_name, ".") == 0 ||
                            strcmp(de->d_name, "..") == 0) {
                            continue;
                        }
                        /* Only consider directories. */
                        struct stat st;
                        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
                            if (snprintf(jsonpath, sizeof(jsonpath),
                                         "%s/pkg.json", sub) >=
                                (int)sizeof(jsonpath)) {
                                continue;
                            }
                            if (access(jsonpath, R_OK) == 0) {
                                strncpy(tmppkgdir, sub,
                                        sizeof(tmppkgdir) - 1);
                                tmppkgdir[sizeof(tmppkgdir) - 1] = '\0';
                                found = 1;
                                break;
                            }
                        }
                    }
                    closedir(d);
                }
            }

            if (!found) {
                fprintf(stderr, "pkg: pkg.json not found in %s\n", tarball);
                unlink(tmppkgdir);
                rmdir(tmppkgdir);
                return 1;
            }

            /* Parse pkg.json. */
            if (parse_pkg_json(jsonpath, &manifest) != 0) {
                fprintf(stderr, "pkg: failed to parse %s\n", jsonpath);
                unlink(tmppkgdir);
                rmdir(tmppkgdir);
                return 1;
            }
        }

        /* Compute the final destination: ~/.mysh/pkgs/<name>-<version> */
        {
            const char *home = getenv("HOME");
            char finalpkgdir[PATH_MAX_PKT];
            if (home == NULL) {
                fputs("pkg: HOME is not set\n", stderr);
                free_pkg_manifest(&manifest);
                unlink(tmppkgdir);
                rmdir(tmppkgdir);
                return 1;
            }
            snprintf(finalpkgdir, sizeof(finalpkgdir), "%s/%s/%s-%s",
                     home, MISH_PKGS_DIR, manifest.name, manifest.version);

            /* If the destination already exists, remove it first. */
            {
                struct stat st;
                if (stat(finalpkgdir, &st) == 0) {
                    /* Recurse-remove the existing dir (rm -rf). */
                    char cmd[PATH_MAX_PKT * 2];
                    snprintf(cmd, sizeof(cmd), "rm -rf %s", finalpkgdir);
                    if (system(cmd) != 0) {
                        fprintf(stderr,
                                "pkg: failed to remove existing %s\n",
                                finalpkgdir);
                        free_pkg_manifest(&manifest);
                        unlink(tmppkgdir);
                        rmdir(tmppkgdir);
                        return 1;
                    }
                }
            }

            /* rename() moves the extracted tree to its final home. */
            if (rename(tmppkgdir, finalpkgdir) != 0) {
                perror("pkg: renaming package directory");
                free_pkg_manifest(&manifest);
                unlink(tmppkgdir);
                rmdir(tmppkgdir);
                return 1;
            }

            /* pkgdir now holds the final resting place of the package. */
            snprintf(pkgdir, sizeof(pkgdir), "%s", finalpkgdir);
        }
    } /* end Stage 1: extraction & directory move */

    /*
     * Stage 2: copy each listed file into ~/.mysh/bin/.
     * The files listed in pkg.json are relative to the package root
     * (the extracted pkgdir).
     */
    {
        const char *home = getenv("HOME");
        char bindir[PATH_MAX_PKT];

        if (home == NULL || home[0] == '\0') {
            fputs("pkg: HOME is not set\n", stderr);
            free_pkg_manifest(&manifest);
            free(files_list);
            return 1;
        }
        if (snprintf(bindir, sizeof(bindir), "%s/%s", home, MISH_BIN_DIR) >=
            (int)sizeof(bindir)) {
            fputs("pkg: bin path too long\n", stderr);
            free_pkg_manifest(&manifest);
            free(files_list);
            return 1;
        }

        for (i = 0; i < manifest.file_count; ++i) {
            char dst[PATH_MAX_PKT];
            char *src;
            const char *base;

            /* Resolve the source path inside the package dir. */
            src = resolves_to_pkgpath(pkgdir, manifest.files[i]);
            if (src == NULL) {
                fprintf(stderr, "pkg: failed to resolve %s\n",
                        manifest.files[i]);
                free_pkg_manifest(&manifest);
                free(files_list);
                return 1;
            }

            /* Destination basename = the final path component of the source. */
            base = strrchr(src, '/');
            base = base != NULL ? base + 1 : src;

            if (snprintf(dst, sizeof(dst), "%s/%s", bindir, base) >=
                (int)sizeof(dst)) {
                fprintf(stderr, "pkg: bin path too long for %s\n", base);
                free(src);
                free_pkg_manifest(&manifest);
                free(files_list);
                return 1;
            }

            /* Copy the file into bin/ and make it executable. */
            if (copy_file_to_bin(src, dst) != 0) {
                free(src);
                free_pkg_manifest(&manifest);
                free(files_list);
                return 1;
            }

            printf("pkg: installed %s -> %s\n", src, dst);

            /* Track this bin file in the comma-separated list for pkgdb. */
            if (db_append_file(&files_list, dst) != 0) {
                free(src);
                free_pkg_manifest(&manifest);
                free(files_list);
                return 1;
            }

            free(src);
        }
    }

    if (files_list == NULL) {
        files_list = strdup("");
        if (files_list == NULL) {
            free_pkg_manifest(&manifest);
            return 1;
        }
    }

    /* Stage 3: record the installation in pkgdb.txt. */
    if (db_append_record(manifest.name, manifest.version, files_list,
                         pkgdir) != 0) {
        fprintf(stderr, "pkg: failed to update package database for %s\n",
                manifest.name);
        free(files_list);
        free_pkg_manifest(&manifest);
        return 1;
    }

    printf("pkg: %s-%s installed successfully.\n", manifest.name,
           manifest.version);

    free(files_list);
    free_pkg_manifest(&manifest);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Subcommand: list                                                   */
/* ------------------------------------------------------------------ */

static int pkg_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fputs("Installed packages:\n", stdout);
    return db_list_all();
}

/* ------------------------------------------------------------------ */
/* Subcommand: info                                                   */
/* ------------------------------------------------------------------ */

static int pkg_info(int argc, char **argv)
{
    char *name = NULL;
    char *version = NULL;
    char *files = NULL;
    char *pkgdir = NULL;
    char pkgjson[PATH_MAX_PKT];
    pkg_manifest_t manifest;
    int rc;

    if (argc < 1 || argv == NULL || argv[0] == NULL) {
        fputs("pkg: info requires a package name\n", stderr);
        return 1;
    }
    name = argv[0];

    /* Look up the package in pkgdb.txt. */
    rc = db_lookup(name, &name, &version, &files, &pkgdir);
    if (rc == 1) {
        fprintf(stderr, "pkg: package not installed: %s\n", argv[0]);
        return 1;
    }
    if (rc == -1) {
        return 1;
    }

    printf("Name:    %s\n", name);
    printf("Version: %s\n", version);

    /* Read pkg.json from the installed package directory for description. */
    if (pkgdir != NULL) {
        snprintf(pkgjson, sizeof(pkgjson), "%s/pkg.json", pkgdir);
        if (parse_pkg_json(pkgjson, &manifest) == 0) {
            if (manifest.description[0] != '\0') {
                printf("Desc:    %s\n", manifest.description);
            }
            printf("Files:   %s\n",
                   manifest.file_count > 0 ? manifest.files[0] : "(none)");
            int j;
            for (j = 1; j < manifest.file_count; ++j) {
                printf("         %s\n", manifest.files[j]);
            }
            free_pkg_manifest(&manifest);
        } else {
            printf("Files:   (pkg.json unreadable)\n");
        }
    } else {
        printf("Files:   (none)\n");
    }

    printf("Location: %s\n", pkgdir != NULL ? pkgdir : "(unknown)");

    free(name);
    free(version);
    free(files);
    free(pkgdir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Subcommand: remove                                                 */
/* ------------------------------------------------------------------ */

/**
 * Remove a file or directory tree at `path` using a system rm -rf call.
 * (A full recursive rmdir implementation would be longer; rm -rf is
 *  sufficient and matches shell semantics.)
 */
static int remove_path(const char *path)
{
    char cmd[PATH_MAX_PKT * 2];

    if (path == NULL) {
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", path);
    /* system() invokes /bin/sh -c "<cmd>"; returns the shell's exit status. */
    if (system(cmd) != 0) {
        fprintf(stderr, "pkg: failed to remove %s\n", path);
        return -1;
    }
    return 0;
}

static int pkg_remove(int argc, char **argv)
{
    char *name = NULL;
    char *version = NULL;
    char *files = NULL;
    char *pkgdir = NULL;
    int rc;

    if (argc < 1 || argv == NULL || argv[0] == NULL) {
        fputs("pkg: remove requires a package name\n", stderr);
        return 1;
    }
    name = argv[0];

    rc = db_lookup(name, &name, &version, &files, &pkgdir);
    if (rc == 1) {
        fprintf(stderr, "pkg: package not installed: %s\n", argv[0]);
        return 1;
    }
    if (rc == -1) {
        return 1;
    }

    printf("pkg: removing %s-%s\n", name, version);

    /* Step 1: remove each owned file in ~/.mysh/bin/. */
    if (files != NULL && files[0] != '\0') {
        char *saveptr = NULL;
        char *tok = strtok_r(files, ",", &saveptr);
        while (tok != NULL) {
            if (unlink(tok) != 0) {
                fprintf(stderr, "pkg: warning: could not unlink %s: %s\n",
                        tok, strerror(errno));
            } else {
                printf("pkg: removed %s\n", tok);
            }
            tok = strtok_r(NULL, ",", &saveptr);
        }
    }

    /* Step 2: remove the package directory ~/.mysh/pkgs/<name>-<version>. */
    if (pkgdir != NULL) {
        if (remove_path(pkgdir) == 0) {
            printf("pkg: removed package directory %s\n", pkgdir);
        }
    }

    /* Step 3: remove the record from pkgdb.txt. */
    if (db_remove_record(argv[0]) != 0) {
        fprintf(stderr, "pkg: warning: could not update pkgdb for %s\n",
                argv[0]);
    }

    printf("pkg: %s-%s removed.\n", name, version);

    free(name);
    free(version);
    free(files);
    free(pkgdir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatch table for subcommands                                     */
/* ------------------------------------------------------------------ */

typedef int (*pkg_subcmd_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    pkg_subcmd_fn run;
    const char *summary;
} pkg_subcmd_t;

static const pkg_subcmd_t pkg_subcommands[] = {
    { "install", pkg_install, "install a package from a .tar.gz tarball" },
    { "list",    pkg_list,    "list all installed packages"          },
    { "info",    pkg_info,    "show details about an installed package" },
    { "remove",  pkg_remove,  "remove an installed package"          },
};

static const int pkg_subcommand_count =
    (int)(sizeof(pkg_subcommands) / sizeof(pkg_subcommands[0]));

/**
 * Find a subcommand by name. Returns the matching entry or NULL.
 */
static const pkg_subcmd_t *pkg_find_subcommand(const char *name)
{
    int i;
    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < pkg_subcommand_count; ++i) {
        if (strcmp(pkg_subcommands[i].name, name) == 0) {
            return &pkg_subcommands[i];
        }
    }
    return NULL;
}

/**
 * Print the list of available pkg subcommands.
 */
static void pkg_print_subcommand_summary(FILE *out)
{
    int i;
    if (out == NULL) {
        return;
    }
    fputs("Subcommands:\n", out);
    for (i = 0; i < pkg_subcommand_count; ++i) {
        fprintf(out, "  %-10s - %s\n", pkg_subcommands[i].name,
                pkg_subcommands[i].summary);
    }
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                   */
/* ------------------------------------------------------------------ */

/**
 * Main execution entry point for the pkg command.
 * Returns 0 on success, 1 on any error.
 */
int pkg_run(int argc, char **argv)
{
    pkg_argtable_t args;
    int nerrors;
    const pkg_subcmd_t *sub;
    const char *subcmd_name;

    if (build_pkg_argtable(&args) != 0) {
        fputs("pkg: failed to allocate argument table\n", stderr);
        return 1;
    }

    /* arg_parse() processes argv against the argtable3 definitions. */
    nerrors = arg_parse(argc, argv, args.table);

    /* Help flag takes priority over syntax errors. */
    if (args.help->count > 0) {
        print_usage_with_argtable(stdout, args.table);
        fputs("\n", stdout);
        pkg_print_subcommand_summary(stdout);
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

    /*
     * Dispatch: argv[1] is the subcommand name (captured by the first
     * arg_filen). argv[2..] are the operands (captured by the second
     * arg_filen). We pass the operands as a fresh argv slice.
     */
    if (args.subcmd->count < 1) {
        /* Should not happen because mincount=1 on the subcommand arg. */
        fputs("pkg: missing subcommand\n", stderr);
        print_usage_with_argtable(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    subcmd_name = args.subcmd->filename[0];
    sub = pkg_find_subcommand(subcmd_name);
    if (sub == NULL) {
        fprintf(stderr, "pkg: unknown subcommand: %s\n", subcmd_name);
        print_usage_with_argtable(stderr, args.table);
        arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
        return 1;
    }

    /* Build an argv slice for the subcommand's operands only.
     * sub_argv[0] is the first operand (e.g. the tarball path), NOT the
     * subcommand name — the subcommand handlers expect operands starting
     * at index 0, mirroring how each cmd_* run() treats argv[0] as the
     * command name. */
    {
        char **sub_argv;
        int sub_argc;
        int i;

        /* Count operands that argtable collected. */
        sub_argc = args.operands->count;
        sub_argv = malloc((size_t)(sub_argc + 1) * sizeof(char *));
        if (sub_argv == NULL) {
            arg_freetable(args.table,
                          sizeof(args.table) / sizeof(args.table[0]));
            return 1;
        }

        for (i = 0; i < sub_argc; ++i) {
            sub_argv[i] = (char *)args.operands->filename[i];
        }
        sub_argv[sub_argc] = NULL;

        nerrors = sub->run(sub_argc, sub_argv);

        free(sub_argv);
    }

    arg_freetable(args.table, sizeof(args.table) / sizeof(args.table[0]));
    return nerrors;
}

cmd_spec_t cmd_pkg_spec = {
    "pkg",
    "manage shell packages",
    "A minimal package manager for AiShell. Installs packages from .tar.gz "
    "tarballs into ~/.mysh/, exposes their executables in ~/.mysh/bin/, and "
    "tracks installations in ~/.mysh/pkgdb.txt. "
    "Subcommands: install <tarball>, list, info <name>, remove <name>.",
    pkg_run,
    pkg_print_usage
};

void register_pkg_command(void)
{
    register_command(&cmd_pkg_spec);
}

/* Standalone entry point for direct invocation outside AiShell.
 * Linked when the STANDALONE macro is defined during compilation. */
#ifdef STANDALONE
int main(int argc, char **argv)
{
    return cmd_pkg_spec.run(argc, argv);
}
#endif
