# Dynamic version calculation
YEAR := $(shell date +%Y)
MONTH := $(shell date +%m)
BASE_YEAR := 2025
MAJOR_VERSION := $(shell expr $$(date +%Y) - $(BASE_YEAR))
ifeq ($(shell test $(YEAR) -lt $(BASE_YEAR); echo $$?),0)
  MAJOR_VERSION := 1
endif

COMMIT_COUNT := $(shell git rev-list --count master 2>/dev/null || git rev-list --count HEAD 2>/dev/null || echo 0)
VERSION := $(MAJOR_VERSION).$(MONTH).$(COMMIT_COUNT)

BUILD_DIR ?= build

.PHONY: all version build clean test tag install

all: build

version:
	@./update_version.sh

$(BUILD_DIR)/Makefile: version CMakeLists.txt
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake ..

build: $(BUILD_DIR)/Makefile version
	@$(MAKE) -C $(BUILD_DIR)

test: build
	@cd $(BUILD_DIR) && ctest --output-on-failure

tag: version
	@echo "Creating git tag $(VERSION)..."
	@git tag -a $(VERSION) -m "Release $(VERSION)"
	@echo "Tag $(VERSION) created."

clean:
	@rm -rf $(BUILD_DIR)

install: build
	@$(MAKE) -C $(BUILD_DIR) install
