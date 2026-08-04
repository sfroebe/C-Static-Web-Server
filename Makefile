CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11
THREAD_FLAGS = -pthread
TARGET = server

.PHONY: all run clean

all: $(TARGET)

$(TARGET): server.c
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -o $(TARGET) server.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
