//
//  TerminalView.h
//  ShellBox
//
//  Created by Theodore Dubois on 11/3/17.
//

#import <UIKit/UIKit.h>
#import "Terminal.h"

enum OverrideAppearance {
    OverrideAppearanceNone,
    OverrideAppearanceLight,
    OverrideAppearanceDark,
};

@interface TerminalView : UIView <UIKeyInput>

@property IBInspectable (nonatomic) BOOL canBecomeFirstResponder;

@property (nonatomic) CGFloat overrideFontSize;
@property (readonly) CGFloat effectiveFontSize;
@property (nonatomic) enum OverrideAppearance overrideAppearance;

@property (nonatomic) UIKeyboardAppearance keyboardAppearance;

@property (nonatomic, weak) UIInputView *inputAccessoryView;
@property (nonatomic) BOOL controlKeySelected;

@property (nonatomic) Terminal *terminal;

- (void)loseFocus:(id)sender;
- (void)pressArrow:(NSInteger)direction;

@end
