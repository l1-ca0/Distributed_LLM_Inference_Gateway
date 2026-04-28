BUILD_DIR := build
CMAKE_BUILD_TYPE ?= Release
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: all build test clean deps

# Default target: install dependencies (if needed), build everything, run tests
all: build test

# Install build dependencies on Debian/Ubuntu systems if they are missing.
# A no-op when cmake, protoc, and grpc++ headers are already present (e.g.
# on a developer machine with brew or an autograder image with deps cached).
deps:
	@if ! (command -v cmake >/dev/null 2>&1 \
	       && command -v protoc >/dev/null 2>&1 \
	       && pkg-config --exists grpc++ 2>/dev/null); then \
		echo "Installing build dependencies (cmake, gRPC, Protobuf)..."; \
		(sudo -n apt-get update -qq 2>/dev/null \
		 && sudo -n apt-get install -y -qq \
		    cmake build-essential pkg-config \
		    libgrpc++-dev libprotobuf-dev \
		    protobuf-compiler protobuf-compiler-grpc) \
		|| (apt-get update -qq \
		    && apt-get install -y -qq \
		       cmake build-essential pkg-config \
		       libgrpc++-dev libprotobuf-dev \
		       protobuf-compiler protobuf-compiler-grpc); \
	fi

# Build all binaries. Build output is silenced on success; on failure
# the captured log is printed and make exits non-zero.
build: deps
	@mkdir -p $(BUILD_DIR)
	@USE_GRPC=$$(pkg-config --exists grpc++ 2>/dev/null && echo ON || echo OFF); \
	if [ ! -f $(BUILD_DIR)/CMakeCache.txt ]; then \
		cd $(BUILD_DIR) && cmake .. \
			-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
			-DUSE_SYSTEM_GRPC=$$USE_GRPC \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			> build.log 2>&1 || (cat build.log; exit 1); \
	fi
	@cd $(BUILD_DIR) && cmake --build . -j$(NPROC) \
		>> build.log 2>&1 || (cat build.log; exit 1)

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
