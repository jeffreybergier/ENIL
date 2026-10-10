GROUP_ui_SOURCES = main.m AppDelegate.m ENILRootCoordinator.m ChatListViewController.m \
          SyncMiniBarView.m \
          MessageListViewController.m MessageSendViewController.m \
          StickerViewController.m MediaViewerViewController.m \
          PreferencesViewController.m \
          QRLoginViewController.m ENILQRImage.m UIViewController+ENILModal.m \
          XPUIKit.m
INFO_PLIST = Info.plist

QRCODEGEN_DIR = ../deps/qrcodegen/c
SHARED_DIR    = ../shared
SOURCE_DIRS   = $(QRCODEGEN_DIR) $(SHARED_DIR)
EXTRA_FLAGS = -I$(QRCODEGEN_DIR) -I$(SHARED_DIR)
GROUP_shared_SOURCES = qrcodegen.c \
  enil_http.c enil_png.c enil_asset_cache.c enil_worker.c enil_health.c enil_session.c enil_identity.c enil_b64.c \
  enil_line.c enil_chrome_gateway.c enil_qrlogin.c enil_native_login.c enil_login_store.c enil_native.c enil_native_poll.c enil_thrift.c enil_api_json.c enil_api_call.c enil_talkserv.c \
  enil_db.c enil_sync.c enil_obs.c enil_crypto.c enil_strbuf.c enil_html.c \
  enil_message_format.c enil_sse.c enil_account.c \
  enil_cocoa.m ENILDictionary.m ENILAccount.m ENILSyncStatusQueue.m \
  ENILKeychain.m ENILUserDefaults.m XPFoundation.m
PHONE_EXTRA_SOURCE_FLAGS = -Wno-pedantic -Wno-conversion \
  -Wno-sign-conversion -Wno-float-conversion
LIBS_IPHONE = -framework ImageIO -framework Security -framework CoreFoundation \
              -framework QuartzCore

BUNDLE_LOCALIZATION_DIRS = $(SHARED_DIR)/Resources

PROJECT_ROOT := $(abspath ../..)
PRODUCTS = ENIL
ENIL_TYPE = app
ENIL_GROUPS = ui shared
ENIL_PLIST = $(INFO_PLIST)
ENIL_RESOURCES = $(filter-out Resources/Info.plist,$(wildcard Resources/*))
ENIL_LOCALIZATIONS = $(BUNDLE_LOCALIZATION_DIRS)
ENIL_FLAGS = $(EXTRA_FLAGS)
ENIL_CORE = static
ENIL_COCOA = static
GROUP_ui_MEMORY = arc
GROUP_shared_FLAGS = $(PHONE_EXTRA_SOURCE_FLAGS)
ENIL_FLAGS += -Wno-gnu-zero-variadic-macro-arguments
ENIL_LDFLAGS = -framework UIKit -framework Foundation -framework CoreGraphics $(LIBS_IPHONE)
ENIL_ENTITLEMENTS = Entitlements.plist
ENIL_SANDBOX = container
ALTIVEC_ROOT ?= /altivec
include $(ALTIVEC_ROOT)/altivec_common_phone.mk
