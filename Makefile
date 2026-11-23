CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=

TARGET := status-client
SOURCES := status.c
TEST_TARGET := tests/test_status

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 $(SOURCES) $(LDFLAGS) -o $@

$(TEST_TARGET): tests/test_status.c status.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 tests/test_status.c $(LDFLAGS) -Wl,--wrap=getaddrinfo -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET)
