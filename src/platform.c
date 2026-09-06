/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "fs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <wincrypt.h>
#include <winternl.h>
#include <io.h>
#include <fcntl.h>
#include <uniwidth.h>
#include <wchar.h>

static int
path_error(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND:
        errno = ENOENT; break;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS:
        errno = EEXIST; break;
    case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY; break;
    case ERROR_NOT_SAME_DEVICE:
        errno = EXDEV; break;
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
        errno = EBUSY; break;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD:
        errno = EACCES; break;
    case ERROR_INVALID_HANDLE:
        errno = EBADF; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY:
        errno = ENOMEM; break;
    case ERROR_FILENAME_EXCED_RANGE:
        errno = ENAMETOOLONG; break;
    case ERROR_NO_UNICODE_TRANSLATION:
        errno = EILSEQ; break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR; break;
    case ERROR_NOT_SUPPORTED: case ERROR_INVALID_FUNCTION:
        errno = ENOTSUP; break;
    case ERROR_CANT_RESOLVE_FILENAME:
        errno = ELOOP; break;
    case ERROR_INVALID_NAME: case ERROR_INVALID_PARAMETER:
        errno = EINVAL; break;
    default:
        errno = EIO; break;
    }
    return -1;
}

static TOKEN_USER *
token_user(HANDLE token)
{
    DWORD size = 0;
    TOKEN_USER *user;

    (void)GetTokenInformation(token, TokenUser, NULL, 0, &size);
    if (!size) {
        path_error(GetLastError());
        return NULL;
    }
    user = malloc(size);
    if (user && !GetTokenInformation(token, TokenUser, user, size, &size)) {
        DWORD error = GetLastError();
        free(user);
        path_error(error);
        return NULL;
    }
    return user;
}

static wchar_t *
wide_path(const char *path)
{
    wchar_t *wide;
    size_t len;
    int chars;

    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    len = strlen(path);
    if (len > SNAG_PATH_MAX_BYTES) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    if (!snag_utf8_valid((const unsigned char *)path, len, true)) {
        errno = EILSEQ;
        return NULL;
    }
    chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!chars) {
        path_error(GetLastError());
        return NULL;
    }
    wide = malloc((size_t)chars * sizeof(*wide));
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, chars)) {
        DWORD error = GetLastError();
        free(wide);
        path_error(error);
        return NULL;
    }
    return wide;
}

static bool private_dacl(PACL dacl, PSID owner);

struct nt_path {
    wchar_t *wide;
    UNICODE_STRING name;
    HANDLE parent;
    bool allocated_name;
};

static void
nt_path_free(struct nt_path *path)
{
    if (path->allocated_name)
        RtlFreeUnicodeString(&path->name);
    free(path->wide);
}

static int
nt_path_init(struct nt_path *out, int dirfd, const char *path, bool relative)
{
    memset(out, 0, sizeof(*out));
    out->wide = wide_path(path);
    if (!out->wide)
        return -1;
    if (!*out->wide) {
        errno = ENOENT;
        return -1;
    }
    if (relative && !snag_path_root_len(path)) {
        BY_HANDLE_FILE_INFORMATION info;
        intptr_t parent = _get_osfhandle(dirfd);
        if (parent == -1) {
            errno = EBADF;
            return -1;
        }
        if (!GetFileInformationByHandle((HANDLE)parent, &info))
            return path_error(GetLastError());
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            errno = ENOTDIR;
            return -1;
        }
        if (*out->wide == L'/' || *out->wide == L'\\') {
            errno = EINVAL;
            return -1;
        }
        for (wchar_t *p = out->wide; *p; ++p)
            if (*p == L'/')
                *p = L'\\';
        out->name.Buffer = out->wide;
        out->name.Length = (USHORT)(wcslen(out->wide) * sizeof(*out->wide));
        out->name.MaximumLength = out->name.Length + sizeof(*out->wide);
        out->parent = (HANDLE)parent;
    } else {
        if (!(BOOLEAN)RtlDosPathNameToNtPathName_U(out->wide, &out->name, NULL, NULL)) {
            errno = EINVAL;
            return -1;
        }
        out->allocated_name = true;
    }
    return 0;
}

