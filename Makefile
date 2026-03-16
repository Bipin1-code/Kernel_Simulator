#Change source variable for to run different kernel lvl
# BlueKernel_Lvl1.c, BlueKernel_lvl2.c, BlueKernel_lvl3.c
# Each level is show you journey of kernel creation

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
