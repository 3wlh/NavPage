# 编译器配置
CC := gcc
CXX := g++

# 根据命令行目标设置架构
ifeq ($(filter amd64,$(MAKECMDGOALS)),amd64)
	ARCH   := amd64
	TRIPLE := x86_64-linux-musl
endif
ifeq ($(filter arm64,$(MAKECMDGOALS)),arm64)
	ARCH   := arm64
	TRIPLE := aarch64-linux-musl
endif

# 交叉编译默认配置
ifeq ($(OS),Windows_NT)
	ARCH   ?= arm64
	TRIPLE ?= aarch64-linux-musl
endif
# Linux本机编译时取本机架构，避免产物名带空后缀
ARCH ?= $(shell $(CC) -dumpmachine | cut -d- -f1)

# 编译与链接选项
CFLAGS := -Os -Wall -Wextra 
# 静态链接
ifneq ($(TRIPLE),)
	LDFLAGS += -target $(TRIPLE)
endif
ifeq ($(OS),Windows_NT)
	LDFLAGS += -static
endif
# 链接库
LIBS := 

# 项目名称与目录结构
TARGET      := navpage
SRC_DIR     := src
BUILD_DIR   := Build
SOURCES := $(wildcard $(SRC_DIR)/*.c*) # 查找导入源码文件
HEADERS := $(wildcard $(SRC_DIR)/*.h) # 头文件改动也要触发重编译
EXECUTABLE := $(BUILD_DIR)/$(TARGET) # 输出执行文件

ASSETS := embedgen/assets.c embedgen/assets.S


ifeq ($(OS),Windows_NT)
EMBEDGEN = $(BUILD_DIR)/embedgen.exe
else
EMBEDGEN = $(BUILD_DIR)/embedgen
endif

all: $(EXECUTABLE)
# 当执行 make arm64 时，指定编译为 aarch64
arm64: all
# 当执行 make amd64 时，指定编译为 x86_64  
amd64: all
# 当执行 make linux 时，交叉编译双架构 
linux: 
	$(MAKE) all ARCH=amd64 TRIPLE=x86_64-linux-musl
	$(MAKE) all ARCH=arm64 TRIPLE=aarch64-linux-musl
			


$(EMBEDGEN): tools/embedgen.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $<

# 编译资源：web 下任意文件变动都会重新生成，避免嵌入的前端与源码不一致（长度对不上会导致 JS 被截断）
WEB_FILES := $(wildcard web/*.html web/*.js web/*.css) $(wildcard web/assets/*) $(wildcard web/vendor/*)
$(ASSETS): $(WEB_FILES) | $(EMBEDGEN)
	./$(EMBEDGEN) web embedgen/assets

# 编译源码
$(EXECUTABLE):$(SOURCES) $(HEADERS) | $(ASSETS) $(BUILD_DIR) 
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(SRC_DIR) -o $@_$(ARCH) $(SOURCES) $(ASSETS) $(LIBS)

# 创建构建目录
$(BUILD_DIR):
	- @mkdir -p $@ || mkdir $@
# 清理构建产物	
clean:
	- @$(RM) -r $(BUILD_DIR)/*_$(ARCH)
	
.PHONY: all clean linux arm64 amd64
