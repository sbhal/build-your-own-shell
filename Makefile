CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
TARGET = mysh

all: $(TARGET)

$(TARGET): mysh_complete.c
	$(CC) $(CFLAGS) -o $(TARGET) mysh_complete.c

clean:
	rm -f $(TARGET)

test: $(TARGET)
	./validate ./$(TARGET)

test-stage: $(TARGET)
	./validate ./$(TARGET) $(STAGE)

.PHONY: all clean test test-stage
