ALTIVEC_ROOT ?= /altivec
include $(ALTIVEC_ROOT)/make/entry.mk
ifeq ($(_altivec_relocated),)
PROJECT_ROOT := $(abspath $(dir $(firstword $(MAKEFILE_LIST))))
ENIL_PROJECT_ROOT := $(PROJECT_ROOT)
PLATFORMS = macOS iOS
PROJECT_macOS_DIR = source/macOS
PROJECT_iOS_DIR = source/iOS
TEST_HOST_COMMAND = $(MAKE) test-host-suite
include $(ALTIVEC_ROOT)/make/projects.mk

# Cloudflare has npm scripts rather than native debug/release configurations.
cloudflare-%: FORCE
	@cd "$(ENIL_PROJECT_ROOT)/source/cloudflare" && npm run "$*"

# Offline, opt-in timing workload; not part of correctness tests.
.PHONY: benchmark-render
benchmark-render:
	@python3 -B "$(ENIL_PROJECT_ROOT)/source/tests/render_benchmark.py" $(BENCH_ARGS)

test: test-host
test-host-suite: test-build-system test-client-identity native-tests cloudflare-test test-ltsm

native-tests:
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_native*.py' -v
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_login_recovery.py' -v
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_windows_login_probe.py' -v

test-client-identity:
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_client_identity.py' -v

test-build-system:
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_build_system.py' -v

.PHONY: ltsm-prepare test-ltsm
ltsm-prepare:
	@$(MAKE) -C "$(ENIL_PROJECT_ROOT)/source/linux" validate BUILD_ROOT="$(BUILD_ROOT)"
	@$(MAKE) -C "$(ENIL_PROJECT_ROOT)/source/linux" "$(BUILD_ROOT)/ltsm-generated/ltsm.c" BUILD_ROOT="$(BUILD_ROOT)"

# Serialize extraction against the Worker suite, which rebuilds the same inputs.
LTSM_TEST_DIR = $(BUILD_ROOT)/linux/debug$(if $(BUILD_VARIANT),/variants/$(BUILD_VARIANT))
test-ltsm: cloudflare-test
	@$(MAKE) -C "$(ENIL_PROJECT_ROOT)/source/linux" ltsm-test ltsm-check CONFIG=debug BUILD_ROOT="$(BUILD_ROOT)"
	@"$(LTSM_TEST_DIR)/ltsm-check/ltsm-check"
	@node "$(ENIL_PROJECT_ROOT)/source/ltsm/test.mjs" "$(LTSM_TEST_DIR)/ltsm-test/ltsm-test"
endif
