# Resolve paths from this file, regardless of the caller's working directory.
ENIL_PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
BUILD_ROOT ?= $(ENIL_PROJECT_ROOT)/build
ifeq ($(strip $(BUILD_ROOT)),)
$(error BUILD_ROOT must not be empty)
endif
# Relative overrides are relative to the repository, including child invocations.
override BUILD_ROOT := $(abspath $(if $(filter /%,$(BUILD_ROOT)),$(BUILD_ROOT),$(ENIL_PROJECT_ROOT)/$(BUILD_ROOT)))
ifneq ($(words $(BUILD_ROOT)),1)
$(error The Altivec native rules do not support whitespace in BUILD_ROOT)
endif
