# 编译器配置
GO := GO
# 项目名称与目录
TARGET := navpage
SRC_DIR := src
BUILD_DIR := Build
# 根据命令行目标设置架构
SOURCES := $(wildcard $(SRC_DIR)/*.go) # 查找源码
EXECUTABLE := $(BUILD_DIR)/$(TARGET)

all: build build-linux build-linux-arm64
	
	
build:
	$(GO) build -ldflags "-s -w" -o $(EXECUTABLE).exe $(SOURCES) && \
	upx -9 $(EXECUTABLE).exe

build-linux:
	CGO_ENABLED=0 GOOS=linux GOARCH=amd64 \
	$(GO) build -ldflags "-s -w" -o $(EXECUTABLE)-linux-amd64 $(SOURCES) && \
	upx -9 $(EXECUTABLE)-linux-amd64
	CGO_ENABLED=0 GOOS=linux GOARCH=arm64 \
	$(GO) build -ldflags "-s -w" -o $(EXECUTABLE)-linux-arm64 $(SOURCES) && \
	upx -9 $(EXECUTABLE)-linux-arm64

run:
	go run $(SOURCES)

clean:
	$(RM) -fr $(BUILD_DIR)

.PHONY: build build-linux build-linux-arm64 run clean