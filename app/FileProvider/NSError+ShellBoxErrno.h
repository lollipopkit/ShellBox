//
//  NSError+ShellBoxErrno.h
//  ShellBox
//
//  Created by Theodore Dubois on 12/15/18.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface NSError (ShellBoxErrno)

+ (NSError *)errorWithShellBoxErrno:(long)err itemIdentifier:(NSFileProviderItemIdentifier)identifier;

@end

extern NSString *const ShellBoxErrnoDomain;

NS_ASSUME_NONNULL_END
