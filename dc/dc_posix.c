/*
 * POSIX calls that newlib declares but KOS doesn't implement, used by
 * sys/sys_unix.c and qcommon/common.c.  None of them mean anything on the
 * Dreamcast (no users, processes or FIFOs), so they all just fail.
 */

#include <errno.h>
#include <pwd.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

uid_t getuid( void ) {
	return 0;
}

struct passwd *getpwuid( uid_t uid ) {
	(void)uid;
	return NULL;
}

mode_t umask( mode_t mask ) {
	(void)mask;
	return 0;
}

int access( const char *path, int mode ) {
	struct stat st;

	(void)mode;
	return stat( path, &st );
}

int mkfifo( const char *path, mode_t mode ) {
	(void)path; (void)mode;
	errno = ENOSYS;
	return -1;
}

int execvp( const char *file, char *const argv[] ) {
	(void)file; (void)argv;
	errno = ENOSYS;
	return -1;
}

int execl( const char *path, const char *arg, ... ) {
	(void)path; (void)arg;
	errno = ENOSYS;
	return -1;
}
