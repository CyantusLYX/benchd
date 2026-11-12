# benchd — multi-user remote serial console (Network Programming midterm)
#
#   make / make release   -O2, LOG_COMPILE_LEVEL=2 (INFO)   -> build/bin, build/lib
#   make debug            -O0 -g, LOG_COMPILE_LEVEL=4 (TRACE), fault injection
#                                                           -> build/debug/bin, build/debug/lib
#   make test | stress | capture | clean | dist
#
# Release and debug objects live in separate directories, so switching never mixes them.

CC      ?= gcc
MODE    ?= release
STUDENT := 112590023

BASE_CFLAGS := -std=gnu11 -Wall -Wextra -Wshadow -Wformat=2 -fstack-protector-strong \
               -D_GNU_SOURCE -Iinclude
ifeq ($(MODE),debug)
  OUT         := build/debug
  MODE_CFLAGS := -O0 -g -DLOG_COMPILE_LEVEL=4 -DBENCH_FAULT_INJECTION
else
  OUT         := build
  MODE_CFLAGS := -O2 -DLOG_COMPILE_LEVEL=2
endif
CFLAGS  += $(BASE_CFLAGS) $(MODE_CFLAGS)
OBJDIR  := $(OUT)/obj
BINDIR  := $(OUT)/bin
LIBDIR  := $(OUT)/lib

LIB_SONAME := libbench.so.1
LIB_REAL   := $(LIBDIR)/$(LIB_SONAME)
LIB_LINK   := $(LIBDIR)/libbench.so

# Executables find libbench.so through a relative rpath: no LD_LIBRARY_PATH needed.
LDFLAGS_BIN := -L$(LIBDIR) -lbench -Wl,-rpath,'$$ORIGIN/../lib'

LIB_SRC    := $(wildcard lib/*.c)
SERVER_SRC := $(wildcard server/*.c)
CLIENT_SRC := $(wildcard client/*.c)

LIB_OBJ    := $(LIB_SRC:%.c=$(OBJDIR)/pic/%.o)
SERVER_OBJ := $(SERVER_SRC:%.c=$(OBJDIR)/%.o)
CLIENT_OBJ := $(CLIENT_SRC:%.c=$(OBJDIR)/%.o)

BINS := $(BINDIR)/benchd $(BINDIR)/bench $(BINDIR)/benchsim $(BINDIR)/bench-evil

.PHONY: all release debug build test stress capture clean dist

all: release

release:
	@$(MAKE) --no-print-directory MODE=release build

debug:
	@$(MAKE) --no-print-directory MODE=debug build

build: $(LIB_LINK) $(BINS)

$(OBJDIR)/pic/%.o: %.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(OBJDIR)/%.o: %.c $(wildcard include/*.h) $(wildcard server/*.h) $(wildcard client/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_REAL): $(LIB_OBJ)
	@mkdir -p $(LIBDIR)
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) -o $@ $^

$(LIB_LINK): $(LIB_REAL)
	ln -sf $(LIB_SONAME) $@

$(BINDIR)/benchd: $(SERVER_OBJ) $(LIB_LINK)
	@mkdir -p $(BINDIR)
	$(CC) -o $@ $(SERVER_OBJ) $(LDFLAGS_BIN) -lpthread

$(BINDIR)/bench: $(CLIENT_OBJ) $(LIB_LINK)
	@mkdir -p $(BINDIR)
	$(CC) -o $@ $(CLIENT_OBJ) $(LDFLAGS_BIN)

$(BINDIR)/benchsim: $(OBJDIR)/tools/benchsim.o $(LIB_LINK)
	@mkdir -p $(BINDIR)
	$(CC) -o $@ $(OBJDIR)/tools/benchsim.o $(LDFLAGS_BIN)

$(BINDIR)/bench-evil: $(OBJDIR)/tools/bench-evil.o $(LIB_LINK)
	@mkdir -p $(BINDIR)
	$(CC) -o $@ $(OBJDIR)/tools/bench-evil.o $(LDFLAGS_BIN)

test: release
	@scripts/test.sh

stress: release
	@scripts/stress.sh

capture: release
	@scripts/capture.sh

clean:
	rm -rf build out

dist:
	tar --exclude=./build --exclude='./captures/*.pcap' --exclude=./out --exclude='./$(STUDENT)_benchd.tar.gz' \
	    --transform 's,^\.,benchd,' -czf $(STUDENT)_benchd.tar.gz .
	@echo "wrote $(STUDENT)_benchd.tar.gz"
