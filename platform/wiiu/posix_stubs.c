// POSIX process and pipe functions that newlib declares but Cafe OS can't provide.
// Godot's Unix layer references them (OS.execute, OS.create_process, pipes, debug
// signal handlers); on Wii U they all fail with ENOSYS.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int execvp(const char *p_file, char *const p_argv[]) {
	(void)p_file;
	(void)p_argv;
	errno = ENOSYS;
	return -1;
}

FILE *popen(const char *p_command, const char *p_mode) {
	(void)p_command;
	(void)p_mode;
	errno = ENOSYS;
	return NULL;
}

int pclose(FILE *p_stream) {
	(void)p_stream;
	errno = ENOSYS;
	return -1;
}

pid_t waitpid(pid_t p_pid, int *r_status, int p_options) {
	(void)p_pid;
	(void)r_status;
	(void)p_options;
	errno = ECHILD;
	return -1;
}

int sigaction(int p_sig, const struct sigaction *p_act, struct sigaction *r_oldact) {
	(void)p_sig;
	(void)p_act;
	(void)r_oldact;
	errno = ENOSYS;
	return -1;
}

int pipe(int r_fds[2]) {
	(void)r_fds;
	errno = ENOSYS;
	return -1;
}

pid_t setsid(void) {
	errno = ENOSYS;
	return -1;
}

int mkfifo(const char *p_path, mode_t p_mode) {
	(void)p_path;
	(void)p_mode;
	errno = ENOSYS;
	return -1;
}
