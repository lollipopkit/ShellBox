//
//  TerminalView.m
//  Shell Box
//
//  Created by Theodore Dubois on 11/3/17.
//

#import "TerminalView.h"
#import "NSObject+SaneKVO.h"
#import "UserPreferences.h"

@interface SwiftTermHostView : UIView <TerminalRenderer>
@property (nonatomic, weak) Terminal *terminal;
@property (nonatomic, weak) UIInputView *customInputAccessoryView;
- (BOOL)becomeTerminalFirstResponder;
- (void)resignTerminalFirstResponder;
- (void)feedData:(NSData *)data;
- (void)scrollToBottom;
- (void)copySelection;
- (void)clearScrollback;
- (void)sendText:(NSString *)text;
- (void)sendData:(NSData *)data;
- (void)sendArrow:(NSInteger)direction;
- (void)applyFontFamily:(NSString *)fontFamily
               fontSize:(NSNumber *)fontSize
        foregroundColor:(NSString *)foregroundColor
        backgroundColor:(NSString *)backgroundColor
            cursorColor:(NSString *)cursorColor
       paletteOverrides:(NSArray<NSString *> *)paletteOverrides
            cursorStyle:(NSInteger)cursorStyle
            blinkCursor:(BOOL)blinkCursor
        optionAsMetaKey:(BOOL)optionAsMetaKey
     keyboardAppearance:(UIKeyboardAppearance)keyboardAppearance;
@end

@interface TerminalView ()
@property (nonatomic) SwiftTermHostView *hostView;
@end

static const char *controlKeys = "abcdefghijklmnopqrstuvwxyz@^26-=[]\\ ";

@implementation TerminalView
@synthesize canBecomeFirstResponder;

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        [self setup];
    }
    return self;
}

- (instancetype)initWithCoder:(NSCoder *)coder {
    if (self = [super initWithCoder:coder]) {
        [self setup];
    }
    return self;
}

- (void)setup {
    self.inputAssistantItem.leadingBarButtonGroups = @[];
    self.inputAssistantItem.trailingBarButtonGroups = @[];

    Class hostClass = NSClassFromString(@"SwiftTermHostView");
    NSAssert(hostClass != Nil, @"SwiftTermHostView must be linked into the app target");
    self.hostView = [[hostClass alloc] initWithFrame:self.bounds];
    self.hostView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self addSubview:self.hostView];

    UserPreferences *prefs = UserPreferences.shared;
    [prefs observe:@[@"colorScheme", @"fontFamily", @"fontSize", @"theme", @"cursorStyle", @"blinkCursor", @"optionMapping"]
           options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self updateStyle];
        });
    }];
}

- (void)setTerminal:(Terminal *)terminal {
    if (_terminal) {
        _terminal.renderer = nil;
    }
    _terminal = terminal;
    self.hostView.terminal = terminal;
    terminal.renderer = self.hostView;
    [self updateStyle];
}

- (void)setInputAccessoryView:(UIInputView *)inputAccessoryView {
    _inputAccessoryView = inputAccessoryView;
    self.hostView.customInputAccessoryView = inputAccessoryView;
}

- (void)setOverrideFontSize:(CGFloat)overrideFontSize {
    _overrideFontSize = overrideFontSize;
    [self updateStyle];
}

- (void)setOverrideAppearance:(enum OverrideAppearance)overrideAppearance {
    _overrideAppearance = overrideAppearance;
    [self updateStyle];
}

- (CGFloat)effectiveFontSize {
    if (self.overrideFontSize != 0)
        return self.overrideFontSize;
    return UserPreferences.shared.fontSize.doubleValue;
}

- (void)setKeyboardAppearance:(UIKeyboardAppearance)keyboardAppearance {
    _keyboardAppearance = keyboardAppearance;
    [self updateStyle];
}

- (void)updateStyle {
    if (!self.hostView)
        return;
    UserPreferences *prefs = UserPreferences.shared;
    if (_overrideFontSize == prefs.fontSize.doubleValue)
        _overrideFontSize = 0;
    Palette *palette = prefs.palette;
    if (self.overrideAppearance != OverrideAppearanceNone) {
        palette = self.overrideAppearance == OverrideAppearanceLight ? prefs.theme.lightPalette : prefs.theme.darkPalette;
    }
    [self.hostView applyFontFamily:prefs.fontFamily
                          fontSize:@(self.effectiveFontSize)
                   foregroundColor:palette.foregroundColor
                   backgroundColor:palette.backgroundColor
                       cursorColor:palette.cursorColor
                  paletteOverrides:palette.colorPaletteOverrides
                       cursorStyle:prefs.cursorStyle
                       blinkCursor:prefs.blinkCursor
                   optionAsMetaKey:prefs.optionMapping == OptionMapEsc
                keyboardAppearance:self.keyboardAppearance];
}

- (BOOL)becomeFirstResponder {
    [self reloadInputViews];
    return [self.hostView becomeTerminalFirstResponder];
}

- (BOOL)resignFirstResponder {
    [self.hostView resignTerminalFirstResponder];
    return [super resignFirstResponder];
}

- (void)loseFocus:(id)sender {
    [self resignFirstResponder];
}

- (void)insertText:(NSString *)text {
    if (self.controlKeySelected) {
        self.controlKeySelected = NO;
        if (text.length == 1)
            return [self insertControlChar:[text characterAtIndex:0]];
    }

    text = [text stringByReplacingOccurrencesOfString:@"\n" withString:@"\r"];
    [self.hostView sendText:text];
}

- (void)insertControlChar:(char)ch {
    if (strchr(controlKeys, ch) != NULL) {
        if (ch == ' ') ch = '\0';
        if (ch == '2') ch = '@';
        if (ch == '6') ch = '^';
        if (ch != '\0')
            ch = toupper(ch) ^ 0x40;
        [self.hostView sendData:[NSData dataWithBytes:&ch length:1]];
    }
}

- (void)deleteBackward {
    [self insertText:@"\x7f"];
}

- (BOOL)hasText {
    return YES;
}

- (void)paste:(id)sender {
    NSString *string = UIPasteboard.generalPasteboard.string;
    if (string) {
        [self insertText:string];
    }
}

- (void)copy:(id)sender {
    [self.hostView copySelection];
}

- (void)clearScrollback:(UIKeyCommand *)command {
    [self.hostView clearScrollback];
}

- (void)pressArrow:(NSInteger)direction {
    [self.hostView sendArrow:direction];
}

@end
