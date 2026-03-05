
CC = gcc
CFLAGS = -Wall -Werror -Wextra
TARGET = Simu
SOURCE = IRQLSimulation.c
OBJECTS = $(SOURCE:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c  $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
