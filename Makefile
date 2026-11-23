CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=

TARGET := status-client
SOURCES := status.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 $(SOURCES) $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
