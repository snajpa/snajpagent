/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "office.h"
#include "media.h"
#include "fs.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#if SNAJPAGENT_OFFICE
static int office_setenv(const char *name,const char *value)
{
#ifdef _WIN32
    wchar_t *key=snag_utf8_to_wide(name),*wide=value?snag_utf8_to_wide(value):NULL;
    struct snag_buf entry;snag_buf_init(&entry,SNAG_PATH_MAX_BYTES+64u);
    wchar_t *pair=NULL;int rc=-1;
    if(!key || (value && !wide) || snag_buf_printf(&entry,"%s=%s",name,value?value:"")<0 ||
        snag_buf_terminate(&entry)<0 || !(pair=snag_utf8_to_wide((char *)entry.data)))goto out;
    /* The CRT table and native environment can be consumed independently. */
    if(!_wputenv(pair)) {
        if(SetEnvironmentVariableW(key,wide) || (!value && GetLastError()==ERROR_ENVVAR_NOT_FOUND))rc=0;
    }
out:
    free(key);free(wide);free(pair);snag_buf_free(&entry);return rc;
#else
    if(value)return setenv(name,value,1);
    (void)unsetenv(name); /* void return on legacy BSD. */
    return getenv(name)?-1:0;
#endif
}

static int office_environment(const char *dir)
{
    char **entries=snag_environment_entries();
    if(!entries)return -1;
    int rc=0;
    for(size_t i=0;entries[i] && !rc;++i) {
#ifdef _WIN32
        /* Keep native system location and hidden drive-current-directory entries. */
        if(entries[i][0]=='=' || snag_environment_prefix(entries[i],"SystemRoot=") ||
            snag_environment_prefix(entries[i],"WINDIR="))continue;
#endif
        char *eq=strchr(entries[i],'=');
        if(eq) {*eq=0;rc=office_setenv(entries[i],NULL);}
    }
    snag_environment_entries_free(entries);
    if(rc || office_setenv("HOME",dir) || office_setenv("TMPDIR",dir) ||
        office_setenv("TMP",dir) || office_setenv("TEMP",dir) ||
        office_setenv("LC_ALL","C") || office_setenv("TZ","UTC") ||
        office_setenv("SAL_USE_VCLPLUGIN","svp") || office_setenv("SAL_DISABLE_OPENCL","1") ||
        office_setenv("LOK_HOST_ALLOWLIST","a^") || office_setenv("SAL_LOG","-WARN"))return -1;
    return 0;
}
#endif

#if SNAJPAGENT_OFFICE && !defined(_WIN32)
#include <fcntl.h>
#include <sys/resource.h>
#include <signal.h>
#if defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#endif

int
snag_office_worker_limits(const char *dir,char *error,size_t size)
{
    struct rlimit cpu={60u,60u},memory={2ull<<30,2ull<<30};
    struct rlimit file={32u<<20,32u<<20},core={0u,0u};
    struct snag_file_privacy privacy;
    struct snag_directory_lock lock={.fd=-1};
    /* Also enforced when invoked directly, independently of the parent runner. */
#if defined(__linux__)
    pid_t parent_pid=getppid();
    if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent_pid)goto failed;
#endif
    if(setrlimit(RLIMIT_CPU,&cpu) ||
#if defined(__APPLE__) || !defined(RLIMIT_AS)
        setrlimit(RLIMIT_DATA,&memory) ||
#else
        setrlimit(RLIMIT_AS,&memory) ||
#endif
        setrlimit(RLIMIT_FSIZE,&file) || setrlimit(RLIMIT_CORE,&core))goto failed;
    if(signal(SIGALRM,SIG_DFL)==SIG_ERR)goto failed;
    alarm(60u);
    int work_fd=snag_open_read_at(AT_FDCWD,".",true);
    if(work_fd<0 || snag_fd_privacy(work_fd,&privacy)<0 ||
        !privacy.effective_owner || !privacy.private_access ||
        snag_directory_lock_acquire(work_fd,&lock)<0)goto failed;
    if(office_environment(dir))goto failed;
    return 0;
failed:
    snag_errorf(error,size,"Office worker limits/private directory failed: %s",strerror(errno));return -1;
}

#if defined(__linux__)
static int
allow_path(int rule, const char *path, uint64_t rights, bool required)
{
    struct landlock_path_beneath_attr entry = {.allowed_access = rights};
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return !required && errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    if (!S_ISDIR(st.st_mode)) entry.allowed_access &= LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;
    entry.parent_fd = fd;
    int rc = (int)syscall(SYS_landlock_add_rule, rule, LANDLOCK_RULE_PATH_BENEATH, &entry, 0);
    close(fd); return rc;
}

int
snag_office_confine(const char *workdir, const char *runtime, const char *input, char *error, size_t size)
{
    int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    /* Additional filesystem defense is opportunistic; package rejection,
     * macro policy, network/exec denial and worker limits remain mandatory. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) goto failed;
    if (abi >= 1) {
    uint64_t read = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    uint64_t write = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi < 3) write &= ~LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi < 2) write &= ~LANDLOCK_ACCESS_FS_REFER;
    struct landlock_ruleset_attr rules = {.handled_access_fs = (1u << (abi < 2 ? 13 : abi < 3 ? 14 : 15)) - 1u};
    int rule = (int)syscall(SYS_landlock_create_ruleset, &rules, sizeof(rules), 0);
    if (rule < 0) goto failed;
    int rc = allow_path(rule, workdir, read | write, true);
    if (!rc) rc = allow_path(rule, runtime, read, true);
    if (!rc) rc = allow_path(rule, input, read, true);
    /* Runtime closure and fonts only. No workspace, home, credentials, or /tmp.
     * Nix store is read-only package content on this native development route. */
    const char *paths[] = {"/usr/lib", "/usr/lib64", "/lib", "/lib64", "/nix/store",
        "/usr/share/fonts", "/usr/share/fontconfig", "/usr/share/locale", "/usr/share/zoneinfo",
        "/etc/fonts", "/etc/ld.so.cache", "/etc/localtime", "/dev/urandom", "/dev/random", "/proc/self/maps"};
    for (size_t i = 0; !rc && i < sizeof(paths) / sizeof(paths[0]); ++i)
        rc = allow_path(rule, paths[i], read, false);
    if (!rc) rc = allow_path(rule, "/dev/null", read | LANDLOCK_ACCESS_FS_WRITE_FILE, true);
    if (!rc) rc = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (!rc) rc = (int)syscall(SYS_landlock_restrict_self, rule, 0);
    close(rule);
    if (rc) goto failed;
    }