static HANDLE
nt_open(const struct nt_path *path, DWORD access, DWORD options)
{
    HANDLE handle = NULL;
    OBJECT_ATTRIBUTES attributes = {0};
    IO_STATUS_BLOCK io;

    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = path->parent;
    attributes.ObjectName = (UNICODE_STRING *)&path->name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    NTSTATUS status = NtCreateFile(&handle, access | SYNCHRONIZE, &attributes, &io, NULL, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | options, NULL, 0);
    if (status < 0) {
        path_error(RtlNtStatusToDosError(status));
        return NULL;
    }
    return handle;
}

static int
file_info(HANDLE handle, snag_file_info *out)
{
    BY_HANDLE_FILE_INFORMATION info;
    snag_file_info st = {0};
    DWORD type;

    if (!out) {
        errno = EINVAL;
        return -1;
    }
    SetLastError(ERROR_SUCCESS);
    type = GetFileType(handle);
    if (type == FILE_TYPE_UNKNOWN) {
        DWORD code = GetLastError();
        return path_error(code ? code : ERROR_NOT_SUPPORTED);
    }
    if (type == FILE_TYPE_DISK) {
        if (!GetFileInformationByHandle(handle, &info))
            return path_error(GetLastError());
        st.st_dev = info.dwVolumeSerialNumber;
        st.st_ino = ((uint64_t)info.nFileIndexHigh << 32u) | info.nFileIndexLow;
        st.st_nlink = info.nNumberOfLinks;
        st.st_size = (int64_t)(((uint64_t)info.nFileSizeHigh << 32u) | info.nFileSizeLow);
        uint64_t ticks = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32u) |
                         info.ftLastWriteTime.dwLowDateTime;
        st.st_mtime = (int64_t)(ticks / 10000000u) - INT64_C(11644473600);
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            st.st_mode = S_IFLNK;
        else if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            st.st_mode = S_IFDIR;
        else
            st.st_mode = S_IFREG;
        /* Attribute bits are not a Unix permission or Windows ACL snapshot. */
        st.st_mode |= 0400u;
        if (S_ISDIR(st.st_mode))
            st.st_mode |= 0100u;
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
            st.st_mode |= 0200u;
    } else if (type == FILE_TYPE_CHAR) {
        st.st_mode = S_IFCHR;
        st.st_rdev = (uint64_t)(uintptr_t)handle;
    } else if (type == FILE_TYPE_PIPE) {
        st.st_mode = S_IFIFO;
    } else {
        return path_error(ERROR_NOT_SUPPORTED);
    }
    *out = st;
    return 0;
}

int
snag_fstat(int fd, snag_file_info *out)
{
    intptr_t handle = _get_osfhandle(fd);

    if (handle == -1) {
        errno = EBADF;
        return -1;
    }
    return file_info((HANDLE)handle, out);
}

static int
path_stat(int dirfd, const char *path, bool relative, bool nofollow, snag_file_info *out)
{
    HANDLE handle = NULL;
    struct nt_path name;
    int rc = -1, error;

    if (nt_path_init(&name, dirfd, path, relative) < 0)
        goto out;
    handle = nt_open(&name, FILE_READ_ATTRIBUTES, nofollow ? FILE_OPEN_REPARSE_POINT : 0);
    if (!handle)
        goto out;
    rc = file_info(handle, out);
out:
    error = errno;
    if (handle && !CloseHandle(handle) && rc == 0) {
        path_error(GetLastError());
        error = errno;
        rc = -1;
    }
    nt_path_free(&name);
    errno = error;
    return rc;
}

int
snag_stat(const char *path, snag_file_info *out)
{
    return path_stat(-1, path, false, false, out);
}

int
snag_lstat(const char *path, snag_file_info *out)
{
    return path_stat(-1, path, false, true, out);
}

int
snag_lstat_at(int dirfd, const char *path, snag_file_info *out)
{
    return path_stat(dirfd, path, true, true, out);
}

int
snag_unlink_at(int dirfd, const char *path, bool directory)
{
    struct nt_path name;
    HANDLE handle = NULL;
    snag_file_info info;
    FILE_DISPOSITION_INFORMATION dispose = {TRUE};
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    int rc = -1, error;

    if (nt_path_init(&name, dirfd, path, true) < 0)
        goto out;
    handle = nt_open(&name, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN_REPARSE_POINT);
    if (!handle || file_info(handle, &info) < 0)
        goto out;
    if (directory != (S_ISDIR(info.st_mode) != 0)) {
        errno = directory ? ENOTDIR : EISDIR;
        goto out;
    }
    status = NtSetInformationFile(handle, &io, &dispose, sizeof(dispose), FileDispositionInformation);
    rc = status < 0 ? path_error(RtlNtStatusToDosError(status)) : 0;
out:
    error = errno;
    if (handle && !CloseHandle(handle) && rc == 0) {
        path_error(GetLastError());
        error = errno;
        rc = -1;
    }
    nt_path_free(&name);
    errno = error;
    return rc;
}

