//
//  ViewController.m
//  Shell Box
//
//  Created by Theodore Dubois on 10/17/17.
//

#import "TerminalViewController.h"
#import "AppDelegate.h"
#import "TerminalView.h"
#import "UserPreferences.h"
#import "CurrentRoot.h"
#import "NSObject+SaneKVO.h"
#import "LinuxInterop.h"
#include <objc/message.h>
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "fs/devices.h"

@interface TerminalViewController () <UIGestureRecognizerDelegate>

@property UITapGestureRecognizer *tapRecognizer;
@property (weak, nonatomic) TerminalView *termView;
@property (weak, nonatomic) NSLayoutConstraint *bottomConstraint;

@property (strong, nonatomic) UIInputView *barView;

@property int sessionPid;
@property (nonatomic) Terminal *sessionTerminal;

@property BOOL ignoreKeyboardMotion;
@property (nonatomic) BOOL hasExternalKeyboard;
@property (nonatomic) BOOL processNotificationsRegistered;

@end

@implementation TerminalViewController

static UIInputView *SwiftUIAccessoryBarView(TerminalViewController *terminalViewController) {
    NSArray<NSString *> *classNames = @[
        @"Shell_Box.ShellBoxAccessoryBarFactory",
        @"ShellBox.ShellBoxAccessoryBarFactory",
        @"ShellBoxAccessoryBarFactory",
    ];
    Class factoryClass = Nil;
    for (NSString *className in classNames) {
        factoryClass = NSClassFromString(className);
        if (factoryClass != Nil)
            break;
    }
    SEL selector = NSSelectorFromString(@"inputViewWithController:");
    if (factoryClass == Nil || ![factoryClass respondsToSelector:selector])
        return nil;

    return ((UIInputView *(*)(id, SEL, TerminalViewController *))objc_msgSend)(factoryClass, selector, terminalViewController);
}

static UIViewController *SwiftUISettingsController(BOOL recoveryMode) {
    NSArray<NSString *> *classNames = @[
        @"Shell_Box.ShellBoxSettingsHostingController",
        @"ShellBox.ShellBoxSettingsHostingController",
        @"ShellBoxSettingsHostingController",
    ];
    Class hostingClass = Nil;
    for (NSString *className in classNames) {
        hostingClass = NSClassFromString(className);
        if (hostingClass != Nil)
            break;
    }
    SEL selector = NSSelectorFromString(@"controllerWithRecoveryMode:");
    if (hostingClass == Nil || ![hostingClass respondsToSelector:selector])
        return nil;

    return ((UIViewController *(*)(id, SEL, BOOL))objc_msgSend)(hostingClass, selector, recoveryMode);
}

- (void)loadView {
    UIView *view = [UIView new];
    view.backgroundColor = UIColor.systemBackgroundColor;
    self.view = view;

    TerminalView *termView = [TerminalView new];
    termView.translatesAutoresizingMaskIntoConstraints = NO;
    termView.canBecomeFirstResponder = YES;
    [view addSubview:termView];
    self.termView = termView;

    UILayoutGuide *safeArea = view.safeAreaLayoutGuide;
    self.bottomConstraint = [view.bottomAnchor constraintEqualToAnchor:termView.bottomAnchor];
    [NSLayoutConstraint activateConstraints:@[
        [termView.topAnchor constraintEqualToAnchor:safeArea.topAnchor],
        [termView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor],
        [termView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor],
        self.bottomConstraint,
    ]];

    self.barView = SwiftUIAccessoryBarView(self);
    termView.inputAccessoryView = self.barView;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self registerProcessNotifications];

#if !ISH_LINUX
    int bootError = [AppDelegate bootError];
    if (bootError < 0) {
        NSString *message = [NSString stringWithFormat:@"could not boot"];
        NSString *subtitle = [NSString stringWithFormat:@"error code %d", bootError];
        if (bootError == _EINVAL)
            subtitle = [subtitle stringByAppendingString:@"\n(try reinstalling the app, see release notes for details)"];
        [self showMessage:message subtitle:subtitle];
        NSLog(@"boot failed with code %d", bootError);
    }
