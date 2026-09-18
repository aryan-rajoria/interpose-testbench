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
 * One artifact per CPU architecture covers glibc (any version) and musl:
 * built with -nostdlib, it has no DT_NEEDED, and its libc references
 * (exec*, snprintf, ...) bind to whichever libc the host process uses.
 * The real-function lookup normally uses dlsym (declared weak); in
 * processes that lack it (glibc < 2.34 without libdl, e.g. Ubuntu 20.04
 * /bin/sh and compiler drivers) it falls back to a small _r_debug/
 * link_map walker — a dlsym(RTLD_NEXT) replacement with no libdl
 * dependency at all.
 *
 * Build: cc -shared -fPIC -O2 -fno-plt -fno-stack-protector -nostdlib \
 *            -o libinterpose.so interpose.c      (on musl; see build.sh)
 */
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>

/* musl's elf.h lacks the glibc ElfW/ELF_ST_TYPE conveniences. */
#ifndef ElfW
#if __SIZEOF_POINTER__ == 8
#define ElfW(type) Elf64_##type
#else
#define ElfW(type) Elf32_##type
#endif
#endif
#ifndef ELF_ST_TYPE
#define ELF_ST_TYPE(info) ((info) & 0xf)
#endif

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

/* dlsym, declared weak so this .so also loads into processes that have
 * no libdl (glibc < 2.34). Must be compiled with -fno-plt so that
 * testing the symbol address reads the GOT slot instead of a PLT stub
 * (an unresolved weak function then reads as NULL instead of crashing). */
#define RTLD_NEXT ((void *) -1l)
extern void *dlsym(void *, const char *) __attribute__((weak));

/* First fields of struct link_map / struct r_debug. De-facto stable,
 * gdb-facing layout shared by glibc and musl dynamic loaders. */
struct link_map_head {
    ElfW(Addr) l_addr;   /* load base of the object */
    char *l_name;        /* object name */
    ElfW(Dyn) *l_ld;     /* relocated PT_DYNAMIC of the object */
    struct link_map_head *l_next, *l_lprev;
};
struct r_debug_head {
    int r_version;       /* pads to pointer alignment on LP64 */
    struct link_map_head *r_map;
};
/* _r_debug is the loader-exported struct itself (not a pointer to it):
 * a mis-typed extern would read r_version+padding as an address. */
extern struct r_debug_head _r_debug __attribute__((weak));
extern ElfW(Dyn) _DYNAMIC[];

/* Number of .dynsym entries derivable from a DT_GNU_HASH table. */
static size_t gnu_hash_nsyms(const uint32_t *h)
{
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
    const uint32_t *buckets =
        (const uint32_t *)((const char *)(h + 4) +
                           (size_t)bloom_size * sizeof(ElfW(Addr)));
    const uint32_t *chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++)
        if (buckets[i] > last)
            last = buckets[i];
    if (last < symoffset)
        return symoffset;
    for (size_t guard = 0; guard < 1u << 20; guard++) {
        if (chain[last - symoffset] & 1u)
            return (size_t)last + 1;
        last++;
    }
    return symoffset;
}

/* Look up a defined STT_FUNC symbol in one loaded object. */
static void *lookup_in(struct link_map_head *m, const char *name)
{
    ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    const uint32_t *hash = NULL, *gnuhash = NULL;

    for (ElfW(Dyn) *d = m->l_ld; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SYMTAB) symtab = (ElfW(Sym) *)d->d_un.d_ptr;
        else if (d->d_tag == DT_STRTAB) strtab = (const char *)d->d_un.d_ptr;
        else if (d->d_tag == DT_HASH) hash = (const uint32_t *)d->d_un.d_ptr;
        else if (d->d_tag == DT_GNU_HASH) gnuhash = (const uint32_t *)d->d_un.d_ptr;
    }
    if (symtab == NULL || strtab == NULL)
        return NULL;

    size_t nsyms = 0;
    if (hash != NULL)
        nsyms = hash[1]; /* DT_HASH: nchain == number of symbols */
    else if (gnuhash != NULL)
        nsyms = gnu_hash_nsyms(gnuhash);
    else
        return NULL;

    for (size_t i = 0; i < nsyms; i++) {
        if (symtab[i].st_name == 0 || symtab[i].st_value == 0 ||
            symtab[i].st_shndx == SHN_UNDEF)
            continue;
        if (ELF_ST_TYPE(symtab[i].st_info) != STT_FUNC)
            continue;
        if (strcmp(strtab + symtab[i].st_name, name) == 0)
            return (void *)(m->l_addr + symtab[i].st_value);
    }
    return NULL;
}

/* dlsym(RTLD_NEXT, name) equivalent: find our own object in the
 * link_map (its l_ld is our _DYNAMIC), then search every later object. */
static void *resolve_next(const char *name)
{
    if ((ElfW(Addr))&_r_debug == 0 || _r_debug.r_map == NULL)
        return NULL;
    for (struct link_map_head *m = _r_debug.r_map; m != NULL; m = m->l_next) {
        if ((ElfW(Addr))m->l_ld == (ElfW(Addr))_DYNAMIC) {
            for (m = m->l_next; m != NULL; m = m->l_next) {
                void *p = lookup_in(m, name);
                if (p != NULL)
                    return p;
            }
            break;
        }
    }
    return NULL;
}

/* Resolve the real function. Uses only write() on failure so the .so
 * can stay free of libc/stdout data-symbol dependencies and be usable
 * from both glibc and musl processes. */
static void *real(const char *name)
{
    void *h = NULL;
    if (dlsym != NULL)
        h = dlsym(RTLD_NEXT, name);
    else
        h = resolve_next(name);
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
