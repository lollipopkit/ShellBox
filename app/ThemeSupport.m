#import "Theme.h"
#import "fs/proc/ish.h"

NSString *const ThemesUpdatedNotification = @"ThemesUpdatedNotification";
NSString *const ThemeUpdatedNotification = @"ThemeUpdatedNotification";

static char *get_documents_directory_impl(void) {
    return strdup(NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject.UTF8String);
}

__attribute__((constructor))
static void ShellBoxInstallThemeSupport(void) {
    get_documents_directory = get_documents_directory_impl;
}

@implementation UIColor (ShellBox)
- (nullable instancetype)shellBox_initWithHexString:(NSString *)string {
    if (![string hasPrefix:@"#"]) {
        return nil;
    }
    NSScanner *scanner = [NSScanner scannerWithString:string];
    scanner.scanLocation = 1;
    unsigned int value;
    if (![scanner scanHexInt:&value] || scanner.scanLocation != string.length) {
        return nil;
    }

    unsigned int red;
    unsigned int green;
    unsigned int blue;
    unsigned int alpha;
    if (string.length == 4) {
        blue = ((value & 0x00f) >> 0) * 0x11;
        green = ((value & 0x0f0) >> 4) * 0x11;
        red = ((value & 0xf00) >> 8) * 0x11;
        alpha = 0xff;
    } else if (string.length == 5) {
        blue = ((value & 0x000f) >> 0) * 0x11;
        green = ((value & 0x00f0) >> 4) * 0x11;
        red = ((value & 0x0f00) >> 8) * 0x11;
        alpha = ((value & 0xf000) >> 12) * 0x11;
    } else if (string.length == 7) {
        blue = (value & 0x0000ff) >> 0;
        green = (value & 0x00ff00) >> 8;
        red = (value & 0xff0000) >> 16;
        alpha = 0xff;
    } else if (string.length == 9) {
        blue = (value & 0x000000ff) >> 0;
        green = (value & 0x0000ff00) >> 8;
        red = (value & 0x00ff0000) >> 16;
        alpha = (value & 0xff000000) >> 24;
    } else {
        return nil;
    }

    return [UIColor colorWithRed:(CGFloat) red / 0xff
                           green:(CGFloat) green / 0xff
                            blue:(CGFloat) blue / 0xff
                           alpha:(CGFloat) alpha / 0xff];
}
@end
