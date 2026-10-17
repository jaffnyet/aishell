#include "argtable3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

struct arg_lit* arg_litn(const char* shortopts, const char* longopts, int mincount, int maxcount, const char* glossary) {
    struct arg_lit *result = (struct arg_lit*)calloc(1, sizeof(struct arg_lit));
    if (!result) return NULL;
    result->hdr.type = ARG_TYPE_LITERAL;
    result->hdr.shortopts = shortopts;
    result->hdr.longopts = longopts;
    result->hdr.glossary = glossary;
    result->hdr.mincount = mincount;
    result->hdr.maxcount = maxcount;
    result->count = 0;
    return result;
}

struct arg_lit* arg_lit0(const char* shortopts, const char* longopts, const char* glossary) {
    return arg_litn(shortopts, longopts, 0, 1, glossary);
}

struct arg_file* arg_filen(const char* shortopts, const char* longopts, const char* datatype, int mincount, int maxcount, const char* glossary) {
    struct arg_file *result;

    if (mincount < 0 || maxcount < mincount) return NULL;
    result = (struct arg_file*)calloc(1, sizeof(struct arg_file));
    if (!result) return NULL;
    result->hdr.type = ARG_TYPE_FILE;
    result->hdr.shortopts = shortopts;
    result->hdr.longopts = longopts;
    result->hdr.datatype = datatype ? datatype : "<file>";
    result->hdr.glossary = glossary;
    result->hdr.mincount = mincount;
    result->hdr.maxcount = maxcount;
    result->filename = (const char**)calloc((size_t)maxcount, sizeof(char*));
    result->basename = (const char**)calloc((size_t)maxcount, sizeof(char*));
    result->extension = (const char**)calloc((size_t)maxcount, sizeof(char*));
    if (maxcount > 0 &&
        (!result->filename || !result->basename || !result->extension)) {
        free((void*)result->filename);
        free((void*)result->basename);
        free((void*)result->extension);
        free(result);
        return NULL;
    }
    result->count = 0;
    return result;
}

struct arg_file* arg_file0(const char* shortopts, const char* longopts, const char* datatype, const char* glossary) {
    return arg_filen(shortopts, longopts, datatype, 0, 1, glossary);
}

struct arg_int* arg_intn(const char* shortopts, const char* longopts,
                         const char* datatype, int mincount, int maxcount,
                         const char* glossary) {
    struct arg_int *result;

    if (mincount < 0 || maxcount < mincount || maxcount <= 0) return NULL;
    result = (struct arg_int*)calloc(1, sizeof(struct arg_int));
    if (!result) return NULL;
    result->ival = (int*)calloc((size_t)maxcount, sizeof(*result->ival));
    if (!result->ival) {
        free(result);
        return NULL;
    }
    result->hdr.type = ARG_TYPE_INTEGER;
    result->hdr.shortopts = shortopts;
    result->hdr.longopts = longopts;
    result->hdr.datatype = datatype ? datatype : "<int>";
    result->hdr.glossary = glossary;
    result->hdr.mincount = mincount;
    result->hdr.maxcount = maxcount;
    return result;
}

struct arg_int* arg_int0(const char* shortopts, const char* longopts,
                         const char* datatype, const char* glossary) {
    return arg_intn(shortopts, longopts, datatype, 0, 1, glossary);
}

struct arg_end* arg_end(int maxerrors) {
    struct arg_end *result;

    if (maxerrors <= 0) return NULL;
    result = (struct arg_end*)calloc(1, sizeof(struct arg_end));
    if (!result) return NULL;
    result->hdr.type = ARG_TYPE_END;
    result->hdr.mincount = 0;
    result->hdr.maxcount = maxerrors;
    result->error = (int*)calloc((size_t)maxerrors, sizeof(int));
    result->parent = (void**)calloc((size_t)maxerrors, sizeof(void*));
    result->argval = (const char**)calloc((size_t)maxerrors, sizeof(char*));
    if (!result->error || !result->parent || !result->argval) {
        free(result->error);
        free(result->parent);
        free((void*)result->argval);
        free(result);
        return NULL;
    }
    result->count = 0;
    return result;
}

static void add_error(struct arg_end *end, void *parent, int err_code, const char *argval) {
    if (end && end->count < end->hdr.maxcount) {
        end->error[end->count] = err_code;
        end->parent[end->count] = parent;
        end->argval[end->count] = argval;
        end->count++;
    }
}

