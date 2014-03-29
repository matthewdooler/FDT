/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

*/

/*
 * Loopback OSXFUSE file system in C. Uses the high-level FUSE API.
 * Based on the fusexmp_fh.c example from the Linux FUSE distribution.
 * Amit Singh <http://osxbook.com>
 *
 * With modifications by Matthew Dooler, so that the functions can be
 * used directly from osxfuse without mounting the filesystem
 */

#include <AvailabilityMacros.h>

#if !defined(AVAILABLE_MAC_OS_X_VERSION_10_5_AND_LATER)
#error "This file system requires Leopard and above."
#endif

#define FUSE_USE_VERSION 26

#define _GNU_SOURCE

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/xattr.h>
//#include <sys/attr.h>
#include <sys/param.h>
#include <sys/vnode.h>

#if defined(_POSIX_C_SOURCE)
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
#endif

#define G_PREFIX                       "org"
#define G_KAUTH_FILESEC_XATTR G_PREFIX ".apple.system.Security"
#define A_PREFIX                       "com"
#define A_KAUTH_FILESEC_XATTR A_PREFIX ".apple.system.Security"
#define XATTR_APPLE_PREFIX             "com.apple."

const char * root_path;

static void resolve_path(char * resolved_path, const char * path) {
    strcpy(resolved_path, root_path);
    strncat(resolved_path, path, PATH_MAX);
}

static int
loopback_getattr(const char *path, struct stat *stbuf)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    res = lstat(resolved_path, stbuf);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_access(const char *path, int amode) {
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    res = access(resolved_path, amode);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_fgetattr(const char *path, struct stat *stbuf,
                  struct fuse_file_info *fi)
{
    int res;
    (void)path;

    res = fstat(fi->fh, stbuf);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_readlink(const char *path, char *buf, size_t size)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    res = readlink(resolved_path, buf, size - 1);
    if (res == -1) {
        return -errno;
    }

    buf[res] = '\0';

    return 0;
}

struct loopback_dirp {
    DIR *dp;
    struct dirent *entry;
    off_t offset;
};

static int
loopback_opendir(const char *path, struct fuse_file_info *fi)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    struct loopback_dirp *d = malloc(sizeof(struct loopback_dirp));
    if (d == NULL) {
        return -ENOMEM;
    }

    d->dp = opendir(resolved_path);
    if (d->dp == NULL) {
        res = -errno;
        free(d);
        return res;
    }

    d->offset = 0;
    d->entry = NULL;

    fi->fh = (unsigned long)d;

    return 0;
}

static inline struct loopback_dirp *
get_dirp(struct fuse_file_info *fi)
{
    return (struct loopback_dirp *)(uintptr_t)fi->fh;
}

static int
loopback_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                 off_t offset, struct fuse_file_info *fi)
{
    struct loopback_dirp *d = get_dirp(fi);
    (void)path;

    if (offset != d->offset) {
        seekdir(d->dp, offset);
        d->entry = NULL;
        d->offset = offset;
    }

    while (1) {
        struct stat st;
        off_t nextoff;

        if (!d->entry) {
            d->entry = readdir(d->dp);
            if (!d->entry) {
                break;
            }
        }

        memset(&st, 0, sizeof(st));
        st.st_ino = d->entry->d_ino;
        st.st_mode = d->entry->d_type << 12;
        nextoff = telldir(d->dp);
        if (filler(buf, d->entry->d_name, &st, nextoff)) {
            break;
        }

        d->entry = NULL;
        d->offset = nextoff;
    }

    return 0;
}

static int
loopback_releasedir(const char *path, struct fuse_file_info *fi)
{
    struct loopback_dirp *d = get_dirp(fi);

    (void)path;

    closedir(d->dp);
    free(d);

    return 0;
}

