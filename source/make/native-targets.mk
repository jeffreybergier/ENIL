# Public targets shared by source/macOS/Makefile and source/iOS/Makefile.
.DEFAULT_GOAL := release
include $(dir $(lastword $(MAKEFILE_LIST)))paths.mk
CONFIG ?= release
ifneq ($(words $(CONFIG)),1)
$(error CONFIG must select exactly one configuration)
endif
ifeq ($(filter $(CONFIG),debug release),)
$(error Unsupported CONFIG "$(CONFIG)"; expected debug or release)
endif

ENIL_BUILD_DIR := $(BUILD_ROOT)/$(ENIL_PLATFORM)/$(CONFIG)
ENIL_ANALYZE_DIR := $(BUILD_ROOT)/$(ENIL_PLATFORM)/analyze
ENIL_BUNDLE := $(ENIL_BUILD_DIR)/ENIL.app
ENIL_ARCHIVE := $(ENIL_BUILD_DIR)/ENIL.$(if $(filter macOS,$(ENIL_PLATFORM)),zip,ipa)
ENIL_ENGINE_TARGET := $(if $(filter macOS,$(ENIL_PLATFORM)),$(ENIL_ARCHIVE),$(ENIL_BUNDLE))
ENIL_ENGINE_ARGS = BUILD_DIR="$(ENIL_BUILD_DIR)" OPT_FLAGS="$(if $(filter debug,$(CONFIG)),-O0,-O3)" ANALYZE_OUTPUT_DIR="$(ENIL_ANALYZE_DIR)"

# Select the configuration in a fresh make context; do not call Altivec's
# debug/release recipes, which hardcode source-local output directories.
debug release:
	+@$(MAKE) --no-print-directory _enil-build CONFIG="$@" BUILD_ROOT="$(BUILD_ROOT)"

_enil-build:
	+@$(MAKE) --no-print-directory -f build.mk validate $(ENIL_ENGINE_ARGS)
	+@$(MAKE) --no-print-directory -f build.mk "$(ENIL_ENGINE_TARGET)" $(ENIL_ENGINE_ARGS)
ifeq ($(ENIL_PLATFORM),iOS)
	+@$(MAKE) --no-print-directory _enil-package CONFIG="$(CONFIG)" BUILD_ROOT="$(BUILD_ROOT)"
endif

# The installed IPA recipe assumes a source-local build directory. Keep its
# compilation/bundling rules, but package to an absolute destination here.
_enil-package: $(ENIL_ARCHIVE)
ifeq ($(ENIL_PLATFORM),iOS)
# The preceding engine invocation already built this prerequisite.
$(ENIL_BUNDLE): ;
$(ENIL_ARCHIVE): $(ENIL_BUNDLE)
	@set -eu; \
	stage="$(ENIL_BUILD_DIR)/Intermediates/Payload"; \
	temporary="$(ENIL_ARCHIVE).tmp.zip"; \
	trap 'rm -rf "$$stage"; rm -f "$$temporary"' EXIT; \
	rm -rf "$$stage"; rm -f "$$temporary"; \
	mkdir -p "$$stage"; \
	cp -RP "$(ENIL_BUNDLE)" "$$stage/"; \
	echo 'Packaging $(ENIL_ARCHIVE)'; \
	cd "$(ENIL_BUILD_DIR)/Intermediates"; \
	zip -rqy "$$temporary" Payload; \
	mv "$$temporary" "$(ENIL_ARCHIVE)"
endif

clean:
	@python3 "$(ENIL_PROJECT_ROOT)/source/make/clean-build.py" "$(ENIL_PROJECT_ROOT)" "$(BUILD_ROOT)" "$(ENIL_PLATFORM)"

help:
	@echo '$(ENIL_PLATFORM): debug, release, analyze, validate, clean; other goals forward to build.mk'
	@echo 'Outputs: $(BUILD_ROOT)/$(ENIL_PLATFORM)/{debug,release}/Intermediates'

# Do not try to remake existing Makefiles via the catch-all rule.
$(MAKEFILE_LIST): ;
%: FORCE
	+@$(MAKE) --no-print-directory -f build.mk "$@" $(ENIL_ENGINE_ARGS)

FORCE:
.PHONY: debug release clean help _enil-build _enil-package FORCE
