# List directories where modules are present
MODULES =  bus common hw device drivers mmio 

KERNEL_MODULES = . $(MODULES)

CC = gcc
CFLAGS = -Wall -Werror -Wextra -std=c11 $(addprefix -I, $(KERNEL_MODULES)) -MMD -MP
TARGET = testKrl
TEST_TARGET = test_pci_validation

# Sources for main kernel (excludes tests directory)
KERNEL_SOURCES = $(foreach dir,$(KERNEL_MODULES),$(wildcard $(dir)/*.c))

TEST_SOURCES = $(foreach dir,$(MODULES),$(wildcard $(dir)/*.c)) \
	$(wildcard tests/*.c)

# Remove the original main.c from test build (to avoid duplicate main)
TEST_SOURCES := $(filter-out main.c, $(TEST_SOURCES))

KERNEL_OBJECTS = $(KERNEL_SOURCES:.c=.o)
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(KERNEL_OBJECTS)
	$(CC) $^ -o $@
	@echo "Built $@ from $^"

# Test build - excludes original main.c
test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJECTS)
	$(CC) $^ -o $@
	@echo "Built test suite $@ from $^"

# Pattern rule for compiling
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "Compiled $< into $@"

info:
	@echo "Kernel Source files: $(KERNEL_SOURCES)"
	@echo "Kernel Object files: $(KERNEL_OBJECTS)"
	@echo "Test source files: $(TEST_SOURCES)"
	@echo "Test object files: $(TEST_OBJECTS)"
	@echo "Include paths: $(CFLAGS)"

# Build info
$(info Building kernel from: $(KERNEL_SOURCES))
$(info Building test from: $(TEST_SOURCES))

# Dependency files
-include $(KERNEL_OBJECTS:.o=.d)
-include $(TEST_OBJECTS:.o=.d)

# Clean everything
clean:
	rm -f $(KERNEL_OBJECTS) $(TARGET) $(KERNEL_OBJECTS:.o=.d)
	rm -f $(TEST_OBJECTS) $(TEST_TARGET) $(TEST_OBJECTS:.o=.d)	
	@echo "Cleaned all build artifacts"

# Clean only test artifacts
clean-test:
	rm -f $(TEST_OBJECTS) $(TEST_TARGET) $(TEST_OBJECTS:.o=.d)
	@echo "Cleaned test artifacts only"

.PHONY: all clean clean-test info test
