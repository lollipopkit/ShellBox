import UIKit

NSSetUncaughtExceptionHandler(ShellBoxExceptionHandler)
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