int
snag_rename_at(int from_dir, const char *from, int to_dir, const char *to)
{
    struct nt_path source, target = {0};
    HANDLE handle = NULL;
    FILE_RENAME_INFORMATION *rename = NULL;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    size_t bytes;
    int rc = -1, error;

    if (nt_path_init(&source, from_dir, from, true) < 0 ||
        nt_path_init(&target, to_dir, to, true) < 0)
        goto out;
    handle = nt_open(&source, DELETE, FILE_OPEN_REPARSE_POINT);
    if (!handle)
        goto out;
    bytes = offsetof(FILE_RENAME_INFORMATION, FileName) + target.name.Length;
    rename = calloc(1u, bytes);
    if (!rename)
        goto out;
    rename->ReplaceIfExists = TRUE;
    rename->RootDirectory = target.parent;
    rename->FileNameLength = target.name.Length;
    memcpy(rename->FileName, target.name.Buffer, target.name.Length);
    status = NtSetInformationFile(handle, &io, rename, (ULONG)bytes, FileRenameInformation);
    rc = status < 0 ? path_error(RtlNtStatusToDosError(status)) : 0;
out:
    error = errno;
    if (handle && !CloseHandle(handle) && rc == 0) {
        path_error(GetLastError());
        error = errno;
        rc = -1;
    }
    free(rename);
    nt_path_free(&source);
    nt_path_free(&target);
    errno = error;
    return rc;
}

static int
open_read(int dirfd, const char *path, bool relative, bool directory, bool security)
{
    struct nt_path name;
    HANDLE handle = NULL;
    snag_file_info info;
    int fd = -1, error;
    size_t root = snag_path_root_len(path);

    if (!path || ((!root && (path[0] == '/' || path[0] == '\\')) ||
                  strchr(path + root, ':'))) {
        errno = EINVAL;
        return -1;
    }
    if (nt_path_init(&name, dirfd, path, relative) < 0)
        goto out;
    handle = nt_open(&name, FILE_READ_DATA | FILE_READ_ATTRIBUTES | (security ? READ_CONTROL : 0), FILE_OPEN_REPARSE_POINT |
                     (directory ? FILE_DIRECTORY_FILE : 0));
    if (!handle || file_info(handle, &info) < 0)
        goto out;
    if (S_ISLNK(info.st_mode)) {
        errno = ELOOP;
        goto out;
    }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
        errno = EACCES;
        goto out;
    }
    fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (fd >= 0)
        handle = NULL;
out:
    error = errno;
    if (handle)
        (void)CloseHandle(handle);
    nt_path_free(&name);
    errno = error;
    return fd;
}

int
snag_open_read(const char *path, bool directory)
{
    return open_read(-1, path, false, directory, false);
}

int
snag_open_read_at(int dirfd, const char *path, bool directory)
{
    return open_read(dirfd, path, true, directory, false);
}

int
snag_open_private_dir_at(int dirfd, const char *path)
{
    return open_read(dirfd, path, true, true, true);
}

void
snag_path_slashes(char *path)
{
    for (; *path; ++path)
        if (*path == '\\')
            *path = '/';
}

/* This native ABI predates the convenience Win32 directory-handle API. */
NTSYSAPI NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
    PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);

struct snag_directory {
    int fd;
    bool started, finished;
    char name[1024];
};

