BUILD_DIR := build
CMAKE_BUILD_TYPE ?= Release
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Set USE_SYSTEM_GRPC=ON if you have gRPC installed (faster builds).
# Default (OFF): builds gRPC from source via FetchContent — no system deps needed.
USE_SYSTEM_GRPC ?= OFF

.PHONY: all build test clean

# Default target: build everything and run tests
all: build test

# Build all binaries
build:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake .. \
		-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
		-DUSE_SYSTEM_GRPC=$(USE_SYSTEM_GRPC)
	@cd $(BUILD_DIR) && cmake --build . -j$(NPROC)
	@echo "Build complete."

# Run the test suite
test: build
	@echo "Running tests..."
	@# TODO: replace with test_driver once implemented
	@$(BUILD_DIR)/replica
	@$(BUILD_DIR)/gateway
	@$(BUILD_DIR)/client
	@echo "All stubs ran successfully."

# Clean build artifacts
clean:
	@rm -rf $(BUILD_DIR)
	@echo "Clean complete."
