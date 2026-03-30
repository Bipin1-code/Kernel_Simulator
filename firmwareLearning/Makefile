
CC = gcc
CFLAGS =  -Wall -Werror -Wextra -pedantic
TARGET = pci_out
SOURCE = PCI_foundation.c
OBJECTS = $(SOURCE:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@
	@echo -e "\x1b[32mBuilt $@ from $^\x1b[0m"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@ 
	@echo -e "\x1b[32mCompiled $< into $@\x1b[0m"	

clean:
	rm -f $(OBJECTS) $(TARGET)
	@echo -e "\x1b[32mRemoved $? $(TARGET)\x1b[0m"
