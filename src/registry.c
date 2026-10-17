#include "cmd_spec.h"

#include <stddef.h>
#include <string.h>

#define COMMAND_CAPACITY 64

static const cmd_spec_t *commands[COMMAND_CAPACITY];
static size_t command_count;

/* Register a command specification into the registry */
void register_command(const cmd_spec_t *spec)
{
    if (spec == NULL || spec->name == NULL || command_count >= COMMAND_CAPACITY) {
        return;
    }

    commands[command_count++] = spec;
}

/* Look up a command by its name */
const cmd_spec_t *find_command(const char *name)
{
    size_t i;

    if (name == NULL) {
        return NULL;
    }

    for (i = 0; i < command_count; ++i) {
        if (strcmp(commands[i]->name, name) == 0) {
            return commands[i];
        }
    }

    return NULL;
}

/* Enumerate all registered commands using a callback */
void for_each_command(
    void (*cb)(const cmd_spec_t *spec, void *userdata),
    void *userdata
)
{
    size_t i;

    if (cb == NULL) {
        return;
    }

    for (i = 0; i < command_count; ++i) {
        cb(commands[i], userdata);
    }
}
