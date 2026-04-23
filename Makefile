BUILD_DIR := build
CMAKE_BUILD_TYPE ?= Release
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Use an installed gRPC if available; otherwise build it via FetchContent.
USE_SYSTEM_GRPC ?= $(shell pkg-config --exists grpc++ 2>/dev/null && echo ON || echo OFF)

.PHONY: all build test clean

# Default target: build everything and run tests
all: build test

# Build all binaries
build:
	@mkdir -p $(BUILD_DIR)
	@if [ ! -f $(BUILD_DIR)/CMakeCache.txt ]; then \
		cd $(BUILD_DIR) && cmake .. \
			-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
			-DUSE_SYSTEM_GRPC=$(USE_SYSTEM_GRPC) \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5; \
	fi
	@cd $(BUILD_DIR) && cmake --build . -j$(NPROC)

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