#endif

    self.terminal = self.terminal;
    [self.termView becomeFirstResponder];

    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardWillChangeFrameNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardDidChangeFrameNotification
                 object:nil];
    [self _updateStyleFromPreferences:NO];
    
    [UserPreferences.shared observe:@[@"hideStatusBar"] options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setNeedsStatusBarAppearanceUpdate];
        });
    }];
    [UserPreferences.shared observe:@[@"colorScheme", @"theme", @"hideExtraKeysWithExternalKeyboard"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateStyleFromPreferences:YES];
        });
    }];
}

- (void)registerProcessNotifications {
    if (self.processNotificationsRegistered)
        return;
    self.processNotificationsRegistered = YES;
#if !ISH_LINUX
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(processExited:)
                                               name:ProcessExitedNotification
                                             object:nil];
#else
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(kernelPanicked:)
                                               name:KernelPanicNotification
                                             object:nil];
#endif
}

- (void)viewDidAppear:(BOOL)animated {
    [AppDelegate maybePresentStartupMessageOnViewController:self];
    [super viewDidAppear:animated];
}

- (void)startNewSession {
    int err = [self startSession];
    if (err < 0) {
        [self showMessage:@"could not start session"
                 subtitle:[NSString stringWithFormat:@"error code %d", err]];
    }
}

- (void)reconnectSessionFromTerminalUUID:(NSUUID *)uuid {
    self.sessionTerminal = [Terminal terminalWithUUID:uuid];
    if (self.sessionTerminal == nil)
        [self startNewSession];
}

- (NSUUID *)sessionTerminalUUID {
    return self.terminal.uuid;
}

- (int)startSession {
    NSArray<NSString *> *command = UserPreferences.shared.launchCommand;

#if !ISH_LINUX
    int err = become_new_init_child();
    if (err < 0)
        return err;
    struct tty *tty;
    self.sessionTerminal = nil;
    Terminal *terminal = [Terminal createPseudoTerminal:&tty];
    if (terminal == nil) {
        NSAssert(IS_ERR(tty), @"tty should be error");
        return (int) PTR_ERR(tty);
    }
    self.sessionTerminal = terminal;
    NSString *stdioFile = [NSString stringWithFormat:@"/dev/pts/%d", tty->num];
    err = create_stdio(stdioFile.fileSystemRepresentation, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    if (err < 0)
        return err;
    tty_release(tty);

    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "TERM=xterm-256color\0";
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0)
        return err;
    self.sessionPid = current->pid;
    task_start(current);
#else
    const char *argv_arr[command.count + 1];
    for (NSUInteger i = 0; i < command.count; i++)
        argv_arr[i] = command[i].UTF8String;
    argv_arr[command.count] = NULL;
    const char *envp_arr[] = {
        "TERM=xterm-256color",
        NULL,
    };
    const char *const *argv = argv_arr;
    const char *const *envp = envp_arr;
    __block Terminal *terminal = nil;
    __block int sessionPid = 0;
    __block int err = 1;
    sync_do_in_workqueue(^(void (^done)(void)) {
        linux_start_session(argv[0], argv, envp, ^(int retval, int pid, nsobj_t term) {
            err = retval;
            if (term)
                terminal = CFBridgingRelease(term);
            sessionPid = pid;
            done();
        });
    });
    NSAssert(err <= 0, @"session start did not finish??");
    if (err < 0)
        return err;
    self.sessionTerminal = terminal;
    self.sessionPid = sessionPid;
#endif
    return 0;
}

#if !ISH_LINUX
- (void)processExited:(NSNotification *)notif {
    int pid = [notif.userInfo[@"pid"] intValue];
    if (pid != self.sessionPid)
        return;

    [self.sessionTerminal destroy];
    // On iOS 13, there are multiple windows, so just close this one.
    if (@available(iOS 13, *)) {
        // On iPhone, destroying scenes will fail, but the error doesn't actually go to the error handler, which is really stupid. Apple doesn't fix bugs, so I'm forced to just add a check here.
        if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad && self.sceneSession != nil) {
            [UIApplication.sharedApplication requestSceneSessionDestruction:self.sceneSession options:nil errorHandler:^(NSError *error) {
                NSLog(@"scene destruction error %@", error);
                self.sceneSession = nil;
                [self processExited:notif];
            }];
            return;
        }
    }
    current = NULL; // it's been freed
    [self startNewSession];
}
#endif