struct snag_directory *
snag_directory_open(int fd)
{
    snag_file_info info;
    struct snag_directory *dir;

    if (snag_fstat(fd, &info) < 0)
        return NULL;
    if (!S_ISDIR(info.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    dir = calloc(1u, sizeof(*dir));
    if (dir)
        dir->fd = fd;
    return dir;
}

const char *
snag_directory_next(struct snag_directory *dir)
{
    union {
        FILE_NAMES_INFORMATION align;
        unsigned char bytes[2048];
    } record;
    FILE_NAMES_INFORMATION *info = &record.align;
    IO_STATUS_BLOCK io = {0};
    NTSTATUS status;
    int bytes;
    size_t header = offsetof(FILE_NAMES_INFORMATION, FileName);

    if (!dir) {
        errno = EINVAL;
        return NULL;
    }
    errno = 0;
    if (dir->finished)
        return NULL;
    status = NtQueryDirectoryFile((HANDLE)_get_osfhandle(dir->fd), NULL, NULL, NULL,
        &io, &record, sizeof(record), FileNamesInformation, TRUE, NULL, !dir->started);
    dir->started = true;
    if (status == (NTSTATUS)0x80000006u) { /* STATUS_NO_MORE_FILES */
        dir->finished = true;
        return NULL;
    }
    if (status < 0) {
        path_error(RtlNtStatusToDosError(status));
        return NULL;
    }
    if (io.Information < header || io.Information > sizeof(record) || !info->FileNameLength ||
        info->FileNameLength > io.Information - header || info->FileNameLength % sizeof(WCHAR)) {
        errno = EIO;
        return NULL;
    }
    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, info->FileName,
        (int)(info->FileNameLength / sizeof(WCHAR)), dir->name, sizeof(dir->name) - 1u, NULL, NULL);
    if (!bytes) {
        DWORD code = GetLastError();
        if (code == ERROR_INSUFFICIENT_BUFFER)
            errno = ENAMETOOLONG;
        else
            path_error(code);
        return NULL;
    }
    if (memchr(dir->name, '\0', (size_t)bytes)) {
        errno = EILSEQ;
        return NULL;
    }
    dir->name[bytes] = '\0';
    return dir->name;
}

int
snag_directory_close(struct snag_directory *dir)
{
    int rc;

    if (!dir) {
        errno = EINVAL;
        return -1;
    }
    rc = _close(dir->fd);
    free(dir);
    return rc;
}

static int
create_private(int dirfd, const char *path, bool relative, bool directory,
               bool exclusive, int *file_fd)
{
    HANDLE token = NULL, created = NULL;
    TOKEN_USER *user = NULL;
    SECURITY_DESCRIPTOR descriptor;
    PACL acl = NULL;
    struct nt_path name;
    OBJECT_ATTRIBUTES attributes = {0};
    IO_STATUS_BLOCK io = {0};
    DWORD acl_size, close_error = 0;
    bool was_created = false;
    NTSTATUS status;
    int rc = -1, error;

    if (nt_path_init(&name, dirfd, path, relative) < 0)
        goto out;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        if (GetLastError() != ERROR_NO_TOKEN ||
            !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            goto native_error;
    }
    if (!(user = token_user(token)))
        goto out;
    acl_size = (DWORD)(sizeof(ACL) + offsetof(ACCESS_ALLOWED_ACE, SidStart)) +
               GetLengthSid(user->User.Sid);
    acl = malloc(acl_size);
    if (!acl)
        goto out;
    if (!InitializeAcl(acl, acl_size, ACL_REVISION) ||
        !AddAccessAllowedAceEx(acl, ACL_REVISION, CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE,
                               FILE_ALL_ACCESS, user->User.Sid) ||
        !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&descriptor, user->User.Sid, FALSE) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        goto native_error;
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = name.parent;
    attributes.ObjectName = &name.name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    attributes.SecurityDescriptor = &descriptor;
    DWORD access = FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
    if (!directory)
        access |= FILE_GENERIC_READ | FILE_GENERIC_WRITE;
    DWORD options = (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE) |
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT;
    status = NtCreateFile(&created, access | DELETE,
                          &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_CREATE, options, NULL, 0);
    if (!exclusive && status < 0) {
        DWORD code = RtlNtStatusToDosError(status);
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            created = NULL;
            status = NtCreateFile(&created, access, &attributes, &io, NULL,
                FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                FILE_OPEN, options, NULL, 0);
        }
    }
    if (status < 0) {
        created = NULL;
        path_error(RtlNtStatusToDosError(status));
        goto out;
    }
    was_created = io.Information == FILE_CREATED;
    {
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(created, &info))
            goto native_error;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            errno = ELOOP;
            goto out;
        }
        if (!directory && info.nNumberOfLinks != 1u) {
            errno = EACCES;
            goto out;
        }
    }
    {
        PSECURITY_DESCRIPTOR actual = NULL;
        PSID actual_owner = NULL;
        PACL actual_dacl = NULL;
        DWORD code = GetSecurityInfo(created, SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &actual_owner, NULL, &actual_dacl, NULL, &actual);
        bool valid = code == ERROR_SUCCESS && actual_owner && IsValidSid(actual_owner) &&
            EqualSid(actual_owner, user->User.Sid) && private_dacl(actual_dacl, actual_owner);
        if (actual)
            LocalFree(actual);
        if (!valid) {
            if (code != ERROR_SUCCESS)
                path_error(code);
            else
                errno = EACCES;
            goto out;
        }
    }
    if (!directory) {
        *file_fd = _open_osfhandle((intptr_t)created, _O_BINARY | _O_RDWR | _O_NOINHERIT);
        if (*file_fd < 0)
            goto out;
        created = NULL; /* CRT descriptor now owns the handle. */
    }
    rc = 0;
    goto out;
