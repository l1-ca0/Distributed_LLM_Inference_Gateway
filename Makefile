BUILD_DIR := build
CMAKE_BUILD_TYPE ?= Release
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Portable cmake binary bootstrapped into build/ when the host has no cmake.
CMAKE_VERSION := 3.27.9
PORTABLE_CMAKE_DIR := $(BUILD_DIR)/cmake-$(CMAKE_VERSION)-linux-x86_64
PORTABLE_CMAKE_URL := https://github.com/Kitware/CMake/releases/download/v$(CMAKE_VERSION)/cmake-$(CMAKE_VERSION)-linux-x86_64.tar.gz

.PHONY: all build test clean deps

# Default target: bootstrap dependencies (if needed), build everything, run tests
all: build test

# Bootstrap cmake on systems that don't already have it, by downloading
# Kitware's portable Linux x86_64 binary into build/. A no-op on a
# developer machine with cmake in PATH. All install output is captured
# to build/install.log; on failure the captured log is printed.
deps:
	@mkdir -p $(BUILD_DIR)
	@LOG=$(BUILD_DIR)/install.log; \
	if ! command -v cmake >/dev/null 2>&1 \
	   && [ ! -x $(PORTABLE_CMAKE_DIR)/bin/cmake ]; then \
		( if command -v curl >/dev/null 2>&1; then \
			curl -fsSL $(PORTABLE_CMAKE_URL) | tar xz -C $(BUILD_DIR); \
		  elif command -v wget >/dev/null 2>&1; then \
			wget -qO- $(PORTABLE_CMAKE_URL) | tar xz -C $(BUILD_DIR); \
		  else \
			echo "Error: need curl or wget to bootstrap cmake" >&2; exit 1; \
		  fi ) > $$LOG 2>&1 || (cat $$LOG; exit 1); \
	fi

# Build all binaries. gRPC and Protobuf are pulled in via CMake FetchContent
# so no system-level package install is needed. Build output is silenced
# on success; on failure the captured log is printed and make exits non-zero.
build: deps
	@mkdir -p $(BUILD_DIR)
	@PATH="$(PORTABLE_CMAKE_DIR)/bin:$$PATH"; export PATH; \
	USE_GRPC=$$(pkg-config --exists grpc++ 2>/dev/null && echo ON || echo OFF); \
	LOG=$(BUILD_DIR)/build.log; \
	if [ ! -f $(BUILD_DIR)/CMakeCache.txt ]; then \
		cmake -S . -B $(BUILD_DIR) \
			-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
			-DUSE_SYSTEM_GRPC=$$USE_GRPC \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			> $$LOG 2>&1 || (cat $$LOG; exit 1); \
	fi; \
	cmake --build $(BUILD_DIR) -j$(NPROC) \
		>> $$LOG 2>&1 || (cat $$LOG; exit 1)

# Run the test suite
test: build
	@echo ""
	@echo "===================================================================="
	@echo " Graded integration tests (9 tests, 200 pts)"
	@echo "===================================================================="
	@$(BUILD_DIR)/test_driver
	@echo ""
	@echo "===================================================================="
	@echo " Supporting: SWIM unit tests"
	@echo "===================================================================="
	@$(BUILD_DIR)/test_gossip_unit
	@echo ""
	@echo "===================================================================="
	@echo " Supporting: SWIM gossip integration tests"
	@echo "===================================================================="
	@$(BUILD_DIR)/test_swim_integration
	@echo ""
	@echo "===================================================================="
	@echo " Supporting: TestCluster harness smoke tests"
	@echo "===================================================================="
	@$(BUILD_DIR)/test_cluster_smoke

# Clean build artifacts
clean:
	@rm -rf $(BUILD_DIR)
