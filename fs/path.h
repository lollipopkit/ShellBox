#ifndef PATH_H
#define PATH_H

#define AT_PWD (struct fd *) -2

// The directory a relative path is resolved against, or AT_PWD. `path` decides
// whether the descriptor is looked at at all: Linux ignores dirfd for an
// absolute path rather than validating it, so `openat(-1, "/", O_DIRECTORY)`
// opens the root there and answered EBADF here. rpm's file-state machine does
// exactly that, and every package in a dnf transaction failed to unpack.
//
// path_normalize starts from the root for a leading slash and never looks at
// `at`, so what this returns in that case only has to be valid.
struct fd *at_fd(fd_t f, const char *path);

#define N_SYMLINK_FOLLOW 1
#define N_SYMLINK_NOFOLLOW 2
#define N_PARENT_DIR_WRITE 4

// Normalizes the path specified and writes the result into the out buffer.
//
// Normalization means:
//  - prepending the current or root directory
//  - converting multiple slashes into one
//  - resolving . and ..
//  - resolving symlinks, skipping the last path component if the follow_links
//    argument is true
// The result will always begin with a slash or be empty.
//
// If the normalized path plus the null terminator would be longer than
// MAX_PATH, _ENAMETOOLONG is returned. The out buffer is expected to be at
// least MAX_PATH in size.
//
// at is the file descriptor to use as a base to interpret relative paths. If
// at is AT_PWD, uses current->pwd (with appropriate locking).
int path_normalize(struct fd *at, const char *path, char *out, int flags);
bool path_is_normalized(const char *path);

// Helper function for iterating through a normalized path.
//
// The *path pointer is advanced to point to the next /, and the next path
// component is copied to component. component must point to a buffer large
// enough to hold a string of MAX_NAME characters.
//
// If the next path component was successfully copied, returns true; otherwise
// returns false. If an error occurred, *err is set to the error code.
// Otherwise, the end of the path has been reached.
bool path_next_component(const char **path, char *component, int *err);

#endif