native_error:
    path_error(GetLastError());
out:
    error = errno;
    if (rc < 0 && created && was_created) {
        FILE_DISPOSITION_INFORMATION dispose = {TRUE};
        (void)NtSetInformationFile(created, &io, &dispose, sizeof(dispose),
                                   FileDispositionInformation);
    }
    if (created && !CloseHandle(created))
        close_error = GetLastError();
    if (token && !CloseHandle(token) && !close_error)
        close_error = GetLastError();
    free(user);
    free(acl);
    nt_path_free(&name);
    errno = error;
    if (rc == 0 && close_error) {
        if (file_fd && *file_fd >= 0) {
            (void)_close(*file_fd);
            *file_fd = -1;
        }
        return path_error(close_error);
    }
    return rc;
}

int
snag_mkdir_private(const char *path)
{
    return create_private(-1, path, false, true, true, NULL);
}

int
snag_mkdir_private_at(int dirfd, const char *path)
{
    return create_private(dirfd, path, true, true, true, NULL);
}

int
snag_create_private_at(int dirfd, const char *path, bool exclusive)
{
    int fd = -1;

    return create_private(dirfd, path, true, false, exclusive, &fd) < 0 ? -1 : fd;
}

static bool
private_dacl(PACL dacl, PSID owner)
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    DWORD system_sid[SECURITY_MAX_SID_SIZE / sizeof(DWORD) + 1u];
    DWORD admin_sid[SECURITY_MAX_SID_SIZE / sizeof(DWORD) + 1u];
    GENERIC_MAPPING mapping = {
        FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS
    };

    if (!dacl || !IsValidAcl(dacl) || !owner || !IsValidSid(owner) ||
        !InitializeSid(system_sid, &authority, 1u) ||
        !InitializeSid(admin_sid, &authority, 2u))
        return false;
    *GetSidSubAuthority(system_sid, 0u) = SECURITY_LOCAL_SYSTEM_RID;
    *GetSidSubAuthority(admin_sid, 0u) = SECURITY_BUILTIN_DOMAIN_RID;
    *GetSidSubAuthority(admin_sid, 1u) = DOMAIN_ALIAS_RID_ADMINS;
    for (DWORD i = 0u; i < dacl->AceCount; ++i) {
        ACE_HEADER *header;
        ACCESS_ALLOWED_ACE *ace;
        PSID sid;
        DWORD mask;
        size_t offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);

        if (!GetAce(dacl, i, (void **)&header))
            return false;
        if ((header->AceFlags & INHERIT_ONLY_ACE) ||
            header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            header->AceSize < offset + 8u)
            return false;
        ace = (ACCESS_ALLOWED_ACE *)header;
        mask = ace->Mask;
        MapGenericMask(&mask, &mapping);
        if (!(mask & ~(READ_CONTROL | SYNCHRONIZE | FILE_READ_ATTRIBUTES)))
            continue;
        sid = &ace->SidStart;
        if (GetSidLengthRequired(*GetSidSubAuthorityCount(sid)) > header->AceSize - offset ||
            !IsValidSid(sid))
            return false;
        if (!EqualSid(sid, owner) && !EqualSid(sid, system_sid) && !EqualSid(sid, admin_sid))
            return false;
    }
    return true;
}

