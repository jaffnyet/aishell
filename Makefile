CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -std=c99

# Command source files for built-in modules
COMMAND_SOURCES = \
	src/commands/cmd_cat.c \
	src/commands/cmd_ls.c \
	src/commands/cmd_stat.c \
	src/commands/cmd_head.c \
	src/commands/cmd_tail.c \
	src/commands/cmd_cp.c \
	src/commands/cmd_mv.c \
	src/commands/cmd_rm.c \
	src/commands/cmd_mkdir.c \
	src/commands/cmd_rmdir.c \
	src/commands/cmd_touch.c \
	src/commands/cmd_cd.c \
	src/commands/cmd_pwd.c \
	src/commands/cmd_echo.c

SOURCES = src/shell_main.c $(COMMAND_SOURCES) src/argtable3.c src/registry.c
OBJECTS = $(SOURCES:.c=.o)

# Standalone command binaries (compiled with -DSTANDALONE)
STANDALONE_BINS = $(addprefix cmd_,cat ls cp mv rm mkdir rmdir head tail stat touch cd pwd echo)

.PHONY: all standalone clean

all: aishell

aishell: $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

# Standalone binary: compile command source with -DSTANDALONE (activates main())
# and link with argtable3 + registry in a single step to avoid object conflicts.
cmd_%: src/commands/cmd_%.c src/registry.o src/argtable3.o
	$(CC) $(CFLAGS) -DSTANDALONE $< src/registry.o src/argtable3.o -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

standalone: $(STANDALONE_BINS)

clean:
	rm -f $(SOURCES:.c=.o) aishell $(STANDALONE_BINS) \
		test.txt \
		tests/test_cat_error.c \
		tests/test_cat_error \
		tests/test_cat_error.log \
		tests/aishell_test.log
