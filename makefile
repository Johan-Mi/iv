override CFLAGS += -Wall -Wextra -Wpedantic -Wconversion -Wno-keyword-macro -Wno-gnu-auto-type
override LDLIBS += -lX11

all: main
.PHONY: all

compile_flags.txt: makefile
	$(file >$@)
	$(foreach O,$(CFLAGS),$(file >>$@,$O))
