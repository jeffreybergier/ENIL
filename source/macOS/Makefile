ALTIVEC_ROOT ?= /altivec
include $(ALTIVEC_ROOT)/make/entry.mk
ifeq ($(_altivec_relocated),)
PROJECT_ROOT := $(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../..)
GROUP_ui_SOURCES = main.m AppDelegate.m XPAppKit.m AboutWindowController.m \
          PreferencesWindowController.m ChatWindowController.m \
          ChatListViewController.m MessageListViewController.m \
          MessageSendViewController.m StickerViewController.m ENILQRImage.m \
          QRLoginWindowController.m StatusViewController.m \
          SyncMiniViewController.m ENILBottomToolbarView.m \
          ENILWindowAwareView.m
INFO_PLIST = Info.plist

QRCODEGEN_DIR = ../deps/qrcodegen/c
SHARED_DIR = ../shared
SOURCE_DIRS = $(QRCODEGEN_DIR) $(SHARED_DIR)
EXTRA_FLAGS = -I$(ALTIVECCORE_DIR)/include -I$(ALTIVECCOCOA_DIR)/include \
              -I$(QRCODEGEN_DIR) -I$(SHARED_DIR)
GROUP_shared_SOURCES = qrcodegen.c enil_http.c enil_jpeg.c enil_png.c enil_asset_cache.c enil_worker.c enil_health.c \
                enil_session.c enil_identity.c enil_b64.c enil_line.c enil_chrome_gateway.c enil_qrlogin.c enil_native_login.c enil_login_store.c enil_native.c enil_native_poll.c enil_thrift.c \
                enil_api_json.c enil_api_call.c enil_talkserv.c enil_db.c \
                enil_sync.c enil_obs.c enil_crypto.c enil_cocoa.m \
                enil_strbuf.c enil_html.c enil_message_format.c \
                enil_sse.c enil_account.c ENILDictionary.m ENILAccount.m \
                ENILSyncStatusQueue.m ENILKeychain.m ENILUserDefaults.m \
                XPFoundation.m
EXTRA_LIBS = -lpthread -ldl -framework CoreServices -framework Security \
             -framework ApplicationServices
MAC_ICON = ENIL.icns
BUNDLE_LOCALIZATION_DIRS = $(SHARED_DIR)/Resources

PRODUCTS = ENIL
ENIL_TYPE = app
ENIL_GROUPS = ui shared
ENIL_PLIST = $(INFO_PLIST)
ENIL_RESOURCES = $(filter-out Resources/Info.plist,$(wildcard Resources/*))
ENIL_LOCALIZATIONS = $(BUNDLE_LOCALIZATION_DIRS)
ENIL_FLAGS = $(EXTRA_FLAGS)
ENIL_CORE = dynamic
ENIL_COCOA = dynamic
# ATSApplicationFontsPath and the HTML renderer both use the app's Fonts/.
# Dynamic Cocoa linkage only embeds the framework's own copy.
ENIL_FONTS = $(wildcard $(ALTIVECCOCOA_DIR)/Resources/Fonts/*)
ENIL_RESOURCES += $(MAC_ICON)
ENIL_LDFLAGS = -framework AppKit -lobjc $(EXTRA_LIBS)
ENIL_FLAGS_ppc = -F$(SDK_ppc)/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks
ENIL_FLAGS_i386 = -F$(SDK_i386)/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks
include $(ALTIVEC_ROOT)/altivec_common_mac.mk
endif
