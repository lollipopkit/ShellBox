//
//  ViewController.h
//  ShellBox
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "Terminal.h"

@interface TerminalViewController : UIViewController

@property (nonatomic) Terminal *terminal;

- (void)startNewSession;
- (void)reconnectSessionFromTerminalUUID:(NSUUID *)uuid;
@property (readonly) NSUUID *sessionTerminalUUID; // 0 means invalid
@property UISceneSession *sceneSession API_AVAILABLE(ios(13.0));

- (void)accessoryInsertText:(NSString *)text;
- (void)accessoryToggleControl;
- (BOOL)accessoryControlActive;
- (void)accessoryPressArrow:(NSInteger)direction;
- (void)accessoryPaste;
- (void)accessoryHideKeyboard;
- (void)accessoryShowSettings;
- (BOOL)accessoryShouldShowUpdateBadge;

@end

extern struct tty_driver ios_tty_driver;
