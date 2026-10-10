PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ENIL_PROJECT_ROOT := $(PROJECT_ROOT)
ALTIVEC_ROOT ?= /altivec
PLATFORMS = macOS iOS
PROJECT_macOS_DIR = source/macOS
PROJECT_iOS_DIR = source/iOS
TEST_HOST_COMMAND = $(MAKE) test-host-suite
include $(ALTIVEC_ROOT)/make/projects.mk

# Cloudflare has npm scripts rather than native debug/release configurations.
cloudflare-%: FORCE
	@cd "$(ENIL_PROJECT_ROOT)/source/cloudflare" && npm run "$*"

test: test-host
test-host-suite: test-build-system test-client-identity native-tests cloudflare-test

native-tests:
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_native*.py' -v
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_login_recovery.py' -v
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_windows_login_probe.py' -v

test-client-identity:
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_client_identity.py' -v

test-build-system:
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_build_system.py' -v
