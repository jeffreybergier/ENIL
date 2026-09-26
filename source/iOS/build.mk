# Altivec Intelligence - ENIL (iPhone)
APP_NAME = ENIL
SOURCES = main.m AppDelegate.m ENILRootCoordinator.m ChatListViewController.m \
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
vpath %.c $(QRCODEGEN_DIR)
vpath %.c $(SHARED_DIR)
vpath %.m $(SHARED_DIR)
ALTIVECCORE_REQUIRED = 1
ALTIVECCOCOA_REQUIRED = 1
EXTRA_FLAGS = -I$(QRCODEGEN_DIR) -I$(SHARED_DIR)
# Portable C deps + the full shared engine (source/shared). These are the SAME
# files the macOS target compiles; they carry their own TARGET_OS_IPHONE guards
# (AIFontAwesome -> CoreText, XPFoundation -> SecItem keychain, enil_cocoa ->
# ImageIO/CoreFoundation). They stay MRC (EXTRA_SOURCES never gets -fobjc-arc).
EXTRA_SOURCES = qrcodegen.c \
  enil_http.c enil_worker.c enil_health.c enil_session.c enil_identity.c enil_b64.c \
  enil_line.c enil_chrome_gateway.c enil_qrlogin.c enil_native_login.c enil_login_store.c enil_native.c enil_native_poll.c enil_thrift.c enil_api_json.c enil_api_call.c enil_talkserv.c \
  enil_db.c enil_sync.c enil_obs.c enil_crypto.c enil_strbuf.c enil_html.c \
  enil_message_format.c enil_sse.c enil_account.c \
  enil_cocoa.m ENILDictionary.m ENILAccount.m ENILSyncStatusQueue.m \
  ENILKeychain.m ENILUserDefaults.m XPFoundation.m
PHONE_SOURCE_FLAGS = -fobjc-arc
PHONE_EXTRA_SOURCE_FLAGS = -Wno-pedantic -Wno-conversion \
  -Wno-sign-conversion -Wno-float-conversion
# Frameworks the shared engine needs on iOS: ImageIO (enil_cocoa transcode),
# Security (XPFoundation SecItem keychain), CoreFoundation (enil_cocoa
# date/collation). pthread/dl are in libSystem on iOS, so unlike the macOS
# target we do NOT add -lpthread/-ldl.
# QuartzCore is a UI need, not an engine one: SyncMiniBarView names CAGradientLayer
# / CABasicAnimation for the SSE-ping shimmer sweep (plain UIView.layer access —
# e.g. MessageSendViewController's cornerRadius — resolves through UIKit, but
# naming the CA* classes needs the framework).
# Local notifications use UILocalNotification (XPUserNotificationCenter in
# XPUIKit), so no UserNotifications.framework link is needed.
LIBS_IPHONE = -framework ImageIO -framework Security -framework CoreFoundation \
              -framework QuartzCore

# Resource files
RESOURCES = $(wildcard Resources/*.png)
BUNDLE_LOCALIZATION_DIRS = $(SHARED_DIR)/Resources
VALIDATE_PATHS = . $(SHARED_DIR)

# Import common Phone build engine
include /altivec/altivec_common_phone.mk

# ENILLog (shared XPFoundation.h) uses the GNU `, ##__VA_ARGS__` extension; the
# iOS engine is -pedantic (the macOS engine isn't), so only iOS flags it. The
# macro is shared and correct — silence this one diagnostic, keep -pedantic.
IOS_FLAGS += -Wno-gnu-zero-variadic-macro-arguments

# --- Code signing (jailbreak / ldid) ---
# The shared engine assembles ENIL.app and zips the IPA but never signs. On a
# jailbroken device we pseudo-sign with ldid and embed a private entitlement
# (com.apple.multitasking.unlimitedassertions) so the app may take an
# unexpiring background task assertion — keeping the LINE push/heartbeat loop
# alive when backgrounded. Same key Maps.app carries for turn-by-turn nav.
#
# Hooked WITHOUT touching the shared engine: sign the linked binary *before*
# the engine copies it into ENIL.app. The order-only prerequisite forces the
# .signed stamp first; re-signing bumps the binary mtime so the bundle (and
# IPA) repackage around the signed binary. The stamp keeps it incremental.
LDID         ?= ldid
ENTITLEMENTS ?= Entitlements.plist

$(APP_BUNDLE): | $(INT_DIR)/.signed

$(INT_DIR)/.signed: $(INT_DIR)/$(APP_NAME)-bin $(ENTITLEMENTS)
	@echo "  > ldid-signing $(APP_NAME) with $(ENTITLEMENTS)"
	@$(LDID) -S$(ENTITLEMENTS) $<
	@touch $@

# Keep the bundle timestamp current after copying files into an existing app.
# This lets the outer packaging rule recognize a rebuilt bundle.
EXTRA_BUNDLE_STEPS += @touch "$@"
