
# List directories where modules are present
MODULES = . bus common hw device drivers

CC = gcc
CFLAGS = -Wall -Werror -Wextra -std=c11 $(addprefix -I, $(MODULES)) -MMD -MP
TARGET = testKrl

SOURCES += $(foreach dir,$(MODULES),$(wildcard $(dir)/*.c))

OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $^ -o $@
	@echo "Built $@ form $^"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "Compiled $< into $@"

info:
	@echo "Source files: $(SOURCES)"
	@echo "Object files: $(OBJECTS)"
	@echo "Include paths: $(CFLAGS)"

-include $(OBJECTS:.o=.d)

# Tracker or logger while build and compilation
$(info Building from: $(SOURCES))

# Clean ups
clean:
	rm -f $(OBJECTS) $(TARGET) $(OBJECTS:.o=.d)
	@echo "Removed $? $(TARGET)"


.PHONY: all clean info
