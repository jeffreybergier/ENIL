# Altivec Intelligence - ENIL (Mac)
APP_NAME = ENIL
SOURCES = main.m AppDelegate.m XPAppKit.m AboutWindowController.m \
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
vpath %.c $(QRCODEGEN_DIR)
vpath %.c $(SHARED_DIR)
vpath %.m $(SHARED_DIR)
IMAGEIO_FPATH = $(SDK_MAC_OLD_PATH)/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks
ALTIVECCORE_REQUIRED = 1
ALTIVECCORE_LINKAGE = dynamic
ALTIVECCOCOA_REQUIRED = 1
ALTIVECCOCOA_LINKAGE = dynamic
EXTRA_FLAGS = -I$(ALTIVECCORE_DIR)/include -I$(ALTIVECCOCOA_DIR)/include \
              -I$(QRCODEGEN_DIR) -I$(SHARED_DIR)
EXTRA_SOURCES = qrcodegen.c enil_http.c enil_worker.c enil_health.c \
                enil_session.c enil_identity.c enil_b64.c enil_line.c enil_qrlogin.c enil_native_login.c enil_login_store.c enil_native.c enil_thrift.c \
                enil_api_json.c enil_api_call.c enil_talkserv.c enil_db.c \
                enil_sync.c enil_obs.c enil_crypto.c enil_cocoa.m \
                enil_strbuf.c enil_html.c enil_message_format.c \
                enil_sse.c enil_account.c ENILDictionary.m ENILAccount.m \
                ENILSyncStatusQueue.m ENILKeychain.m ENILUserDefaults.m \
                XPFoundation.m
EXTRA_LIBS = -lpthread -ldl -framework CoreServices -framework Security \
             -framework ApplicationServices
MAC_ICON = ENIL.icns
BUNDLE_FONT_DIRS = $(ALTIVECCOCOA_DIR)/Resources/Fonts
BUNDLE_LOCALIZATION_DIRS = $(SHARED_DIR)/Resources

# Import common Mac build engine
include /altivec/altivec_common_mac.mk

# Override/Append
MAC_FLAGS += $(EXTRA_FLAGS)
MAC_LIBS += $(EXTRA_LIBS)
LEGACY_GCC_FLAGS += -F$(IMAGEIO_FPATH)

# Project directories `validate` must confirm exist before a build runs.
# Only our own code — deps manage their own presence checks.
VALIDATE_PATHS = . $(SHARED_DIR)

# Keep the bundle timestamp current after copying files into an existing app.
# This lets the outer packaging rule recognize a rebuilt bundle.
EXTRA_BUNDLE_STEPS += @touch "$@"
