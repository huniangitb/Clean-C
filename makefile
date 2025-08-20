# Makefile for the cleaner project

# Compiler and flags
CC = clang
CFLAGS = -pthread -flto -O2

# Directories for build artifacts and final executables
BUILD_DIR = build
BIN_DIR = bin

# LDFLAGS for the main program
LDFLAGS_CLEANER = -lrt  /data/user/0/com.termux/files/usr/lib/libcJSON.a

# LDFLAGS for the client (不需要任何库)
LDFLAGS_CLIENT =

# Source files for the main program
SRCS_CLEANER = main.c logger.c config_manager.c stats_manager.c file_cleaner.c gc_trim.c udp_server.c

# Object files for the main program (now prefixed with BUILD_DIR)
# 使用 patsubst 将 .c 替换为 $(BUILD_DIR)/.o
OBJS_CLEANER = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS_CLEANER))

# Executable names (now prefixed with BIN_DIR)
TARGET_CLEANER = $(BIN_DIR)/cleaner
TARGET_CLIENT = $(BIN_DIR)/udp_client

# Default target
all: $(TARGET_CLEANER) $(TARGET_CLIENT)

# Rule to create the build directory
# 这是一个“顺序依赖” (order-only prerequisite)，确保目录在编译前存在
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Rule to create the bin directory
# 这是一个“顺序依赖”，确保目录在链接前存在
$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# Link the main program
# 依赖于所有的对象文件和 bin 目录
$(TARGET_CLEANER): $(OBJS_CLEANER) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS_CLEANER) $(LDFLAGS_CLEANER)
	@echo "Stripping $(notdir $@)..." # 打印正在 strip 的文件
	strip $@

# Link the client program
# 依赖于客户端源文件和 bin 目录
$(TARGET_CLIENT): udp_client.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ udp_client.c $(LDFLAGS_CLIENT)
	@echo "Stripping $(notdir $@)..." # 打印正在 strip 的文件
	strip $@

# Compile source files into object files (output to BUILD_DIR)
# 依赖于源文件和 build 目录
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean up build artifacts
clean:
	@echo "Cleaning up build and bin directories..."
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Phony targets
.PHONY: all clean $(BUILD_DIR) $(BIN_DIR)