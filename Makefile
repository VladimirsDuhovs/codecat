# Aggressive release build with LTO on GCC/Clang
APP := codecat
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

CC ?= gcc

CFLAGS_RELEASE := -O3 -march=native -flto -fno-plt -fno-semantic-interposition -pipe -DNDEBUG
LDFLAGS_RELEASE := -s -flto

CFLAGS_DEBUG := -O0 -g3 -DDEBUG

all: release

release: CFLAGS := $(CFLAGS_RELEASE)
release: LDFLAGS := $(LDFLAGS_RELEASE)
release: $(APP)

debug: CFLAGS := $(CFLAGS_DEBUG)
debug: LDFLAGS :=
debug: $(APP)

$(APP): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Wall -Wextra -Wpedantic -std=c11 -c $< -o $@

clean:
	rm -f $(OBJ) $(APP)

install: $(APP)
	install -d /usr/local/bin
	install -m 0755 $(APP) /usr/local/bin/$(APP)

reinstall: clean release install

uninstall:
	rm -f /usr/local/bin/$(APP)

.PHONY: all release debug clean install reinstall uninstall