#if ISH_LINUX
- (void)kernelPanicked:(NSNotification *)notif {
    [NSNotificationCenter.defaultCenter postNotificationName:@"ShellBoxAlertNotification"
                                                      object:nil
                                                    userInfo:@{
                                                        @"title": @"panik",
                                                        @"message": notif.userInfo[@"message"] ?: @"",
                                                    }];
}
#endif

- (void)showMessage:(NSString *)message subtitle:(NSString *)subtitle {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:@"ShellBoxAlertNotification"
                                                          object:nil
                                                        userInfo:@{
                                                            @"title": message ?: @"Shell Box",
                                                            @"message": subtitle ?: @"",
                                                        }];
    });
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
    if (object == [UserPreferences shared]) {
        [self _updateStyleFromPreferences:YES];
    } else {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    }
}

- (void)_updateStyleFromPreferences:(BOOL)animated {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    NSTimeInterval duration = animated ? 0.1 : 0;
    [UIView animateWithDuration:duration animations:^{
        self.view.backgroundColor = [[UIColor alloc] shellBox_initWithHexString:UserPreferences.shared.palette.backgroundColor];
        self.termView.keyboardAppearance = UserPreferences.shared.keyboardAppearance;
    }];
    UIView *oldBarView = self.termView.inputAccessoryView;
    if (UserPreferences.shared.hideExtraKeysWithExternalKeyboard && self.hasExternalKeyboard) {
        self.termView.inputAccessoryView = nil;
    } else {
        self.termView.inputAccessoryView = self.barView;
    }
    if (self.termView.inputAccessoryView != oldBarView && self.termView.isFirstResponder) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.ignoreKeyboardMotion = YES; // avoid infinite recursion
            [self.termView reloadInputViews];
            self.ignoreKeyboardMotion = NO;
        });
    }
}
- (void)_updateStyleAnimated {
    [self _updateStyleFromPreferences:YES];
}

- (UIStatusBarStyle)preferredStatusBarStyle {
    return UserPreferences.shared.statusBarStyle;
}

- (BOOL)prefersStatusBarHidden {
    return UserPreferences.shared.hideStatusBar;
}

