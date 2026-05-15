//
//  CurrentRoot.h
//  Shell Box
//
//  Created by Theodore Dubois on 11/4/21.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

extern int fs_ish_version;
extern int fs_ish_apt_version;

void FsInitialize(void);
bool FsIsManaged(void);
bool FsNeedsRepositoryUpdate(void);
void FsUpdateOnlyRepositoriesFile(void);
void FsUpdateRepositories(void);

/// An integer representing the current major version of the apt source configuration. An upgrade will be run if the number in /ish/apt-version is smaller. After a successful upgrade, the newer number is copied into /ish/apt-version.
/// To upgrade:
/// - update the default rootfs to the same version
/// - update gen_apt_sources.py to generate the new version of /etc/apt/sources.list
/// - set both of the following constants appropriately, making sure to use a larger number than the previous one
#define CURRENT_APT_VERSION 1200
#define CURRENT_APT_VERSION_STRING "Debian 12 bookworm"

/// Apply rootfs patches from RootfsPatch.bundle on boot.
/// The bundle contains a manifest.plist with a version number and file list.
/// Files are written to guest fs when the bundle version exceeds /ish/overlay-version.
void FsApplyOverlay(void);

extern NSString *const FsUpdatedNotification;

NS_ASSUME_NONNULL_END
