//
//  Terminal.h
//  ShellBox
//
//  Created by Theodore Dubois on 10/18/17.
//

#import <UIKit/UIKit.h>

struct tty;

@protocol TerminalRenderer <NSObject>
- (void)terminalDidReceiveOutput:(NSData * _Nonnull)data;
- (void)terminalScrollToBottom;
@end

@interface Terminal : NSObject

+ (Terminal *)terminalWithType:(int)type number:(int)number;
#if !ISH_LINUX
// Returns a strong struct tty and a Terminal that has a weak reference to the same tty
+ (Terminal *)createPseudoTerminal:(struct tty **)tty;
#endif

+ (Terminal *)terminalWithUUID:(NSUUID *)uuid;
@property (readonly) NSUUID *uuid;

+ (void)convertCommand:(NSArray<NSString *> *)command toArgs:(char *)argv limitSize:(size_t)maxSize;

- (int)sendOutput:(const void *)buf length:(int)len;
- (void)sendInput:(NSData *)input;
- (void)setWindowSizeWithColumns:(int)columns rows:(int)rows;

- (NSString *)arrow:(char)direction;

// Make this terminal no longer be the singleton terminal with its type and number. Will happen eventually if all references go away, but sometimes you want it to happen now.
- (void)destroy;

@property (nullable, nonatomic, weak) id<TerminalRenderer> renderer;
@property (nonatomic) BOOL enableVoiceOverAnnounce;
// Use KVO on this
@property (readonly) BOOL loaded;

@end

extern struct tty_driver ios_console_driver;