int
snag_fd_privacy(int fd, struct snag_file_privacy *out)
{
    struct snag_file_privacy privacy = {0};
    HANDLE process = NULL, thread = NULL;
    TOKEN_USER *real = NULL, *effective = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PSID owner = NULL;
    PACL dacl = NULL;
    intptr_t handle;
    DWORD code, close_error = 0;
    int rc = -1, error;

    if (!out) {
        errno = EINVAL;
        return -1;
    }
    handle = _get_osfhandle(fd);
    if (handle == -1) {
        errno = EBADF;
        return -1;
    }
    code = GetSecurityInfo((HANDLE)handle, SE_FILE_OBJECT,
                           OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                           &owner, NULL, &dacl, NULL, &descriptor);
    if (code != ERROR_SUCCESS) {
        path_error(code);
        goto out;
    }
    if (!owner || !IsValidSid(owner)) {
        errno = EACCES;
        goto out;
    }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process)) {
        path_error(GetLastError());
        goto out;
    }
    if (!(real = token_user(process)))
        goto out;
    privacy.real_owner = EqualSid(owner, real->User.Sid) != 0;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread)) {
        if (!(effective = token_user(thread)))
            goto out;
        privacy.effective_owner = EqualSid(owner, effective->User.Sid) != 0;
    } else if (GetLastError() == ERROR_NO_TOKEN) {
        privacy.effective_owner = privacy.real_owner;
    } else {
        path_error(GetLastError());
        goto out;
    }
    privacy.private_access = private_dacl(dacl, owner);
    rc = 0;
out:
    error = errno;
    if (thread && !CloseHandle(thread))
        close_error = GetLastError();
    if (process && !CloseHandle(process) && !close_error)
        close_error = GetLastError();
    free(real);
    free(effective);
    if (descriptor)
        LocalFree(descriptor);
    errno = error;
    if (rc == 0 && close_error)
        return path_error(close_error);
    if (rc == 0)
        *out = privacy;
    return rc;
}

char *
snag_realpath(const char *path)
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    wchar_t *wide = wide_path(path), *final = NULL;
    const wchar_t *body;
    char *result = NULL;
    size_t prefix = 0u;
    DWORD capacity, got;
    int bytes, error;

    if (!wide)
        return NULL;
    handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        goto native_error;
    capacity = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    if (!capacity)
        goto native_error;
    if (capacity > SNAG_PATH_MAX_BYTES + 9u) {
        errno = ENAMETOOLONG;
        goto out;
    }
    final = malloc(((size_t)capacity + 1u) * sizeof(*final));
    if (!final)
        goto out;
    got = GetFinalPathNameByHandleW(handle, final, capacity + 1u, FILE_NAME_NORMALIZED);
    if (!got)
        goto native_error;
    if (got > capacity) {
        errno = ENAMETOOLONG;
        goto out;
    }
    body = final;
    if (wcsncmp(body, L"\\\\?\\UNC\\", 8u) == 0) {
        body += 8u;
        prefix = 2u;
    } else if (wcsncmp(body, L"\\\\?\\", 4u) == 0) {
        body += 4u;
    }
    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, body, -1,
                                NULL, 0, NULL, NULL);
    if (!bytes)
        goto native_error;
    if ((size_t)bytes + prefix > SNAG_PATH_MAX_BYTES + 1u) {
        errno = ENAMETOOLONG;
        goto out;
    }
    result = malloc((size_t)bytes + prefix);
    if (!result)
        goto out;
    if (prefix)
        result[0] = result[1] = '/';
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, body, -1,
                             result + prefix, bytes, NULL, NULL)) {
        DWORD native_error = GetLastError();
        free(result);
        result = NULL;
        path_error(native_error);
        goto out;
    }
    for (char *p = result; *p; ++p)
        if (*p == '\\')
            *p = '/';
    if (!snag_path_root_len(result)) {
        free(result);
        result = NULL;
        errno = EIO;
    }
    goto out;
native_error:
    path_error(GetLastError());
out:
    error = errno;
    if (handle != INVALID_HANDLE_VALUE && !CloseHandle(handle) && result) {
        DWORD native_error = GetLastError();
        free(result);
        result = NULL;
        path_error(native_error);
        error = errno;
    }
    free(final);
    free(wide);
    errno = error;
    return result;
}

static bool
path_separator(unsigned char c)
{
    return c == '/' || c == '\\';
}

