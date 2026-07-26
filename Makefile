CC = gcc
# -O2 enables ordinary compiler optimizations without changing the server's
# educational control flow. Keep warnings enabled for development.
CFLAGS = -Wall -Wextra -pedantic -std=c11 -O2
TARGET = server

.PHONY: all run clean

all: $(TARGET)

$(TARGET): server.c
	$(CC) $(CFLAGS) -o $(TARGET) server.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
