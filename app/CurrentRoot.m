//
//  CurrentRoot.m
//  ShellBox
//
//  Created by Theodore Dubois on 11/4/21.
//

#import "CurrentRoot.h"
#include "kernel/calls.h"
#include "fs/path.h"

#ifdef ISH_LINUX
#import "LinuxInterop.h"
#endif

int fs_ish_version;
int fs_ish_apt_version;

#if !ISH_LINUX
static ssize_t read_file(const char *path, char *buf, size_t size) {
    struct fd *fd = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    ssize_t n = fd->ops->read(fd, buf, size);
    fd_close(fd);
    if (n == size)
        return _ENAMETOOLONG;
    return n;
}

static ssize_t write_file(const char *path, const char *buf, size_t size) {
    struct fd *fd = generic_open(path, O_WRONLY_|O_CREAT_|O_TRUNC_, 0644);
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    ssize_t n = fd->ops->write(fd, buf, size);
    fd_close(fd);
    return n;
}
static int remove_directory(const char *path) {
    return generic_rmdirat(AT_PWD, path);
}
#else
#define read_file linux_read_file
#define write_file linux_write_file
#define remove_directory linux_remove_directory
#endif

static void create_directory_chain(NSString *path) {
    NSArray<NSString *> *components = [path pathComponents];
    if (components.count == 0)
        return;
    NSMutableString *currentPath = [NSMutableString string];
    for (NSString *component in components) {
        if ([component isEqualToString:@"/"]) {
            [currentPath setString:@"/"];
            continue;
        }
        if (currentPath.length > 1)
            [currentPath appendString:@"/"];
        [currentPath appendString:component];
        if (currentPath.length > 1)
            generic_mkdirat(AT_PWD, currentPath.UTF8String, 0755);
    }
}