#if defined(__x86_64__)
#define OFFICE_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define OFFICE_ARCH AUDIT_ARCH_AARCH64
#elif defined(__i386__)
#define OFFICE_ARCH AUDIT_ARCH_I386
#else
    errno = ENOTSUP; goto failed;
#endif
#ifdef OFFICE_ARCH
#define DENY(call) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_##call, 0, 1), BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM)
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, OFFICE_ARCH, 1, 0),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),
#ifdef __x86_64__
        /* x32 syscall numbers are not the x86_64 allow path. */
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K, 0x40000000u, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM),
#endif
        DENY(socket), DENY(connect), DENY(bind), DENY(listen), DENY(accept4),
#ifdef SYS_socketcall
        DENY(socketcall),
#endif
        DENY(execve), DENY(execveat), DENY(ptrace), DENY(process_vm_readv), DENY(process_vm_writev),
        DENY(mount), DENY(umount2), DENY(unshare), DENY(setns), DENY(bpf), DENY(open_by_handle_at),
        DENY(io_uring_setup), DENY(kill),
        DENY(chown), DENY(fchown), DENY(fchownat), DENY(linkat), DENY(symlinkat),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)
    };
#undef DENY
    struct sock_fprog program = {.len = (unsigned short)(sizeof(filter) / sizeof(filter[0])), .filter = filter};
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) < 0) goto failed;
    (void)snprintf(error, size, "Network/exec syscalls denied; filesystem confinement %s",
        abi < 1 ? "unavailable on this host" : "active");
    return abi < 1 ? 1 : 0;
#endif
failed:
    snag_errorf(error, size, "Office confinement failed: %s", strerror(errno)); return -1;
}
#else
int snag_office_confine(const char *dir, const char *runtime, const char *input, char *error, size_t size)
{
    (void)dir; (void)runtime; (void)input;
    (void)snprintf(error,size,"OS filesystem/network/exec confinement unavailable; package checks and worker limits active");
    return 1;
}
#endif
#elif SNAJPAGENT_OFFICE && defined(_WIN32)
static void CALLBACK office_timeout(void *opaque,BOOLEAN fired)
{
    (void)opaque;(void)fired;
    (void)TerminateProcess(GetCurrentProcess(),124u);
}

int snag_office_worker_limits(const char *dir,char *error,size_t size)
{
    HANDLE job=NULL,timer=NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    DWORD required=JOB_OBJECT_LIMIT_JOB_MEMORY|JOB_OBJECT_LIMIT_JOB_TIME;
    if(!QueryInformationJobObject(NULL,JobObjectExtendedLimitInformation,&limits,sizeof(limits),NULL)) {
        job=CreateJobObjectW(NULL,NULL);
        limits.BasicLimitInformation.LimitFlags=required;
        limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart=60ll*10000000ll;
        limits.JobMemoryLimit=(SIZE_T)2u<<30;
        if(!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) ||
            !AssignProcessToJobObject(job,GetCurrentProcess()))goto failed;
        /* Own handle remains open for this disposable worker's lifetime. */
    }
    if((limits.BasicLimitInformation.LimitFlags&required)!=required ||
        limits.JobMemoryLimit>((SIZE_T)2u<<30) ||
        limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart>60ll*10000000ll)goto failed;
    if(!CreateTimerQueueTimer(&timer,NULL,office_timeout,NULL,60000u,0u,WT_EXECUTEONLYONCE))goto failed;
    struct snag_file_privacy privacy;
    struct snag_directory_lock lock={.fd=-1};
    int fd=snag_open_read(".",true);
    if(fd<0 || snag_fd_privacy(fd,&privacy)<0 || !privacy.effective_owner || !privacy.private_access ||
        snag_directory_lock_acquire(fd,&lock)<0 || office_environment(dir))goto failed;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    return 0;
failed:
    snag_errorf(error,size,"Office worker job limits/private directory failed (Windows %lu, errno %d)",
        (unsigned long)GetLastError(),errno);return -1;
}

int snag_office_confine(const char *dir,const char *runtime,const char *input,char *error,size_t size)
{
    (void)dir;(void)runtime;(void)input;
    (void)snprintf(error,size,"OS filesystem/network/exec confinement unavailable; package checks and job limits active");
    return 1;
}
#else
int snag_office_worker_limits(const char *dir,char *error,size_t size)
{
    (void)dir;snag_errorf(error,size,"Office worker limits are not implemented for this target yet");return -1;
}
int snag_office_confine(const char *dir, const char *runtime, const char *input, char *error, size_t size)
{
    (void)dir; (void)runtime; (void)input;
    snag_errorf(error, size, "Office confinement is not implemented for this target yet"); return -1;
}
#endif