size_t
snag_path_root_len(const char *path)
{
    const char *p;

    if (!path || !*path)
        return 0u;
    if (((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && path_separator((unsigned char)path[2]))
        return 3u;
    if (!path_separator((unsigned char)path[0]) ||
        !path_separator((unsigned char)path[1]) || !path[2] ||
        path_separator((unsigned char)path[2]))
        return 0u;
    /* Device namespaces are not ordinary UNC server/share roots. */
    if ((path[2] == '.' || path[2] == '?') &&
        path_separator((unsigned char)path[3]))
        return 0u;
    p = path + 2u;
    while (*p && !path_separator((unsigned char)*p))
        ++p;
    if (!*p || !*++p || path_separator((unsigned char)*p))
        return 0u;
    while (*p && !path_separator((unsigned char)*p))
        ++p;
    return (size_t)(p - path) + (*p != '\0');
}

int
snag_char_width(uint32_t cp)
{
    if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
        return -1;
    return uc_width(cp, "UTF-8");
}

int
snag_fd_cloexec(int fd)
{
    intptr_t handle = _get_osfhandle(fd);

    if (handle == -1) {
        errno = EBADF;
        return -1;
    }
    if (SetHandleInformation((HANDLE)handle, HANDLE_FLAG_INHERIT, 0))
        return 0;
    errno = GetLastError() == ERROR_INVALID_HANDLE ? EBADF : EIO;
    return -1;
}

int
snag_random_bytes(unsigned char *out, size_t len)
{
    HCRYPTPROV provider;
    bool ok = true;

    if (!len)
        return 0;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        errno = EIO;
        return -1;
    }
    while (len) {
        DWORD take = len > UINT32_MAX ? UINT32_MAX : (DWORD)len;
        if (!CryptGenRandom(provider, take, out)) {
            ok = false;
            break;
        }
        out += take;
        len -= take;
    }
    if (!CryptReleaseContext(provider, 0))
        ok = false;
    if (!ok)
        errno = EIO;
    return ok ? 0 : -1;
}

uint64_t
snag_time_ms(void)
{
    FILETIME now;
    uint64_t ticks;

    GetSystemTimeAsFileTime(&now);
    ticks = ((uint64_t)now.dwHighDateTime << 32u) | now.dwLowDateTime;
    /* FILETIME counts 100 ns from 1601; callers use milliseconds from 1970. */
    return ticks < UINT64_C(116444736000000000) ? 0u :
           (ticks - UINT64_C(116444736000000000)) / 10000u;
}

uint64_t
snag_monotonic_ms(void)
{
    LARGE_INTEGER counter, frequency;
    uint64_t ticks, hz;

    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
        return 0u;
    ticks = (uint64_t)counter.QuadPart;
    hz = (uint64_t)frequency.QuadPart;
    if (ticks / hz > UINT64_MAX / 1000u)
        return UINT64_MAX;
    return ticks / hz * 1000u +
           (uint64_t)((long double)(ticks % hz) * 1000.0L / (long double)hz);
}

int
snag_sync_file(int fd)
{
    return _commit(fd);
}

int
snag_sync_dir(int fd)
{
    intptr_t handle = _get_osfhandle(fd);
    BY_HANDLE_FILE_INFORMATION info;
    DWORD error;

    if (handle == -1) {
        errno = EBADF;
        return -1;
    }
    if (FlushFileBuffers((HANDLE)handle))
        return 0;
    error = GetLastError();
    if ((error == ERROR_INVALID_FUNCTION || error == ERROR_ACCESS_DENIED ||
         error == ERROR_INVALID_HANDLE) &&
        GetFileInformationByHandle((HANDLE)handle, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        errno = ENOTSUP;
        return 1; /* Valid directory, but this OS cannot flush it this way. */
    }
    errno = error == ERROR_INVALID_HANDLE ? EBADF : EIO;
    return -1;
}

#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/stat.h>
#include <dirent.h>

static int
open_read(int dirfd, const char *path, bool directory)
{
    int fd = openat(dirfd, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                    (directory ? O_DIRECTORY : 0));
    struct stat st;
    int error, rc;

    if (fd < 0)
        return -1;
    rc = fstat(fd, &st);
    if (rc == 0 && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)))
        return fd;
    error = rc < 0 ? errno : EACCES;
    (void)close(fd);
    errno = error;
    return -1;
}

int
snag_open_read(const char *path, bool directory)
{
    return open_read(AT_FDCWD, path, directory);
}

int
snag_open_read_at(int dirfd, const char *path, bool directory)
{
    return open_read(dirfd, path, directory);
}

int
snag_open_private_dir_at(int dirfd, const char *path)
{
    return open_read(dirfd, path, true);
}

void
snag_path_slashes(char *path)
{
    (void)path;
}

struct snag_directory {
    DIR *native;
};

