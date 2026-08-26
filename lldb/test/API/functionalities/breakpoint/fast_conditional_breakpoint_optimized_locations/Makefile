C_SOURCES := main.c

# Optimized on purpose: this is where a local stops being a frame offset and
# becomes a register, or gets folded to a constant, which are the location forms
# the unoptimized directory next door cannot produce.
CFLAGS_EXTRAS += -O1

include Makefile.rules
