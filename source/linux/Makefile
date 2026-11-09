ALTIVEC_ROOT ?= /altivec
include $(ALTIVEC_ROOT)/make/entry.mk
ifeq ($(_altivec_relocated),)
PROJECT_ROOT := $(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../..)
LTSM_HOST = 1
include ../ltsm/products.mk
include $(ALTIVEC_ROOT)/altivec_common_host.mk
endif
