# Standalone build for the C hello plugin. CMake also builds this target.
ROOT := ../..
CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra
CPPFLAGS += -I$(ROOT)/include -I$(ROOT)/third_party/jsmn
SOURCES = main.c $(ROOT)/src/plugin/client.c $(ROOT)/src/plugin/ipc.c \
          $(ROOT)/src/plugin/json.c $(ROOT)/src/plugin/jsmn_impl.c

hello: $(SOURCES)
	mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o bin/hello

clean:
	rm -f bin/hello

.PHONY: clean
