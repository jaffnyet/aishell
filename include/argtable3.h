#ifndef ARGTABLE3_H
#define ARGTABLE3_H

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum arg_type {
    ARG_TYPE_LITERAL = 1,
    ARG_TYPE_FILE,
    ARG_TYPE_END,
    ARG_TYPE_INTEGER
};

typedef struct arg_hdr {
    int type;
    char flag;
    const char *shortopts;
    const char *longopts;
    const char *datatype;
    const char *glossary;
    int mincount;
    int maxcount;
    void *parent;
    void (*resetfn)(struct arg_hdr *parent);
    int (*scanfn)(struct arg_hdr *parent, const char *argval);
    int (*checkfn)(struct arg_hdr *parent);
    void (*errorfn)(struct arg_hdr *parent, FILE *fp, int error, const char *argval, const char *progname);
} arg_hdr_t;

typedef struct arg_lit {
    struct arg_hdr hdr;
    int count;
} arg_lit_t;

typedef struct arg_file {
    struct arg_hdr hdr;
    int count;
    const char **filename;
    const char **basename;
    const char **extension;
} arg_file_t;

typedef struct arg_int {
    struct arg_hdr hdr;
    int count;
    int *ival;
} arg_int_t;

typedef struct arg_end {
    struct arg_hdr hdr;
    int count;
    int *error;
    void **parent;
    const char **argval;
} arg_end_t;

struct arg_lit* arg_litn(const char* shortopts, const char* longopts, int mincount, int maxcount, const char* glossary);
struct arg_lit* arg_lit0(const char* shortopts, const char* longopts, const char* glossary);
struct arg_file* arg_file0(const char* shortopts, const char* longopts, const char* datatype, const char* glossary);
struct arg_file* arg_filen(const char* shortopts, const char* longopts, const char* datatype, int mincount, int maxcount, const char* glossary);
struct arg_int* arg_intn(const char* shortopts, const char* longopts, const char* datatype, int mincount, int maxcount, const char* glossary);
struct arg_int* arg_int0(const char* shortopts, const char* longopts, const char* datatype, const char* glossary);
struct arg_end* arg_end(int maxerrors);

int arg_parse(int argc, char **argv, void **argtable);
void arg_print_syntax(FILE *fp, void **argtable, const char *suffix);
void arg_print_glossary(FILE *fp, void **argtable, const char *format);
void arg_print_errors(FILE *fp, struct arg_end *end, const char *progname);
void arg_freetable(void **argtable, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ARGTABLE3_H */
