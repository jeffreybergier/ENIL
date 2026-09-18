# ENIL: native builds and Linux-hosted checks.
.DEFAULT_GOAL := release
_enil_root := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
include $(_enil_root)/source/make/paths.mk

# Each platform keeps its own make context and compiler settings.
debug release analyze validate: %: macOS-% iOS-%

macOS-%: FORCE
	+@$(MAKE) --no-print-directory -C "$(ENIL_PROJECT_ROOT)/source/macOS" "$*" BUILD_ROOT="$(BUILD_ROOT)"

iOS-%: FORCE
	+@$(MAKE) --no-print-directory -C "$(ENIL_PROJECT_ROOT)/source/iOS" "$*" BUILD_ROOT="$(BUILD_ROOT)"

clean:
	@python3 "$(ENIL_PROJECT_ROOT)/source/make/clean-build.py" "$(ENIL_PROJECT_ROOT)" "$(BUILD_ROOT)"

# Cloudflare has npm scripts rather than native debug/release configurations.
cloudflare-%: FORCE
	@cd "$(ENIL_PROJECT_ROOT)/source/cloudflare" && npm run "$*"

test: test-host
test-host: test-build-system test-client-identity test-native cloudflare-test

test-native:
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_native*.py' -v
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_login_recovery.py' -v
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_windows_login_probe.py' -v

test-client-identity:
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_client_identity.py' -v

test-build-system:
	@python3 -B -m unittest discover -s "$(ENIL_PROJECT_ROOT)/source/tests" -p 'test_build_system.py' -v

help:
	@echo 'make [release|debug|analyze|validate]  Run both native targets (default: release)'
	@echo 'make macOS-<target> / iOS-<target>    Forward any child Makefile target'
	@echo 'make clean                           Remove native outputs for both platforms'
	@echo 'make macOS-clean / iOS-clean          Remove only one platform output tree'
	@echo 'make test-host                       Run all host protocol, recovery, build-system, and Worker tests'
	@echo 'make test-native                     Check Windows transport, login, and recovery on loopback'
	@echo 'make test-client-identity             Check session identity and loopback HTTP requests'
	@echo 'make cloudflare-<script>              Run a Worker npm script (e.g. test, build)'
	@echo 'BUILD_ROOT=/path                     Override the default repository-root build/'

FORCE:
.PHONY: debug release analyze validate clean test test-host test-native test-build-system test-client-identity help FORCE