void FsInitialize(void) {
    // /ish/version is the last ish version that opened this root. Used to migrate the filesystem.
    char buf[1000];
    NSString *currentVersion = NSBundle.mainBundle.infoDictionary[(__bridge NSString *) kCFBundleVersionKey];
    NSString *currentVersionFile = [NSString stringWithFormat:@"%@\n", currentVersion];
    ssize_t n = read_file("/ish/version", buf, sizeof(buf));
    if (n >= 0) {
        NSString *version = [[NSString alloc] initWithBytesNoCopy:buf length:n encoding:NSUTF8StringEncoding freeWhenDone:NO];
        version = [version stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        fs_ish_version = version.intValue;

        version = nil;

        n = read_file("/ish/apt-version", buf, sizeof(buf));
        if (n >= 0) {
            NSString *version = [[NSString alloc] initWithBytesNoCopy:buf length:n encoding:NSUTF8StringEncoding freeWhenDone:NO];
            version = [version stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            fs_ish_apt_version = version.intValue;
        }

        // If no newer value for CURRENT_APT_VERSION, do silent update.
        if (fs_ish_apt_version >= CURRENT_APT_VERSION)
            FsUpdateRepositories();

        if (currentVersion.intValue > fs_ish_version) {
            fs_ish_version = currentVersion.intValue;
            write_file("/ish/version", currentVersionFile.UTF8String, [currentVersionFile lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
        }
    }

    // Apply rootfs overlay patches (e.g. fetch-polyfill.js)
    FsApplyOverlay();

    if (fs_ish_version == 0) {
        create_directory_chain(@"/ish");
        fs_ish_version = currentVersion.intValue;
        write_file("/ish/version", currentVersionFile.UTF8String, [currentVersionFile lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    }
}

bool FsIsManaged(void) {
    return fs_ish_version != 0;
}

bool FsNeedsRepositoryUpdate(void) {
    return FsIsManaged() && fs_ish_apt_version < CURRENT_APT_VERSION;
}

void FsUpdateOnlyRepositoriesFile(void) {
    NSURL *repositories = [NSBundle.mainBundle URLForResource:@"repositories" withExtension:@"txt"];
    if (repositories != nil) {
        NSMutableData *repositoriesData = [@"# This file contains pinned APT sources managed by ShellBox. If the /ish directory\n"
                                           @"# exists, ShellBox uses the metadata stored in it to keep this file up to date (by\n"
                                           @"# overwriting the contents on boot.)\n" dataUsingEncoding:NSUTF8StringEncoding].mutableCopy;
        [repositoriesData appendData:[NSData dataWithContentsOfURL:repositories]];
        write_file("/etc/apt/sources.list", repositoriesData.bytes, repositoriesData.length);
    }
}

void FsUpdateRepositories(void) {
    FsUpdateOnlyRepositoriesFile();
    fs_ish_apt_version = CURRENT_APT_VERSION;
    NSString *currentVersionFile = [NSString stringWithFormat:@"%d\n", fs_ish_apt_version];
    write_file("/ish/apt-version", currentVersionFile.UTF8String, [currentVersionFile lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    remove_directory("/ish/apt");
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:FsUpdatedNotification object:nil];
    });
}

void FsApplyOverlay(void) {
    // Locate RootfsPatch.bundle inside the app bundle
    NSURL *patchBundleURL = [NSBundle.mainBundle URLForResource:@"RootfsPatch" withExtension:@"bundle"];
    if (patchBundleURL == nil) {
        NSLog(@"[RootfsPatch] bundle not found in app, skipping overlay");
        return;
    }
    NSBundle *patchBundle = [NSBundle bundleWithURL:patchBundleURL];
    if (patchBundle == nil) {
        NSLog(@"[RootfsPatch] failed to load bundle at %@", patchBundleURL);
        return;
    }

    // Read manifest
    NSURL *manifestURL = [patchBundle URLForResource:@"manifest" withExtension:@"plist"];
    if (manifestURL == nil) {
        NSLog(@"[RootfsPatch] manifest.plist not found in bundle");
        return;
    }
    NSDictionary *manifest = [NSDictionary dictionaryWithContentsOfURL:manifestURL];
    if (manifest == nil) {
        NSLog(@"[RootfsPatch] failed to parse manifest.plist");
        return;
    }

    int patchVersion = [manifest[@"version"] intValue];
    if (patchVersion <= 0)
        return;

    // Check installed overlay version in guest fs. This version must increase
    // whenever patch contents change because the app group container can
    // survive uninstall/reinstall during development.
    char buf[100];
    int installedVersion = 0;
    ssize_t n = read_file("/ish/overlay-version", buf, sizeof(buf));
    if (n > 0) {
        buf[n] = '\0';
        installedVersion = atoi(buf);
    }
    if (installedVersion >= patchVersion) {
        NSLog(@"[RootfsPatch] patch %d already installed (bundle %d), skipping", installedVersion, patchVersion);
        return;
    }

    NSLog(@"[RootfsPatch] applying patch %d over installed %d", patchVersion, installedVersion);

    // Apply each file from the manifest
    NSArray *files = manifest[@"files"];
    if (files == nil)
        return;

    int applied = 0, failed = 0;
    for (NSDictionary *entry in files) {
        NSString *src = entry[@"src"];
        NSString *dst = entry[@"dst"];
        if (src == nil || dst == nil)
            continue;

        // Ensure parent directories exist in guest fs
        NSString *parentDir = [dst stringByDeletingLastPathComponent];
        if (parentDir.length > 1)
            create_directory_chain(parentDir);

        // Read file from patch bundle
        NSURL *srcURL = [patchBundle.bundleURL URLByAppendingPathComponent:src];
        NSData *data = [NSData dataWithContentsOfURL:srcURL];
        if (data == nil) {
            NSLog(@"[RootfsPatch] SKIP %@ (not found in bundle)", src);
            failed++;
            continue;
        }

        ssize_t written = write_file(dst.UTF8String, data.bytes, data.length);
        if (written < 0) {
            NSLog(@"[RootfsPatch] FAIL %@ -> %@ (error %zd)", src, dst, written);
            failed++;
        } else {
            NSLog(@"[RootfsPatch] OK %@ -> %@ (%lu bytes)", src, dst, (unsigned long)data.length);
            NSNumber *mode = entry[@"mode"];
            if (mode != nil) {
                generic_setattrat(AT_PWD, dst.UTF8String, (struct attr) {.type = attr_mode, .mode = mode.intValue}, false);
            }
            applied++;
        }
    }

    // Record installed version
    generic_mkdirat(AT_PWD, "/ish", 0755);
    NSString *versionStr = [NSString stringWithFormat:@"%d\n", patchVersion];
    write_file("/ish/overlay-version", versionStr.UTF8String,
               [versionStr lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);

    NSLog(@"[RootfsPatch] patch %d done: %d applied, %d failed", patchVersion, applied, failed);
}

NSString *const FsUpdatedNotification = @"FsUpdatedNotification";
