//
//  SceneDelegate.m
//  Shell Box
//
//  Created by Theodore Dubois on 10/26/19.
//

#import "SceneDelegate.h"
#import "AboutViewController.h"
#include <objc/message.h>

TerminalViewController *currentTerminalViewController = NULL;

@interface SceneDelegate ()

@property NSString *terminalUUID;
@property TerminalViewController *terminalViewController;

@end

static NSString *const TerminalUUID = @"TerminalUUID";

static UIViewController *SwiftUIRootControllerForTerminal(TerminalViewController *terminalViewController) {
    NSArray<NSString *> *classNames = @[
        @"Shell_Box.ShellBoxRootHostingController",
        @"ShellBox.ShellBoxRootHostingController",
        @"ShellBoxRootHostingController",
    ];
    Class hostingClass = Nil;
    for (NSString *className in classNames) {
        hostingClass = NSClassFromString(className);
        if (hostingClass != Nil)
            break;
    }
    SEL selector = NSSelectorFromString(@"controllerWithTerminalViewController:");
    if (hostingClass == Nil || ![hostingClass respondsToSelector:selector])
        return terminalViewController;

    UIViewController *controller = ((UIViewController *(*)(id, SEL, TerminalViewController *))objc_msgSend)(hostingClass, selector, terminalViewController);
    return controller ?: terminalViewController;
}

static TerminalViewController *TerminalViewControllerFromRoot(UIViewController *rootViewController) {
    if ([rootViewController isKindOfClass:TerminalViewController.class])
        return (TerminalViewController *) rootViewController;
    if ([rootViewController respondsToSelector:@selector(terminalViewController)])
        return [rootViewController valueForKey:@"terminalViewController"];
    return nil;
}

@implementation SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
        UINavigationController *vc = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
        AboutViewController *avc = (AboutViewController *) vc.topViewController;
        avc.recoveryMode = YES;
        self.window.rootViewController = vc;
        return;
    }

    TerminalViewController *vc = TerminalViewControllerFromRoot(self.window.rootViewController);
    if (vc == nil)
        return;
    self.terminalViewController = vc;
    self.window.rootViewController = SwiftUIRootControllerForTerminal(vc);
    vc.sceneSession = session;
    if (session.stateRestorationActivity == nil) {
        [vc startNewSession];
    } else {
        self.terminalUUID = session.stateRestorationActivity.userInfo[TerminalUUID];
        [vc reconnectSessionFromTerminalUUID:
         [[NSUUID alloc] initWithUUIDString:self.terminalUUID]];
    }
}

- (NSUserActivity *)stateRestorationActivityForScene:(UIScene *)scene {
    NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:@"app.ish.scene"];
    TerminalViewController *vc = self.terminalViewController ?: TerminalViewControllerFromRoot(self.window.rootViewController);
    if ([vc isKindOfClass:TerminalViewController.class]) {
        self.terminalUUID = vc.sessionTerminalUUID.UUIDString;
        if (self.terminalUUID != nil) {
            [activity addUserInfoEntriesFromDictionary:@{TerminalUUID: self.terminalUUID}];
        }
    }
    return activity;
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    TerminalViewController *terminalViewController = self.terminalViewController ?: TerminalViewControllerFromRoot(self.window.rootViewController);
    currentTerminalViewController = terminalViewController;
}

- (void)sceneWillResignActive:(UIScene *)scene {
    TerminalViewController *terminalViewController = self.terminalViewController ?: TerminalViewControllerFromRoot(self.window.rootViewController);

    if (currentTerminalViewController == terminalViewController) {
        currentTerminalViewController = NULL;
    }
}

@end
