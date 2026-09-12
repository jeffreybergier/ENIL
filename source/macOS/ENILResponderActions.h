#import <Foundation/Foundation.h>

extern NSString * const ENILResponderPayloadChatIdKey;
extern NSString * const ENILResponderPayloadDisplayNameKey;
extern NSString * const ENILResponderPayloadTextKey;
extern NSString * const ENILResponderPayloadResourcesKey;
extern NSString * const ENILResponderPayloadPackageIdKey;
extern NSString * const ENILResponderPayloadStickerIdKey;
extern NSString * const ENILResponderPayloadIsSticonKey;
extern NSString * const ENILResponderPayloadImagePathKey;
extern NSString * const ENILResponderPayloadAltTextKey;
extern NSString * const ENILResponderPayloadFromFriendsKey;

@protocol ENILResponderActions <NSObject>

- (void)enilDismissSheet:(id)sender;
- (void)enilStartSync:(id)sender;
- (void)enilToggleSSE:(id)sender;
- (void)enilSelectChat:(id)sender;
- (void)enilDeleteChat:(id)sender;
- (void)enilMarkChatSeen:(id)sender;
- (void)enilLoadMoreMessages:(id)sender;
- (void)enilCloseChat:(id)sender;
- (void)enilLeaveChat:(id)sender;
- (void)enilSendCurrentMessage:(id)sender;
- (void)enilLogOut:(id)sender;
- (void)enilReauthenticate:(id)sender;
- (void)enilPickSticker:(id)sender;
- (void)enilSendMessage:(id)sender;
- (void)enilSendImage:(id)sender;
- (void)enilHideAttachments:(id)sender;
- (void)enilShowStickers:(id)sender;
- (void)enilShowEmoji:(id)sender;
- (void)enilAttachPhoto:(id)sender;

@end