struct snag_directory *
snag_directory_open(int fd)
{
    struct snag_directory *dir = malloc(sizeof(*dir));

    if (dir && !(dir->native = fdopendir(fd))) {
        int error = errno;
        free(dir);
        errno = error;
        return NULL;
    }
    return dir;
}

const char *
snag_directory_next(struct snag_directory *dir)
{
    struct dirent *entry;

    if (!dir) {
        errno = EINVAL;
        return NULL;
    }
    errno = 0;
    entry = readdir(dir->native);
    return entry ? entry->d_name : NULL;
}

int
snag_directory_close(struct snag_directory *dir)
{
    int rc;

    if (!dir) {
        errno = EINVAL;
        return -1;
    }
    rc = closedir(dir->native);
    free(dir);
    return rc;
}

int
snag_fstat(int fd, snag_file_info *out)
{
    return fstat(fd, out);
}

int
snag_stat(const char *path, snag_file_info *out)
{
    return stat(path, out);
}

int
snag_lstat(const char *path, snag_file_info *out)
{
    return lstat(path, out);
}

int
snag_lstat_at(int dirfd, const char *path, snag_file_info *out)
{
    return fstatat(dirfd, path, out, AT_SYMLINK_NOFOLLOW);
}

int
snag_unlink_at(int dirfd, const char *path, bool directory)
{
    return unlinkat(dirfd, path, directory ? AT_REMOVEDIR : 0);
}

int
snag_rename_at(int from_dir, const char *from, int to_dir, const char *to)
{
    return renameat(from_dir, from, to_dir, to);
}

int
snag_mkdir_private(const char *path)
{
    return mkdir(path, 0700);
}

int
snag_mkdir_private_at(int dirfd, const char *path)
{
    return mkdirat(dirfd, path, 0700);
}

int
snag_create_private_at(int dirfd, const char *path, bool exclusive)
{
    struct stat st;
    struct snag_file_privacy privacy;
    int fd = openat(dirfd, path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
                    (exclusive ? O_EXCL : 0), 0600);

    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || snag_fd_privacy(fd, &privacy) < 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1u ||
        !privacy.effective_owner || !privacy.private_access) {
        (void)close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}

int
snag_fd_privacy(int fd, struct snag_file_privacy *out)
{
    struct stat st;

    if (!out) {
        errno = EINVAL;
        return -1;
    }
    if (fstat(fd, &st) < 0)
        return -1;
    out->real_owner = st.st_uid == getuid();
    out->effective_owner = st.st_uid == geteuid();
    out->private_access = !(st.st_mode & 077u);
    return 0;
}

char *
snag_realpath(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    return realpath(path, NULL);
}

size_t
snag_path_root_len(const char *path)
{
    return path && path[0] == '/' ? 1u : 0u;
}

int
snag_char_width(uint32_t cp)
{
    return cp <= (uint32_t)WCHAR_MAX ? wcwidth((wchar_t)cp) : -1;
}

int
snag_fd_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);

    return flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -1 : 0;
}

int
snag_random_bytes(unsigned char *out, size_t len)
{
    size_t done = 0;
    int fd;

    if (!len)
        return 0;
    fd = open("/dev/urandom", O_RDONLY
#ifdef O_CLOEXEC
        | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
        | O_NOFOLLOW
#endif
    );
    if (fd < 0)
        return -1;
    while (done < len) {
        size_t take = len - done > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : len - done;
        ssize_t n = read(fd, out + done, take);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        int error = n == 0 ? EIO : errno;
        (void)close(fd);
        errno = error;
        return -1;
    }
    return close(fd);
}

uint64_t
snag_time_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000u)
        return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t
snag_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

int
snag_sync_file(int fd)
{
#if defined(__APPLE__)
    return fsync(fd);
#else
    if (fdatasync(fd) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS)
        return -1;
    return fsync(fd);
#endif
}

int
snag_sync_dir(int fd)
{
    if (fsync(fd) == 0)
        return 0;
    if (errno == EINVAL || errno == ENOTSUP || errno == EROFS)
        return 1;
    return -1;
}
#endif

int
snag_write_full(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t done = 0;

    while (done < len) {
#ifdef _WIN32
        unsigned int take = len - done > INT_MAX ? INT_MAX : (unsigned int)(len - done);
        int n = _write(fd, p + done, take);
#else
        size_t take = len - done > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : len - done;
        ssize_t n = write(fd, p + done, take);
#endif
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            errno = EIO;
        return -1;
    }
    return 0;
}
