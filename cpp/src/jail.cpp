#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif  // !defined(_GNU_SOURCE)
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int init_exitstatus = 0;

void init_term(int sig) {
  _exit(init_exitstatus);
}

void init(pid_t rootpid) {
  pid_t pid;
  int status;
  // So that we exit with the status of the process we launched.
  signal(SIGTERM, init_term);
  while ((pid = wait(&status)) > 0) {
    // This loop will end iff either:
    //  -There are no processes left inside our PID namespace.
    //  -Or we get a signal.
    if (pid == rootpid)
      init_exitstatus = status;
  }
  if (!WIFEXITED(init_exitstatus))
    _exit(254);
  _exit(WEXITSTATUS(init_exitstatus));
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <program>\n", argv[0]);
    return 1;
  }

  printf("<program> is %s\n", argv[1]);

  long pid = syscall(SYS_clone, CLONE_NEWNS | CLONE_NEWPID | SIGCHLD, nullptr);
  if (pid == 0) {
    // This is the child.
    // systemd configures all mounts as shared. Remount as private to
    // make the mount namespace meaningful.
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
      perror("mount(/, MS_PRIVATE)");
      exit(1);
    }
    // /proc could be remounted without unmounting the previous mount. However,
    // that would show two separate /proc mounts inside the namespace, and a
    // root process would be able to unmount the namespaced /proc and see the
    // non-namespaced /proc.
    // Use MNT_DETACH since there are references to /proc still open.
    if (umount2("/proc", MNT_DETACH) < 0) {
      perror("umount(/proc)");
      exit(1);
    }
    if (mount("proc", "/proc", "proc", MS_RDONLY, "") < 0) {
      perror("mount(/proc)");
      exit(1);
    }
    char** child_argv = static_cast<char**>(calloc(argc, sizeof(char*)));
    for (size_t i = 1; i < argc; i++) {
      child_argv[i - 1] = argv[i];
    }
    child_argv[argc - 1] = nullptr;

    pid_t child_pid = fork();
    if (child_pid < 0) {
      _exit(child_pid);
    } else if (child_pid > 0) {
      // Best effort. Don't bother checking the return value.
      prctl(PR_SET_NAME, "jail-init");
      // Provide a simple init() process inside the PID namespace.
      init(child_pid);  // Never returns.
    }
    int res = execve(child_argv[0], child_argv, environ);
    if (res < 0) {
      perror("execve");
      exit(1);
    }
  } else if (pid > 0) {
    // This is the parent.
    int status;
    pid_t p = waitpid(pid, &status, __WALL);
    if (p == pid) {
      printf("child exited\n");
      exit(0);
    } else {
      perror("waitpid");
      exit(1);
    }
  } else {
    // error
    perror("sys_clone");
    exit(1);
  }
  return 0;
}
