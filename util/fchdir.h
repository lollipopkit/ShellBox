// temporarily change directory and block other threads from doing so
// useful for simulating mknodat on ios, dealing with long unix socket paths, etc
// Returns 0 with the lock held and the directory changed, or -1 with
// neither. unlock_fchdir is only for a successful lock.
int lock_fchdir(int dirfd);
void unlock_fchdir(void);
