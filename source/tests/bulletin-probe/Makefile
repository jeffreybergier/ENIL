ALTIVEC_ROOT ?= /altivec
include $(ALTIVEC_ROOT)/make/entry.mk
ifeq ($(_altivec_relocated),)
PROJECT_ROOT := $(abspath $(dir $(firstword $(MAKEFILE_LIST))))
PLATFORMS = iOS
PROJECT_iOS_DIR = source/iOS
TEST_HOST_COMMAND = node $(PROJECT_ROOT)/tools/test-host.cjs
TEST_NATIVE_COMMAND = node $(PROJECT_ROOT)/tools/test-native.cjs
include $(ALTIVEC_ROOT)/make/projects.mk
.PHONY: check-project
check-project:
	node tools/check-project.cjs
endif