int arg_parse(int argc, char **argv, void **argtable) {
    struct arg_end *end = NULL;
    int tablen = 0;

    while (argtable[tablen] != NULL) {
        struct arg_hdr *h = (struct arg_hdr*)argtable[tablen];
        if (h->type == ARG_TYPE_END) {
            end = (struct arg_end*)h;
            end->count = 0;
        } else if (h->type == ARG_TYPE_LITERAL) {
            struct arg_lit *l = (struct arg_lit*)h;
            l->count = 0;
        } else if (h->type == ARG_TYPE_FILE) {
            struct arg_file *f = (struct arg_file*)h;
            f->count = 0;
        } else if (h->type == ARG_TYPE_INTEGER) {
            struct arg_int *arg = (struct arg_int*)h;
            arg->count = 0;
        }
        tablen++;
    }

    int file_idx = 0;
    for (int i = 1; i < argc; i++) {
        const char *token = argv[i];
        int matched = 0;

        if (token[0] == '-') {
            const char *opt = token + 1;
            int is_long = (token[1] == '-');
            if (is_long) opt = token + 2;

            for (int t = 0; t < tablen; t++) {
                struct arg_hdr *h = (struct arg_hdr*)argtable[t];
                if (h == (struct arg_hdr*)end) continue;

                if (is_long && h->longopts && strcmp(h->longopts, opt) == 0) {
                    if (h->type == ARG_TYPE_LITERAL) {
                        ((struct arg_lit*)h)->count++;
                        matched = 1;
                        break;
                    }
                    if (h->type == ARG_TYPE_INTEGER) {
                        struct arg_int *arg = (struct arg_int*)h;
                        char *endptr = NULL;
                        long value;
                        if (i + 1 >= argc) {
                            add_error(end, h, 4, token);
                        } else {
                            errno = 0;
                            value = strtol(argv[++i], &endptr, 10);
                            if (errno == ERANGE || endptr == argv[i] ||
                                *endptr != '\0' || value < INT_MIN ||
                                value > INT_MAX) {
                                add_error(end, h, 5, argv[i]);
                            } else if (arg->count >= h->maxcount) {
                                add_error(end, h, 6, token);
                            } else {
                                arg->ival[arg->count++] = (int)value;
                            }
                        }
                        matched = 1;
                        break;
                    }
                } else if (!is_long && h->shortopts && strcmp(h->shortopts, opt) == 0) {
                    if (h->type == ARG_TYPE_LITERAL) {
                        ((struct arg_lit*)h)->count++;
                        matched = 1;
                        break;
                    }
                    if (h->type == ARG_TYPE_INTEGER) {
                        struct arg_int *arg = (struct arg_int*)h;
                        char *endptr = NULL;
                        long value;
                        if (i + 1 >= argc) {
                            add_error(end, h, 4, token);
                        } else {
                            errno = 0;
                            value = strtol(argv[++i], &endptr, 10);
                            if (errno == ERANGE || endptr == argv[i] ||
                                *endptr != '\0' || value < INT_MIN ||
                                value > INT_MAX) {
                                add_error(end, h, 5, argv[i]);
                            } else if (arg->count >= h->maxcount) {
                                add_error(end, h, 6, token);
                            } else {
                                arg->ival[arg->count++] = (int)value;
                            }
                        }
                        matched = 1;
                        break;
                    }
                }
            }
            if (!matched && end) {
                add_error(end, NULL, 1, token);
            }
        } else {
            for (int t = file_idx; t < tablen; t++) {
                struct arg_hdr *h = (struct arg_hdr*)argtable[t];
                if (h == (struct arg_hdr*)end) continue;
                if (h->type == ARG_TYPE_FILE) {
                    struct arg_file *f = (struct arg_file*)h;
                    if (f->count < f->hdr.maxcount) {
                        f->filename[f->count++] = token;
                        matched = 1;
                        if (f->count >= f->hdr.maxcount) {
                            file_idx = t + 1;
                        }
                        break;
                    }
                }
            }
            if (!matched && end) {
                add_error(end, NULL, 2, token);
            }
        }
    }

    for (int t = 0; t < tablen; t++) {
        struct arg_hdr *h = (struct arg_hdr*)argtable[t];
        if (h == (struct arg_hdr*)end) continue;
        if (h->type == ARG_TYPE_FILE) {
            struct arg_file *f = (struct arg_file*)h;
            if (f->count < f->hdr.mincount && end) {
                add_error(end, f, 3, f->hdr.datatype);
            }
        }
    }

    return end ? end->count : 0;
}