- (void)keyboardDidSomething:(NSNotification *)notification {
    if (self.ignoreKeyboardMotion)
        return;

    CGRect screenKeyboardFrame = [notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    UIScreen *screen = UIScreen.mainScreen;
    // notification.object is nil before iOS 16.1 and the correct UIScreen after iOS 16.1
    if (notification.object != nil)
        screen = notification.object;
    CGRect keyboardFrame = [self.view convertRect:screenKeyboardFrame fromCoordinateSpace:screen.coordinateSpace];
    if (CGRectEqualToRect(keyboardFrame, CGRectZero))
        return;
    CGRect intersection = CGRectIntersection(keyboardFrame, self.view.bounds);
    keyboardFrame = intersection;
    NSLog(@"%@ %@", notification.name, @(keyboardFrame));
    self.hasExternalKeyboard = keyboardFrame.size.height < 100;
    CGFloat pad = CGRectGetMaxY(self.view.bounds) - CGRectGetMinY(keyboardFrame);
    // The keyboard appears to be undocked. This means it can either be split or
    // truly floating. In the former case we want to keep the pad, but in the
    // latter we should fall back to the input accessory view instead of the
    // keyboard.
    if (pad != keyboardFrame.size.height && keyboardFrame.size.width != UIScreen.mainScreen.bounds.size.width) {
        pad = MAX(self.view.safeAreaInsets.bottom, self.termView.inputAccessoryView.frame.size.height);
    }
    // NSLog(@"pad %f", pad);
    self.bottomConstraint.constant = pad;

    BOOL initialLayout = self.termView.needsUpdateConstraints;
    [self.view setNeedsUpdateConstraints];
    if (!initialLayout) {
        // if initial layout hasn't happened yet, the terminal view is going to be at a really weird place, so animating it is going to look really bad
        NSNumber *interval = notification.userInfo[UIKeyboardAnimationDurationUserInfoKey];
        NSNumber *curve = notification.userInfo[UIKeyboardAnimationCurveUserInfoKey];
        [UIView animateWithDuration:interval.doubleValue
                              delay:0
                            options:curve.integerValue << 16
                         animations:^{
                             [self.view layoutIfNeeded];
                         }
                         completion:nil];
    }
}

- (void)setHasExternalKeyboard:(BOOL)hasExternalKeyboard {
    _hasExternalKeyboard = hasExternalKeyboard;
    [self _updateStyleFromPreferences:YES];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    // Hack to resolve a layering mismatch between the UI and preferences.
    if (@available(iOS 12.0, *)) {
        if (previousTraitCollection.userInterfaceStyle != self.traitCollection.userInterfaceStyle) {
            // Ensure that the relevant things listening for this will update.
            UserPreferences.shared.colorScheme = UserPreferences.shared.colorScheme;
        }
    }
}

#pragma mark Accessory Bar

- (void)showAbout:(id)sender {
    if ([sender isKindOfClass:[UIGestureRecognizer class]]) {
        UIGestureRecognizer *recognizer = sender;
        if (recognizer.state != UIGestureRecognizerStateBegan)
            return;
    }
    [self accessoryShowSettings];
}

- (void)accessoryInsertText:(NSString *)text {
    [self.termView insertText:text];
}

- (void)accessoryToggleControl {
    self.termView.controlKeySelected = !self.termView.controlKeySelected;
}

- (BOOL)accessoryControlActive {
    return self.termView.controlKeySelected;
}

- (void)accessoryPressArrow:(NSInteger)direction {
    char arrow = 0;
    switch (direction) {
        case 1: arrow = 'A'; break;
        case 2: arrow = 'B'; break;
        case 3: arrow = 'D'; break;
        case 4: arrow = 'C'; break;
        default: return;
    }
    [self pressKey:[self.terminal arrow:arrow]];
}

- (void)accessoryPaste {
    [self.termView paste:nil];
}

- (void)accessoryHideKeyboard {
    [self.termView loseFocus:nil];
}

- (void)accessoryShowSettings {
    UIViewController *settingsController = SwiftUISettingsController(NO);
    if (settingsController == nil)
        return;
    [self presentViewController:settingsController animated:YES completion:nil];
    [self.termView resignFirstResponder];
}

- (BOOL)accessoryShouldShowUpdateBadge {
    return FsNeedsRepositoryUpdate();
}

- (void)pressKey:(NSString *)key {
    [self.termView insertText:key];
}

- (void)switchTerminal:(UIKeyCommand *)sender {
    unsigned i = (unsigned) sender.input.integerValue;
    if (i == 7)
        self.terminal = self.sessionTerminal;
    else
        self.terminal = [Terminal terminalWithType:TTY_CONSOLE_MAJOR number:i];
}

- (void)increaseFontSize:(UIKeyCommand *)command {
    self.termView.overrideFontSize = self.termView.effectiveFontSize + 1;
}
- (void)decreaseFontSize:(UIKeyCommand *)command {
    self.termView.overrideFontSize = self.termView.effectiveFontSize - 1;
}
- (void)resetFontSize:(UIKeyCommand *)command {
    self.termView.overrideFontSize = 0;
}

- (NSArray<UIKeyCommand *> *)keyCommands {
    static NSMutableArray<UIKeyCommand *> *commands = nil;
    if (commands == nil) {
        commands = [NSMutableArray new];
        for (unsigned i = 1; i <= 7; i++) {
            [commands addObject:
             [UIKeyCommand keyCommandWithInput:[NSString stringWithFormat:@"%d", i]
                                 modifierFlags:UIKeyModifierCommand|UIKeyModifierAlternate|UIKeyModifierShift
                                        action:@selector(switchTerminal:)]];
        }
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"+"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(increaseFontSize:)
                      discoverabilityTitle:@"Increase Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"="
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(increaseFontSize:)]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"-"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(decreaseFontSize:)
                      discoverabilityTitle:@"Decrease Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"0"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(resetFontSize:)
                      discoverabilityTitle:@"Reset Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@","
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(showAbout:)
                      discoverabilityTitle:@"Settings"]];
    }
    return commands;
}

- (void)setTerminal:(Terminal *)terminal {
    _terminal = terminal;
    self.termView.terminal = self.terminal;
}

- (void)setSessionTerminal:(Terminal *)sessionTerminal {
    if (_terminal == _sessionTerminal)
        self.terminal = sessionTerminal;
    _sessionTerminal = sessionTerminal;
}

@end