static int
loopback_mknod(const char *path, mode_t mode, dev_t rdev)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    if (S_ISFIFO(mode)) {
        res = mkfifo(resolved_path, mode);
    } else {
        res = mknod(resolved_path, mode, rdev);
    }

    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_mkdir(const char *path, mode_t mode)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    res = mkdir(resolved_path, mode);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_unlink(const char *path)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    res = unlink(resolved_path);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_rmdir(const char *path)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    res = rmdir(resolved_path);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_symlink(const char *from, const char *to)
{
    int res;
    char resolved_from[PATH_MAX];
    char resolved_to[PATH_MAX];
    resolve_path(resolved_from, from);
    resolve_path(resolved_to, to);

    res = symlink(resolved_from, resolved_to);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_rename(const char *from, const char *to)
{
    int res;
    char resolved_from[PATH_MAX];
    char resolved_to[PATH_MAX];
    resolve_path(resolved_from, from);
    resolve_path(resolved_to, to);

    res = rename(resolved_from, resolved_to);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_exchange(const char *path1, const char *path2, unsigned long options)
{
    int res;
    char resolved_path_1[PATH_MAX];
    char resolved_path_2[PATH_MAX];
    resolve_path(resolved_path_1, path1);
    resolve_path(resolved_path_2, path2);

    res = exchangedata(resolved_path_1, resolved_path_2, options);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_link(const char *from, const char *to)
{
    int res;
    char resolved_from[PATH_MAX];
    char resolved_to[PATH_MAX];
    resolve_path(resolved_from, from);
    resolve_path(resolved_to, to);

    res = link(resolved_from, resolved_to);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_fsetattr_x(const char *path, struct setattr_x *attr,
                    struct fuse_file_info *fi)
{
    int res;
    uid_t uid = -1;
    gid_t gid = -1;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    if (SETATTR_WANTS_MODE(attr)) {
        res = lchmod(resolved_path, attr->mode);
        if (res == -1) {
            return -errno;
        }
    }

    if (SETATTR_WANTS_UID(attr)) {
        uid = attr->uid;
    }

    if (SETATTR_WANTS_GID(attr)) {
        gid = attr->gid;
    }

    if ((uid != -1) || (gid != -1)) {
        res = lchown(resolved_path, uid, gid);
        if (res == -1) {
            return -errno;
        }
    }

    if (SETATTR_WANTS_SIZE(attr)) {
        if (fi) {
            res = ftruncate(fi->fh, attr->size);
        } else {
            res = truncate(resolved_path, attr->size);
        }
        if (res == -1) {
            return -errno;
        }
    }

    if (SETATTR_WANTS_MODTIME(attr)) {
        struct timeval tv[2];
        if (!SETATTR_WANTS_ACCTIME(attr)) {
            gettimeofday(&tv[0], NULL);
        } else {
            tv[0].tv_sec = attr->acctime.tv_sec;
            tv[0].tv_usec = attr->acctime.tv_nsec / 1000;
        }
        tv[1].tv_sec = attr->modtime.tv_sec;
        tv[1].tv_usec = attr->modtime.tv_nsec / 1000;
        res = utimes(resolved_path, tv);
        if (res == -1) {
            return -errno;
        }
    }

    // This doesn't compile with osxfuse
    /*if (SETATTR_WANTS_CRTIME(attr)) {
        struct attrlist attributes;

        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.reserved = 0;
        attributes.commonattr = ATTR_CMN_CRTIME;
        attributes.dirattr = 0;
        attributes.fileattr = 0;
        attributes.forkattr = 0;
        attributes.volattr = 0;

        res = setattrlist(resolved_path, &attributes, &attr->crtime,
                  sizeof(struct timespec), FSOPT_NOFOLLOW);

        if (res == -1) {
            return -errno;
        }
    }*/

    // This doesn't compile with osxfuse
    /*if (SETATTR_WANTS_CHGTIME(attr)) {
        struct attrlist attributes;

        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.reserved = 0;
        attributes.commonattr = ATTR_CMN_CHGTIME;
        attributes.dirattr = 0;
        attributes.fileattr = 0;
        attributes.forkattr = 0;
        attributes.volattr = 0;

        res = setattrlist(resolved_path, &attributes, &attr->chgtime,
                  sizeof(struct timespec), FSOPT_NOFOLLOW);

        if (res == -1) {
            return -errno;
        }
    }*/

    // This doesn't compile with osxfuse
    /*if (SETATTR_WANTS_BKUPTIME(attr)) {
        struct attrlist attributes;

        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.reserved = 0;
        attributes.commonattr = ATTR_CMN_BKUPTIME;
        attributes.dirattr = 0;
        attributes.fileattr = 0;
        attributes.forkattr = 0;
        attributes.volattr = 0;

        res = setattrlist(resolved_path, &attributes, &attr->bkuptime,
                  sizeof(struct timespec), FSOPT_NOFOLLOW);

        if (res == -1) {
            return -errno;
        }
    }*/

    if (SETATTR_WANTS_FLAGS(attr)) {
        res = lchflags(resolved_path, attr->flags);
        if (res == -1) {
            return -errno;
        }
    }

    return 0;
}

static int
loopback_setattr_x(const char *path, struct setattr_x *attr)
{
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);
    return loopback_fsetattr_x(resolved_path, attr, (struct fuse_file_info *)0);
}

// This doesn't compile with osxfuse
/*static int
loopback_getxtimes(const char *path, struct timespec *bkuptime,
                   struct timespec *crtime)
{
    int res = 0;
    struct attrlist attributes;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.reserved    = 0;
    attributes.commonattr  = 0;
    attributes.dirattr     = 0;
    attributes.fileattr    = 0;
    attributes.forkattr    = 0;
    attributes.volattr     = 0;



    struct xtimeattrbuf {
        uint32_t size;
        struct timespec xtime;
    } __attribute__ ((packed));


    struct xtimeattrbuf buf;

    //attributes.commonattr = ATTR_CMN_BKUPTIME;
    res = getattrlist(resolved_path, &attributes, &buf, sizeof(buf), FSOPT_NOFOLLOW);
    if (res == 0) {
        (void)memcpy(bkuptime, &(buf.xtime), sizeof(struct timespec));
    } else {
        (void)memset(bkuptime, 0, sizeof(struct timespec));
    }

    //attributes.commonattr = ATTR_CMN_CRTIME;
    res = getattrlist(resolved_path, &attributes, &buf, sizeof(buf), FSOPT_NOFOLLOW);
    if (res == 0) {
        (void)memcpy(crtime, &(buf.xtime), sizeof(struct timespec));
    } else {
        (void)memset(crtime, 0, sizeof(struct timespec));
    }

    return 0;
}*/

static int
loopback_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int fd;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    fd = open(resolved_path, fi->flags, mode);
    if (fd == -1) {
        return -errno;
    }

    fi->fh = fd;
    return 0;
}

static int
loopback_open(const char *path, struct fuse_file_info *fi)
{
    int fd;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    fd = open(resolved_path, fi->flags);
    if (fd == -1) {
        return -errno;
    }

    fi->fh = fd;
    return 0;
}

static int
loopback_read(const char *path, char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi)
{
    int res;
    (void)path;
    res = pread(fi->fh, buf, size, offset);
    if (res == -1) {
        res = -errno;
    }

    return res;
}

static int
loopback_write(const char *path, const char *buf, size_t size,
               off_t offset, struct fuse_file_info *fi)
{
    int res;

    (void)path;

    res = pwrite(fi->fh, buf, size, offset);
    if (res == -1) {
        res = -errno;
    }

    return res;
}

static int
loopback_statfs(const char *path, struct statvfs *stbuf)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    res = statvfs(resolved_path, stbuf);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_flush(const char *path, struct fuse_file_info *fi)
{
    int res;

    (void)path;

    res = close(dup(fi->fh));
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;

    close(fi->fh);

    return 0;
}

static int
loopback_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
    int res;

    (void)path;

    (void)isdatasync;

    res = fsync(fi->fh);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_setxattr(const char *path, const char *name, const char *value,
                  size_t size, int flags, uint32_t position)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    if (!strncmp(name, XATTR_APPLE_PREFIX, sizeof(XATTR_APPLE_PREFIX) - 1)) {
        flags &= ~(XATTR_NOSECURITY);
    }

    if (!strcmp(name, A_KAUTH_FILESEC_XATTR)) {

        char new_name[MAXPATHLEN];

        memcpy(new_name, A_KAUTH_FILESEC_XATTR, sizeof(A_KAUTH_FILESEC_XATTR));
        memcpy(new_name, G_PREFIX, sizeof(G_PREFIX) - 1);

        res = setxattr(resolved_path, new_name, value, size, position, XATTR_NOFOLLOW);

    } else {
        res = setxattr(resolved_path, name, value, size, position, XATTR_NOFOLLOW);
    }

    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int
loopback_getxattr(const char *path, const char *name, char *value, size_t size,
                  uint32_t position)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    if (strcmp(name, A_KAUTH_FILESEC_XATTR) == 0) {

        char new_name[MAXPATHLEN];

        memcpy(new_name, A_KAUTH_FILESEC_XATTR, sizeof(A_KAUTH_FILESEC_XATTR));
        memcpy(new_name, G_PREFIX, sizeof(G_PREFIX) - 1);

        res = getxattr(resolved_path, new_name, value, size, position, XATTR_NOFOLLOW);

    } else {
        res = getxattr(resolved_path, name, value, size, position, XATTR_NOFOLLOW);
    }

    if (res == -1) {
        return -errno;
    }

    return res;
}

static int
loopback_listxattr(const char *path, char *list, size_t size)
{
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    ssize_t res = listxattr(resolved_path, list, size, XATTR_NOFOLLOW);
    if (res > 0) {
        if (list) {
            size_t len = 0;
            char *curr = list;
            do {
                size_t thislen = strlen(curr) + 1;
                if (strcmp(curr, G_KAUTH_FILESEC_XATTR) == 0) {
                    memmove(curr, curr + thislen, res - len - thislen);
                    res -= thislen;
                    break;
                }
                curr += thislen;
                len += thislen;
            } while (len < res);
        } else {
            /*
            ssize_t res2 = getxattr(resolved_path, G_KAUTH_FILESEC_XATTR, NULL, 0, 0,
                                    XATTR_NOFOLLOW);
            if (res2 >= 0) {
                res -= sizeof(G_KAUTH_FILESEC_XATTR);
            }
            */
        }
    }

    if (res == -1) {
        return -errno;
    }

    return res;
}

static int
loopback_removexattr(const char *path, const char *name)
{
    int res;
    char resolved_path[PATH_MAX];
    resolve_path(resolved_path, path);

    if (strcmp(name, A_KAUTH_FILESEC_XATTR) == 0) {

        char new_name[MAXPATHLEN];

        memcpy(new_name, A_KAUTH_FILESEC_XATTR, sizeof(A_KAUTH_FILESEC_XATTR));
        memcpy(new_name, G_PREFIX, sizeof(G_PREFIX) - 1);

        res = removexattr(resolved_path, new_name, XATTR_NOFOLLOW);

    } else {
        res = removexattr(resolved_path, name, XATTR_NOFOLLOW);
    }

    if (res == -1) {
        return -errno;
    }

    return 0;
}

#if FUSE_VERSION >= 29

static int
loopback_fallocate(const char *path, int mode, off_t offset, off_t length,
                   struct fuse_file_info *fi)
{
    fstore_t fstore;

    if (!(mode & PREALLOCATE)) {
        return -ENOTSUP;
    }

    fstore.fst_flags = 0;
    if (mode & ALLOCATECONTIG) {
        fstore.fst_flags |= F_ALLOCATECONTIG;
    }
    if (mode & ALLOCATEALL) {
        fstore.fst_flags |= F_ALLOCATEALL;
    }

    if (mode & ALLOCATEFROMPEOF) {
        fstore.fst_posmode = F_PEOFPOSMODE;
    } else if (mode & ALLOCATEFROMVOL) {
        fstore.fst_posmode = F_VOLPOSMODE;
    }

    fstore.fst_offset = offset;
    fstore.fst_length = length;

    if (fcntl(fi->fh, F_PREALLOCATE, &fstore) == -1) {
        return -errno;
    } else {
        return 0;
    }
}

#endif /* FUSE_VERSION >= 29 */

void *
loopback_init(struct fuse_conn_info *conn)
{
    FUSE_ENABLE_SETVOLNAME(conn);
    FUSE_ENABLE_XTIMES(conn);

    return NULL;
}

void
loopback_destroy(void *userdata)
{
    /* nothing */
}

static struct fuse_operations passthru_ops_s = {
    .init        = loopback_init,
    .destroy     = loopback_destroy,
    .getattr     = loopback_getattr,
    .fgetattr    = loopback_fgetattr,
    .access      = loopback_access,
    .readlink    = loopback_readlink,
    .opendir     = loopback_opendir,
    .readdir     = loopback_readdir,
    .releasedir  = loopback_releasedir,
    .mknod       = loopback_mknod,
    .mkdir       = loopback_mkdir,
    .symlink     = loopback_symlink,
    .unlink      = loopback_unlink,
    .rmdir       = loopback_rmdir,
    .rename      = loopback_rename,
    .link        = loopback_link,
    .create      = loopback_create,
    .open        = loopback_open,
    .read        = loopback_read,
    .write       = loopback_write,
    .statfs      = loopback_statfs,
    .flush       = loopback_flush,
    .release     = loopback_release,
    .fsync       = loopback_fsync,
    .setxattr    = loopback_setxattr,
    .getxattr    = loopback_getxattr,
    .listxattr   = loopback_listxattr,
    .removexattr = loopback_removexattr,
    .exchange    = loopback_exchange,
    /*.getxtimes   = loopback_getxtimes,*/
    .setattr_x   = loopback_setattr_x,
    .fsetattr_x  = loopback_fsetattr_x,
#if FUSE_VERSION >= 29
    .fallocate   = loopback_fallocate,
#endif
};