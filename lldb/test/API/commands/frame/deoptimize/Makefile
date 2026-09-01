C_SOURCES := main.c

# The command exists for optimised code, so the inferior has to be optimised or
# the test proves nothing: at -O0 every local is already in a slot and readable.
CFLAGS_EXTRAS += -O2

include Makefile.rules
