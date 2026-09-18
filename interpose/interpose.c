/*
 * interpose.c — minimal LD_PRELOAD process-creation interceptor.
 *
 * Logs every intercepted exec-family call to the file named by the
 * env var INTERPOSE_LOG, one line per call:
 *     <pid> <func> <argv0> [argv...] <-
 *
 * Purely env-driven: no dispatcher, no daemon. Env (LD_PRELOAD +
 * INTERPOSE_LOG) is inherited by children, so every process spawned by
 * the build stays intercepted.
 *
 * Build: cc -shared -fPIC -O2 -o libinterpose.so interpose.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_ARGS 64

static int log_fd = -2; /* -2 = not opened yet, -1 = give up */

static int get_log_fd(void)
{
    if (log_fd == -2) {
        const char *path = getenv("INTERPOSE_LOG");
        if (path == NULL || *path == '\0') {
            log_fd = -1;
        } else {
            log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        }
    }
    return log_fd;
}

/* Join argv into a single line and append it to the log. Best effort:
 * uses only async-signal-safe calls after the snprintf. */
static void log_call(const char *func, char *const argv[])
{
    int fd = get_log_fd();
    if (fd < 0 || argv == NULL || argv[0] == NULL)
        return;

    char buf[4096];
    int off = snprintf(buf, sizeof buf, "%ld %s", (long)getpid(), func);
    for (int i = 0; argv[i] != NULL && off < (int)sizeof buf - 2; i++) {
        off += snprintf(buf + off, sizeof buf - off, " %s", argv[i]);
    }
    if (off >= (int)sizeof buf - 3)
        off = (int)sizeof buf - 3;
    buf[off++] = ' ';
    buf[off++] = '<';
    buf[off++] = '-';
    buf[off++] = '\n';
    ssize_t r = write(fd, buf, (size_t)off);
    (void)r;
}

/* ---- real function resolution ---- */

/* Resolve the real function. Uses only write() on failure so the .so
 * can stay free of libc/stdout data-symbol dependencies and be usable
 * from both glibc and musl processes. */
static void *real(const char *name)
{
    void *h = dlsym(RTLD_NEXT, name);
    if (h == NULL) {
        static const char msg[] = "interpose: cannot resolve ";
        write(STDERR_FILENO, msg, sizeof msg - 1);
        write(STDERR_FILENO, name, strlen(name));
        write(STDERR_FILENO, "\n", 1);
        _exit(127);
    }
    return h;
}

/* ---- exec family ---- */

int execve(const char *path, char *const argv[], char *const envp[])
{
    static int (*fn)(const char *, char *const[], char *const[]);
    if (!fn)
        fn = real("execve");
    char *tmp[] = {(char *)path, NULL};
    log_call("execve", argv && argv[0] ? argv : tmp);
    return fn(path, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    static int (*fn)(const char *, char *const[]);
    if (!fn)
        fn = real("execv");
    char *tmp[] = {(char *)path, NULL};
    log_call("execv", argv && argv[0] ? argv : tmp);
    return fn(path, argv);
}

int execvp(const char *file, char *const argv[])
{
    static int (*fn)(const char *, char *const[]);
    if (!fn)
        fn = real("execvp");
    char *tmp[] = {(char *)file, NULL};
    log_call("execvp", argv && argv[0] ? argv : tmp);
    return fn(file, argv);
}

#ifdef __GLIBC__
int execvpe(const char *file, char *const argv[], char *const envp[])
{
    static int (*fn)(const char *, char *const[], char *const[]);
    if (!fn)
        fn = real("execvpe");
    char *tmp[] = {(char *)file, NULL};
    log_call("execvpe", argv && argv[0] ? argv : tmp);
    return fn(file, argv, envp);
}
#endif

/* The variadic execl* wrappers flatten into argv, then log and forward. */

static int flatten_and_exec(const char *func, const char *path, int use_envp,
                            const char *arg0, va_list ap)
{
    char *argv[MAX_ARGS + 1];
    char *envp_buf[MAX_ARGS + 1];
    int n = 0;
    if (arg0 != NULL) {
        argv[n++] = (char *)arg0;
        char *a;
        while (n < MAX_ARGS && (a = va_arg(ap, char *)) != NULL)
            argv[n++] = a;
        argv[n] = NULL;
    } else {
        argv[0] = NULL;
    }

    char **envp = NULL;
    if (use_envp) {
        /* first vararg after NULL terminator is the envp array */
        envp = va_arg(ap, char **);
        (void)envp_buf;
    }

    char *tmp[] = {(char *)path, NULL};
    log_call(func, argv[0] ? argv : tmp);

    if (strcmp(func, "execl") == 0)
        return ((int (*)(const char *, char *const[]))real("execv"))(path, argv);
    if (strcmp(func, "execlp") == 0)
        return ((int (*)(const char *, char *const[]))real("execvp"))(path, argv);
    /* execle */
    return ((int (*)(const char *, char *const[], char *const[]))real("execve"))(path, argv, envp);
}

int execl(const char *path, const char *arg0, ...)
{
    va_list ap;
    va_start(ap, arg0);
    int r = flatten_and_exec("execl", path, 0, arg0, ap);
    va_end(ap);
    return r;
}

int execlp(const char *file, const char *arg0, ...)
{
    va_list ap;
    va_start(ap, arg0);
    int r = flatten_and_exec("execlp", file, 0, arg0, ap);
    va_end(ap);
    return r;
}

int execle(const char *path, const char *arg0, ...)
{
    va_list ap;
    va_start(ap, arg0);
    int r = flatten_and_exec("execle", path, 1, arg0, ap);
    va_end(ap);
    return r;
}

/* ---- posix_spawn family ---- */

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[])
{
    static int (*fn)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                     const posix_spawnattr_t *, char *const[], char *const[]);
    if (!fn)
        fn = real("posix_spawn");
    char *tmp[] = {(char *)path, NULL};
    log_call("posix_spawn", argv && argv[0] ? argv : tmp);
    return fn(pid, path, fa, attr, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[])
{
    static int (*fn)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                     const posix_spawnattr_t *, char *const[], char *const[]);
    if (!fn)
        fn = real("posix_spawnp");
    char *tmp[] = {(char *)file, NULL};
    log_call("posix_spawnp", argv && argv[0] ? argv : tmp);
    return fn(pid, file, fa, attr, argv, envp);
}

/* ---- shell wrappers ---- */

FILE *popen(const char *cmd, const char *mode)
{
    static FILE *(*fn)(const char *, const char *);
    if (!fn)
        fn = real("popen");
    char *tmp[] = {(char *)cmd, (char *)mode, NULL};
    log_call("popen", tmp);
    return fn(cmd, mode);
}

int system(const char *cmd)
{
    static int (*fn)(const char *);
    if (!fn)
        fn = real("system");
    char *tmp[] = {(char *)cmd, NULL};
    log_call("system", tmp);
    return fn(cmd);
}

int pclose(FILE *f)
{
    static int (*fn)(FILE *);
    if (!fn)
        fn = real("pclose");
    return fn(f);
}