void arg_print_syntax(FILE *fp, void **argtable, const char *suffix) {
    int i = 0;
    while (argtable[i] != NULL) {
        struct arg_hdr *h = (struct arg_hdr*)argtable[i];
        if (h->type == ARG_TYPE_INTEGER) {
            fprintf(fp, "[-%s", h->shortopts ? h->shortopts : "");
            if (h->longopts) fprintf(fp, "|--%s", h->longopts);
            fprintf(fp, " %s] ", h->datatype ? h->datatype : "<int>");
        } else if (h->shortopts || h->longopts) {
            if (h->shortopts && h->longopts)
                fprintf(fp, "[-%s|--%s] ", h->shortopts, h->longopts);
            else if (h->shortopts)
                fprintf(fp, "[-%s] ", h->shortopts);
            else
                fprintf(fp, "[--%s] ", h->longopts);
        } else if (h->type == ARG_TYPE_FILE) {
            if (h->mincount == 0)
                fprintf(fp, "[%s...] ", h->datatype);
            else
                fprintf(fp, "%s... ", h->datatype);
        }
        i++;
    }
    if (suffix) fprintf(fp, "%s", suffix);
}

void arg_print_glossary(FILE *fp, void **argtable, const char *format) {
    int i = 0;
    char optbuf[128];
    while (argtable[i] != NULL) {
        struct arg_hdr *h = (struct arg_hdr*)argtable[i];
        if (h->glossary) {
            if (h->type == ARG_TYPE_INTEGER && h->shortopts && h->longopts) {
                snprintf(optbuf, sizeof(optbuf), "-%s, --%s %s",
                         h->shortopts, h->longopts,
                         h->datatype ? h->datatype : "<int>");
            } else if (h->type == ARG_TYPE_INTEGER && h->shortopts) {
                snprintf(optbuf, sizeof(optbuf), "-%s %s",
                         h->shortopts, h->datatype ? h->datatype : "<int>");
            } else if (h->type == ARG_TYPE_INTEGER && h->longopts) {
                snprintf(optbuf, sizeof(optbuf), "--%s %s",
                         h->longopts, h->datatype ? h->datatype : "<int>");
            } else if (h->shortopts && h->longopts) {
                snprintf(optbuf, sizeof(optbuf), "-%s, --%s", h->shortopts, h->longopts);
            } else if (h->shortopts) {
                snprintf(optbuf, sizeof(optbuf), "-%s", h->shortopts);
            } else if (h->longopts) {
                snprintf(optbuf, sizeof(optbuf), "--%s", h->longopts);
            } else if (h->type == ARG_TYPE_FILE || h->type == ARG_TYPE_INTEGER) {
                snprintf(optbuf, sizeof(optbuf), "%s", h->datatype);
            } else {
                optbuf[0] = '\0';
            }
            fprintf(fp, format ? format : "  %-20s %s\n", optbuf, h->glossary);
        }
        i++;
    }
}

void arg_print_errors(FILE *fp, struct arg_end *end, const char *progname) {
    if (!end) return;
    for (int i = 0; i < end->count; i++) {
        int code = end->error[i];
        const char *val = end->argval[i];
        if (code == 1) {
            fprintf(fp, "%s: invalid option -- '%s'\n", progname ? progname : "error", val);
        } else if (code == 2) {
            fprintf(fp, "%s: unexpected argument -- '%s'\n", progname ? progname : "error", val);
        } else if (code == 3) {
            fprintf(fp, "%s: missing required argument '%s'\n", progname ? progname : "error", val);
        } else if (code == 4) {
            fprintf(fp, "%s: option requires an argument -- '%s'\n",
                    progname ? progname : "error", val);
        } else if (code == 5) {
            fprintf(fp, "%s: invalid integer argument -- '%s'\n",
                    progname ? progname : "error", val);
        } else if (code == 6) {
            fprintf(fp, "%s: option specified too many times -- '%s'\n",
                    progname ? progname : "error", val);
        } else {
            fprintf(fp, "%s: syntax error\n", progname ? progname : "error");
        }
    }
}

void arg_freetable(void **argtable, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!argtable[i]) continue;
        struct arg_hdr *h = (struct arg_hdr*)argtable[i];
        if (h->type == ARG_TYPE_FILE) {
            struct arg_file *f = (struct arg_file*)h;
            free((void*)f->filename);
            free((void*)f->basename);
            free((void*)f->extension);
        } else if (h->type == ARG_TYPE_INTEGER) {
            struct arg_int *arg = (struct arg_int*)h;
            free(arg->ival);
        } else if (h->type == ARG_TYPE_END) {
            struct arg_end *e = (struct arg_end*)h;
            free((void*)e->error);
            free((void*)e->parent);
            free((void*)e->argval);
        }
        free(argtable[i]);
    }
}
