#ifndef CMD_SPEC_H
#define CMD_SPEC_H

#include <stdio.h>

typedef struct cmd_spec {
    const char *name;
    const char *summary;
    const char *long_help;
    int (*run)(int argc, char **argv);
    void (*print_usage)(FILE *out);
} cmd_spec_t;

void register_command(const cmd_spec_t *spec);
const cmd_spec_t *find_command(const char *name);
void for_each_command(
    void (*cb)(const cmd_spec_t *spec, void *userdata),
    void *userdata
);

#endif
