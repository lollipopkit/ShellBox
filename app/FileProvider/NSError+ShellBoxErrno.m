//
//  NSError+ShellBoxErrno.m
//  ShellBox
//
//  Created by Theodore Dubois on 12/15/18.
//

#import <FileProvider/FileProvider.h>
#import "NSError+ShellBoxErrno.h"
#include "kernel/errno.h"

@implementation NSError (ShellBoxErrno)

+ (NSError *)errorWithShellBoxErrno:(long)err itemIdentifier:(nonnull NSFileProviderItemIdentifier)identifier {
    switch (err) {
        case _ENOENT:
            return [NSError fileProviderErrorForNonExistentItemWithIdentifier:identifier];
    }
    return [NSError errorWithDomain:ShellBoxErrnoDomain
                               code:err
                           userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"error code %ld", err]}];
}

@end

NSString *const ShellBoxErrnoDomain = @"ShellBoxErrnoDomain";
