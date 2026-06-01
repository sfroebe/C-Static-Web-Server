CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11
TARGET = server

.PHONY: all run clean

all: $(TARGET)

$(TARGET): server.c
	$(CC) $(CFLAGS) -o $(TARGET) server.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
