ALTIVEC_ROOT ?= /altivec
include $(ALTIVEC_ROOT)/make/entry.mk
ifeq ($(_altivec_relocated),)
PROJECT_ROOT := $(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../..)
PRODUCTS = ENILBulletinProbe model
PACKAGE_PRODUCTS = ENILBulletinProbe
model_TYPE = static
model_SOURCES = ../shared/model.c
ENILBulletinProbe_TYPE = app
ENILBulletinProbe_DEPS = model
ENILBulletinProbe_PLIST = Info.plist
ENILBulletinProbe_RESOURCES = $(wildcard Resources/*) ../../LICENSE
ENILBulletinProbe_LOCALIZATIONS = ../shared/Resources
ENILBulletinProbe_SOURCES = ../shared/XPFoundation.m
ENILBulletinProbe_FLAGS = -I../shared -I../service
ENILBulletinProbe_GROUPS = ui
GROUP_ui_SOURCES = main.m AppDelegate.m UI/MainViewController.m
GROUP_ui_MEMORY = arc
ENILBulletinProbe_LDFLAGS = -framework UIKit -framework Foundation -framework CoreGraphics
ENILBulletinProbe_ENTITLEMENTS = AppEntitlements.plist
ENILBulletinProbe_SANDBOX = container
include $(ALTIVEC_ROOT)/altivec_common_phone.mk
endif
