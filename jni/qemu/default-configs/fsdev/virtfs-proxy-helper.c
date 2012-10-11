/*
 * Helper for QEMU Proxy FS Driver
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 * M. Mohan Kumar <mohan@in.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See
 * the COPYING file in the top-level directory.
 */

#include <sys/resource.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/capability.h>
#include <sys/fsuid.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#ifdef CONFIG_LINUX_MAGIC_H
#include <linux/magic.h>
#endif
#include "qemu-common.h"
#include "qemu_socket.h"
#include "qemu-xattr.h"
#include "virtio-9p-marshal.h"
#include "hw/9pfs/virtio-9p-proxy.h"
#include "fsdev/virtio-9p-marshal.h"

#define PROGNAME "virtfs-proxy-helper"

#ifndef XFS_SUPER_MAGIC
#define XFS_SUPER_MAGIC  0x58465342
#endif
#ifndef EXT2_SUPER_MAGIC
#define EXT2_SUPER_MAGIC 0xEF53
#endif
#ifndef REISERFS_SUPER_MAGIC
#define REISERFS_SUPER_MAGIC 0x52654973
#endif
#ifndef BTRFS_SUPER_MAGIC
#define BTRFS_SUPER_MAGIC 0x9123683E
#endif

static struct option helper_opts[] = {
    {"fd", required_argument, NULL, 'f'},
    {"path", required_argument, NULL, 'p'},
    {"nodaemon", no_argument, NULL, 'n'},
    {"socket", required_argument, NULL, 's'},
    {"uid", required_argument, NULL, 'u'},
    {"gid", required_argument, NULL, 'g'},
};

static bool is_daemon;
static bool get_version; /* IOC getversion IOCTL supported */

static void GCC_FMT_ATTR(2, 3) do_log(int loglevel, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    if (is_daemon) {
        vsyslog(LOG_CRIT, format, ap);
    } else {
        vfprintf(stderr, format, ap);
    }
    va_end(ap);
}

static void do_perror(const char *string)
{
    if (is_daemon) {
        syslog(LOG_CRIT, "%s:%s", string, strerror(errno));
    } else {
        fprintf(stderr, "%s:%s\n", string, strerror(errno));
    }
}

static int do_cap_set(cap_value_t *cap_value, int size, int reset)
{
    cap_t caps;
    if (reset) {
        /*
         * Start with an empty set and set permitted and effective
         */
        caps = cap_init();
        if (caps == NULL) {
            do_perror("cap_init");
            return -1;
        }
        if (cap_set_flag(caps, CAP_PERMITTED, size, cap_value, CAP_SET) < 0) {
            do_perror("cap_set_flag");
            goto error;
        }
    } else {
        caps = cap_get_proc();
        if (!caps) {
            do_perror("cap_get_proc");
            return -1;
        }
    }
    if (cap_set_flag(caps, CAP_EFFECTIVE, size, cap_value, CAP_SET) < 0) {
        do_perror("cap_set_flag");
        goto error;
    }
    if (cap_set_proc(caps) < 0) {
        do_perror("cap_set_proc");
        goto error;
    }
    cap_free(caps);
    return 0;

error:
    cap_free(caps);
    return -1;
}

static int init_capabilities(void)
{
    /* helper needs following capbabilities only */
    cap_value_t cap_list[] = {
        CAP_CHOWN,
        CAP_DAC_OVERRIDE,
        CAP_FOWNER,
        CAP_FSETID,
        CAP_SETGID,
        CAP_MKNOD,
        CAP_SETUID,
    };
    return do_cap_set(cap_list, ARRAY_SIZE(cap_list), 1);
}

static int socket_read(int sockfd, void *buff, ssize_t size)
{
    ssize_t retval, total = 0;

    while (size) {
        retval = read(sockfd, buff, size);
        if (retval == 0) {
            return -EIO;
        }
        if (retval < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        size -= retval;
        buff += retval;
        total += retval;
    }
    return total;
}

static int socket_write(int sockfd, void *buff, ssize_t size)
{
    ssize_t retval, total = 0;

    while (size) {
        retval = write(sockfd, buff, size);
        if (retval < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        size -= retval;
        buff += retval;
        total += retval;
    }
    return total;
}

static int read_request(int sockfd, struct iovec *iovec, ProxyHeader *header)
{
    int retval;

    /*
     * read the request header.
     */
    iovec->iov_len = 0;
    retval = socket_read(sockfd, iovec->iov_base, PROXY_HDR_SZ);
    if (retval < 0) {
        return retval;
    }
    iovec->iov_len = PROXY_HDR_SZ;
    retval = proxy_unmarshal(iovec, 0, "dd", &header->type, &header->size);
    if (retval < 0) {
        return retval;
    }
    /*
     * We can't process message.size > PROXY_MAX_IO_SZ.
     * Treat it as fatal error
     */
    if (header->size > PROXY_MAX_IO_SZ) {
        return -ENOBUFS;
    }
    retval = socket_read(sockfd, iovec->iov_base + PROXY_HDR_SZ, header->size);
    if (retval < 0) {
        return retval;
    }
    iovec->iov_len += header->size;
    return 0;
}

static int send_fd(int sockfd, int fd)
{
    struct msghdr msg;
    struct iovec iov;
    int retval, data;
    struct cmsghdr *cmsg;
    union MsgControl msg_control;

    iov.iov_base = &data;
    iov.iov_len = sizeof(data);

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    /* No ancillary data on error */
    if (fd < 0) {
        /* fd is really negative errno if the request failed  */
        data = fd;
    } else {
        data = V9FS_FD_VALID;
        msg.msg_control = &msg_control;
        msg.msg_controllen = sizeof(msg_control);

        cmsg = &msg_control.cmsg;
        cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    }

    do {
        retval = sendmsg(sockfd, &msg, 0);
    } while (retval < 0 && errno == EINTR);
    if (fd >= 0) {
        close(fd);
    }
    if (retval < 0) {
        return retval;
    }
    return 0;
}

static int send_status(int sockfd, struct iovec *iovec, int status)
{
    ProxyHeader header;
    int retval, msg_size;;

    if (status < 0) {
        header.type = T_ERROR;
    } else {
        header.type = T_SUCCESS;
    }
    header.size = sizeof(status);
    /*
     * marshal the return status. We don't check error.
     * because we are sure we have enough space for the status
     */
    msg_size = proxy_marshal(iovec, 0, "ddd", header.type,
                             header.size, status);
    retval = socket_write(sockfd, iovec->iov_base, msg_size);
    if (retval < 0) {
        return retval;
    }
    return 0;
}

/*
 * from man 7 capabilities, section
 * Effect of User ID Changes on Capabilities:
 * 4. If the file system user ID is changed from 0 to nonzero (see setfsuid(2))
 * then the following capabilities are cleared from the effective set:
 * CAP_CHOWN, CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH,  CAP_FOWNER, CAP_FSETID,
 * CAP_LINUX_IMMUTABLE  (since  Linux 2.2.30), CAP_MAC_OVERRIDE, and CAP_MKNOD
 * (since Linux 2.2.30). If the file system UID is changed from nonzero to 0,
 * then any of these capabilities that are enabled in the permitted set
 * are enabled in the effective set.
 */
static int setfsugid(int uid, int gid)
{
    /*
     * We still need DAC_OVERRIDE because  we don't change
     * supplementary group ids, and hence may be subjected DAC rules
     */
    cap_value_t cap_list[] = {
        CAP_DAC_OVERRIDE,
    };

    setfsgid(gid);
    setfsuid(uid);

    if (uid != 0 || gid != 0) {
        return do_cap_set(cap_list, ARRAY_SIZE(cap_list), 0);
    }
    return 0;
}

/*
 * send response in two parts
 * 1) ProxyHeader
 * 2) Response or error status
 * This function should be called with marshaled response
 * send_response constructs header part and error part only.
 * send response sends {ProxyHeader,Response} if the request was success
 * otherwise sends {ProxyHeader,error status}
 */
static int send_response(int sock, struct iovec *iovec, int size)
{
    int retval;
    ProxyHeader header;

    /*
     * If response size exceeds available iovec->iov_len,
     * we return ENOBUFS
     */
    if (size > PROXY_MAX_IO_SZ) {
        size = -ENOBUFS;
    }

    if (size < 0) {
        /*
         * In case of error we would not have got the error encoded
         * already so encode the error here.
         */
        header.type = T_ERROR;
        header.size = sizeof(size);
        proxy_marshal(iovec, PROXY_HDR_SZ, "d", size);
    } else {
        header.type = T_SUCCESS;
        header.size = size;
    }
    proxy_marshal(iovec, 0, "dd", header.type, header.size);
    retval = socket_write(sock, iovec->iov_base, header.size + PROXY_HDR_SZ);
    if (retval < 0) {
        return retval;;
    }
    return 0;
}

/*
 * gets generation number
 * returns -errno on failure and sizeof(generation number) on success
 */
static int do_getversion(struct iovec *iovec, struct iovec *out_iovec)
{
    uint64_t version;
    int retval = -ENOTTY;
#ifdef FS_IOC_GETVERSION
    int fd;
    V9fsString path;
#endif


    /* no need to issue ioctl */
    if (!get_version) {
        version = 0;
        retval = proxy_marshal(out_iovec, PROXY_HDR_SZ, "q", version);
        return retval;
    }
#ifdef FS_IOC_GETVERSION
    retval = proxy_unmarshal(iovec, PROXY_HDR_SZ, "s", &path);
    if (retval < 0) {
        return retval;
    }

    fd = open(path.data, O_RDONLY);
    if (fd < 0) {
        retval = -errno;
        goto err_out;
    }
    if (ioctl(fd, FS_IOC_GETVERSION, &version) < 0) {
        retval = -errno;
    } else {
        retval = proxy_marshal(out_iovec, PROXY_HDR_SZ, "q", version);
    }
    close(fd);
err_out:
    v9fs_string_free(&path);
#endif
    return retval;
}

static int do_getxattr(int type, struct iovec *iovec, struct iovec *out_iovec)
{
    int size = 0, offset, retval;
    V9fsString path, name, xattr;

    v9fs_string_init(&xattr);
    v9fs_string_init(&path);
    retval = proxy_unmarshal(iovec, PROXY_HDR_SZ, "ds", &size, &path);
    if (retval < 0) {
        return retval;
    }
    offset = PROXY_HDR_SZ + retval;

    if (size) {
        xattr.data = g_malloc(size);
        xattr.size = size;
    }
    switch (type) {
    case T_LGETXATTR:
        v9fs_string_init(&name);
        retval = proxy_unmarshal(iovec, offset, "s", &name);
        if (retval > 0) {
            retval = lgetxattr(path.data, name.data, xattr.data, size);
            if (retval < 0) {
                retval = -errno;
            } else {
                xattr.size = retval;
            }
        }
        v9fs_string_free(&name);
        break;
    case T_LLISTXATTR:
        retval = llistxattr(path.data, xattr.data, size);
        if (retval < 0) {
            retval = -errno;
        } else {
            xattr.size = retval;
        }
        break;
    }
    if (retval < 0) {
        goto err_out;
    }

    if (!size) {
        proxy_marshal(out_iovec, PROXY_HDR_SZ, "d", retval);
        retval = sizeof(retval);
    } else {
        retval = proxy_marshal(out_iovec, PROXY_HDR_SZ, "s", &xattr);
    }
err_out:
    v9fs_string_free(&xattr);
    v9fs_string_free(&path);
    return retval;
}

static void stat_to_prstat(ProxyStat *pr_stat, struct stat *stat)
{
    memset(pr_stat, 0, sizeof(*pr_stat));
    pr_stat->st_dev = stat->st_dev;
    pr_stat->st_ino = stat->st_ino;
    pr_stat->st_nlink = stat->st_nlink;
    pr_stat->st_mode = stat->st_mode;
    pr_stat->st_uid = stat->st_uid;
    pr_stat->st_gid = stat->st_gid;
    pr_stat->st_rdev = stat->st_rdev;
    pr_stat->st_size = stat->st_size;
    pr_stat->st_blksize = stat->st_blksize;
    pr_stat->st_blocks = stat->st_blocks;
    pr_stat->st_atim_sec = stat->st_atim.tv_sec;
    pr_stat->st_atim_nsec = stat->st_atim.tv_nsec;
    pr_stat->st_mtim_sec = stat->st_mtim.tv_sec;
    pr_stat->st_mtim_nsec = stat->st_mtim.tv_nsec;
    pr_stat->st_ctim_sec = stat->st_ctim.tv_sec;
    pr_stat->st_ctim_nsec = stat->st_ctim.tv_nsec;
}

static void statfs_to_prstatfs(ProxyStatFS *pr_stfs, struct statfs *stfs)
{
    memset(pr_stfs, 0, sizeof(*pr_stfs));
    pr_stfs->f_type = stfs->f_type;
    pr_stfs->f_bsize = stfs->f_bsize;
    pr_stfs->f_blocks = stfs->f_blocks;
    pr_stfs->f_bfree = stfs->f_bfree;
    pr_stfs->f_bavail = stfs->f_bavail;
    pr_stfs->f_files = stfs->f_files;
    pr_stfs->f_ffree = stfs->f_ffree;
    pr_stfs->f_fsid[0] = stfs->f_fsid.__val[0];
    pr_stfs->f_fsid[1] = stfs->f_fsid.__val[1];
    pr_stfs->f_namelen = stfs->f_namelen;
    pr_stfs->f_frsize = stfs->f_frsize;
}

/*
 * Gets stat/statfs information and packs in out_iovec structure
 * on success returns number of bytes packed in out_iovec struture
 * otherwise returns -errno
 */
static int do_stat(int type, struct iovec *iovec, struct iovec *out_iovec)
{
    int retval;
    V9fsString path;
    ProxyStat pr_stat;
    ProxyStatFS pr_stfs;
    struct stat st_buf;
    struct statfs stfs_buf;

    v9fs_string_init(&path);
    retval = proxy_unmarshal(iovec, PROXY_HDR_SZ, "s", &path);
    if (retval < 0) {
        return retval;
    }

    switch (type) {
    case T_LSTAT:
        retval = lstat(path.data, &st_buf);
        if (retval < 0) {
            retval = -errno;
        } else {
            stat_to_prstat(&pr_stat, &st_buf);
            retval = proxy_marshal(out_iovec, PROXY_HDR_SZ,
                                   "qqqdddqqqqqqqqqq", pr_stat.st_dev,
                                   pr_stat.st_ino, pr_stat.st_nlink,
                                   pr_stat.st_mode, pr_stat.st_uid,
                                   pr_stat.st_gid, pr_stat.st_rdev,
                                   pr_stat.st_size, pr_stat.st_blksize,
                                   pr_stat.st_blocks,
                                   pr_stat.st_atim_sec, pr_stat.st_atim_nsec,
                                   pr_stat.st_mtim_sec, pr_stat.st_mtim_nsec,
                                   pr_stat.st_ctim_sec, pr_stat.st_ctim_nsec);
        }
        break;
    case T_STATFS:
        retval = statfs(path.data, &stfs_buf);
        if (retval < 0) {
            retval = -errno;
        } else {
            statfs_to_prstatfs(&pr_stfs, &stfs_buf);
            retval = proxy_marshal(out_iovec, PROXY_HDR_SZ,
                                   "qqqqqqqqqqq", pr_stfs.f_type,
                                   pr_stfs.f_bsize, pr_stfs.f_blocks,
                                   pr_stfs.f_bfree, pr_stfs.f_bavail,
                                   pr_stfs.f_files, pr_stfs.f_ffree,
                                   pr_stfs.f_fsid[0], pr_stfs.f_fsid[1],
                                   pr_stfs.f_namelen, pr_stfs.f_frsize);
        }
        break;
    }
    v9fs_string_free(&path);
    return retval;
}

static int do_readlink(struct iovec *iovec, struct iovec *out_iovec)
{
    char *buffer;
    int size, retval;
    V9fsString target, path;

    v9fs_string_init(&path);
    retval = proxy_unmarshal(iovec, PROXY_HDR_SZ, "sd", &path, &size);
    if (retval < 0) {
        v9fs_string_free(&path);
        return retval;
    }
    buffer = g_malloc(size);
    v9fs_string_init(&target);
    retval = readlink(path.data, buffer, size);
    if (retval > 0) {
        buffer[retval] = '\0';
        v9fs_string_sprintf(&target, "%s", buffer);
        retval = proxy_marshal(out_iovec, PROXY_HDR_SZ, "s", &target);
    } else {
        retval = -errno;
    }
    g_free(buffer);
    v9fs_string_free(&target);
    v9fs_string_free(&path);
    return retval;
}

/*
 * create other filesystem objects and send 0 on success
 * return -errno on error
 */
static int do_create_others(int type, struct iovec *iovec)
{
    dev_t rdev;
    int retval = 0;
    int offset = PROXY_HDR_SZ;
    V9fsString oldpath, path;
    int mode, uid, gid, cur_uid, cur_gid;

    v9fs_string_init(&path);
    v9fs_string_init(&oldpath);
    cur_uid = geteuid();
    cur_gid = getegid();

    retval = proxy_unmarshal(iovec, offset, "dd", &uid, &gid);
    if (retval < 0) {
        return retval;
    }
    offset += retval;
    retval = setfsugid(uid, gid);
    if (retval < 0) {
        retval = -errno;
        goto err_out;
    }
    switch (type) {
    case T_MKNOD:
        retval = proxy_unmarshal(iovec, offset, "sdq", &path, &mode, &rdev);
        if (retval < 0) {
            goto err_out;
        }
        retval = mknod(path.data, mode, rdev);
        break;
    case T_MKDIR:
        retval = proxy_unmarshal(iovec, offset, "sd", &path, &mode);
        if (retval < 0) {
            goto err_out;
        }
        retval = mkdir(path.data, mode);
        break;
    case T_SYMLINK:
        retval = proxy_unmarshal(iovec, offset, "ss", &oldpath, &path);
        if (retval < 0) {
            goto err_out;
        }
        retval = symlink(oldpath.data, path.data);
        break;
    }
    if (retval < 0) {
        retval = -errno;
    }

err_out:
    v9fs_string_free(&path);
    v9fs_string_free(&oldpath);
    setfsugid(cur_uid, cur_gid);
    return retval;
}

/*
 * create a file and send fd on success
 * return -errno on error
 */
static int do_create(struct iovec *iovec)
{
    int ret;
    V9fsString path;
    int flags, mode, uid, gid, cur_uid, cur_gid;

    v9fs_string_init(&path);
    ret = proxy_unmarshal(iovec, PROXY_HDR_SZ, "sdddd",
                          &path, &flags, &mode, &uid, &gid);
    if (ret < 0) {
        goto unmarshal_err_out;
    }
    cur_uid = geteuid();
    cur_gid = getegid();
    ret = setfsugid(uid, gid);
    if (ret < 0) {
        /*
         * On failure reset back to the
         * old uid/gid
         */
        ret = -errno;
        goto err_out;
    }
    ret = open(path.data, flags, mode);
    if (ret < 0) {
    	LOGV("%s: Could not open file: %s", __func__, path.data);
        ret = -errno;
    }

err_out:
    setfsugid(cur_uid, cur_gid);
unmarshal_err_out:
    v9fs_string_free(&path);
    return ret;
}

/*
 * open a file and send fd on success
 * return -errno on error
 */
static int do_open(struct iovec *iovec)
{
    int flags, ret;
    V9fsString path;

    v9fs_string_init(&path);
    ret = proxy_unmarshal(iovec, PROXY_HDR_SZ, "sd", &path, &flags);
    if (ret < 0) {
        goto err_out;
    }
    ret = open(path.data, flags);
    if (ret < 0) {
    	LOGV("%s: Could not open file: %s", __func__, path.data);
        ret = -errno;
    }
err_out:
    v9fs_string_free(&path);
    return ret;
}

/* create unix domain socket and return the descriptor */
static int proxy_socket(const char *path, uid_t uid, gid_t gid)
{
    int sock, client;
    struct sockaddr_un proxy, qemu;
    socklen_t size;

    /* requested socket already exists, refuse to start */
    if (!access(path, F_OK)) {
        do_log(LOG_CRIT, "socket already exists\n");
        return -1;
    }

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        do_perror("socket");
        return -1;
    }

    /* mask other part of mode bits */
    umask(7);

    proxy.sun_family = AF_UNIX;
    strcpy(proxy.sun_path, path);
    if (bind(sock, (struct sockaddr *)&proxy,
            sizeof(struct sockaddr_un)) < 0) {
        do_perror("bind");
        return -1;
    }
    if (chown(proxy.sun_path, uid, gid) < 0) {
        do_perror("chown");
        return -1;
    }
    if (listen(sock, 1) < 0) {
        do_perror("listen");
        return -1;
    }

    client = accept(sock, (struct sockaddr *)&qemu, &size);
    if (client < 0) {
        do_perror("accept");
        return -1;
    }
    return client;
}

static void usage(char *prog)
{
    fprintf(stderr, "usage: %s\n"
            " -p|--path <path> 9p path to export\n"
            " {-f|--fd <socket-descriptor>} socket file descriptor to be used\n"
            " {-s|--socket <socketname> socket file used for communication\n"
            " \t-u|--uid <uid> -g|--gid <gid>} - uid:gid combination to give "
            " access to this socket\n"
            " \tNote: -s & -f can not be used together\n"
            " [-n|--nodaemon] Run as a normal program\n",
            basename(prog));
}

static int process_reply(int sock, int type,
                         struct iovec *out_iovec, int retval)
{
    switch (type) {
    case T_OPEN:
    case T_CREATE:
        if (send_fd(sock, retval) < 0) {
            return -1;
        }
        break;
    case T_MKNOD:
    case T_MKDIR:
    case T_SYMLINK:
    case T_LINK:
    case T_CHMOD:
    case T_CHOWN:
    case T_TRUNCATE:
    case T_UTIME:
    case T_RENAME:
    case T_REMOVE:
    case T_LSETXATTR:
    case T_LREMOVEXATTR:
        if (send_status(sock, out_iovec, retval) < 0) {
            return -1;
        }
        break;
    case T_LSTAT:
    case T_STATFS:
    case T_READLINK:
    case T_LGETXATTR:
    case T_LLISTXATTR:
    case T_GETVERSION:
        if (send_response(sock, out_iovec, retval) < 0) {
            return -1;
        }
        break;
    default:
        return -1;
        break;
    }
    return 0;
}

static int process_requests(int sock)
{
    int flags;
    int size = 0;
    int retval = 0;
    uint64_t offset;
    ProxyHeader header;
    int mode, uid, gid;
    V9fsString name, value;
    struct timespec spec[2];
    V9fsString oldpath, path;
    struct iovec in_iovec, out_iovec;

    in_iovec.iov_base  = g_malloc(PROXY_MAX_IO_SZ + PROXY_HDR_SZ);
    in_iovec.iov_len   = PROXY_MAX_IO_SZ + PROXY_HDR_SZ;
    out_iovec.iov_base = g_malloc(PROXY_MAX_IO_SZ + PROXY_HDR_SZ);
    out_iovec.iov_len  = PROXY_MAX_IO_SZ + PROXY_HDR_SZ;

    while (1) {
        /*
         * initialize the header type, so that we send
         * response to proper request type.
         */
        header.type = 0;
        retval = read_request(sock, &in_iovec, &header);
        if (retval < 0) {
            goto err_out;
        }

        switch (header.type) {
        case T_OPEN:
            retval = do_open(&in_iovec);
            break;
        case T_CREATE:
            retval = do_create(&in_iovec);
            break;
        case T_MKNOD:
        case T_MKDIR:
        case T_SYMLINK:
            retval = do_create_others(header.type, &in_iovec);
            break;
        case T_LINK:
            v9fs_string_init(&path);
            v9fs_string_init(&oldpath);
            retval = proxy_unmarshal(&in_iovec, PROXY_HDR_SZ,
                                     "ss", &oldpath, &path);
            if (retval > 0) {
                retval = link(oldpath.data, path.data);
                if (retval < 0) {
                    retval = -errno;
                }
            }
            v9fs_string_free(&oldpath);
            v9fs_string_free(&path);
            break;
        case T_LSTAT:
        case T_STATFS:
            retval = do_stat(header.type, &in_iovec, &out_iovec);
            break;
        case T_READLINK:
            retval = do_readlink(&in_iovec, &out_iovec);
            break;
        case T_CHMOD:
            v9fs_string_init(&path);
            retval = proxy_unmarshal(&in_iovec, PROXY_HDR_SZ,
                                     "sd", &path, &mode);
            if (retval > 0) {
                retval = chmod(path.data, mode);
                if (retval < 0) {
                    retval = -errno;
                }
            }
            v9fs_string_free(&path);
            break;
        case T_CHOWN:
            v9fs_string_init(&path);
            retval = proxy_unmarshal(&in_iovec, PROXY_HDR_SZ, "sdd", &path,
                                     &uid, &gid);
            if (retval > 0) {
                retval = lchown(path.data, uid, gid);
                if (retval < 0) {
                    retval = -errno;
                }
            }
            v9fs_string_free(&path);
            break;
        case T_TRUNCATE:
            v9fs_string_init(&path);
            retval = proxy_unmarshal(&in_iovec, PROXY_HDR_SZ, "sq",
                                     &path, &offset);
            if (retval > 0) {
                retval = truncate(path.data, offset);
                if (retval < 0) {
                    retval = -errno;
                }
            }
            v9fs_string_free(&path);
            break;
        case T_UTIME:
            v9fs_string_init(&path);
            retval = proxy_unmarshal(&in_iovec, PROXY_HDR_SZ, "sqqqq", &path,
                                     &spec[0].tv_sec, &spec[0].tv_nsec,
                                     &spec[1].tv_sec, &spec[1].tv_nsec);
            if (retval > 0) {
                retval = qemu_utimens(path.data, spec);
                if (retval < 0) {
                    retval = -errno;
                }
            }
            v9fs_string_free(&path);
            break;
        case T_RENAME:
            v9fs_string_init(&path);
            v9fs_string_init(&oldpath);
            retval = proxy_unmarshal(&in_iovec, PROXY_HDR_SZ,
                                     "ss", &oldpath, &path);
            if (retval > 0) {
                retval = rename(oldpath.data, path.data);
                if (retval < 0) {
                    retval = -errno;
                }
            }
            v9fs_string_free(&oldpath);
            v9fs_string_free(&path);
            break;
        case T_REMOVE:
            v9fs_string_init(&path);
            retval = proxy_unmarshal(&in_iovec, PROXY_HDR_SZ, "s", &path);
            if (retval > 0) {
                retval = remove(path.data);
                if (retval < 0) {
                    retval = -errno;
                }
            }
            v9fs_string_free(&path);
            break;
        case T_LGETXATTR:
        case T_LLISTXATTR:
            retval = do_getxattr(header.type, &in_iovec, &out_iovec);
            break;
        case T_LSETXATTR:
            v9fs_string_init(&path);
            v9fs_string_init(&name);
            v9fs_string_init(&value);
            retval = proxy_unmarshal(&in_iovec, PROXY_HDR_SZ, "sssdd", &path,
                                     &name, &value, &size, &flags);
            if (retval > 0) {
                retval = lsetxattr(path.data,
                                   name.data, value.data, size, flags);
                if (retval < 0) {
                    retval = -errno;
                }
            }
            v9fs_string_free(&path);
            v9fs_string_free(&name);
            v9fs_string_free(&value);
            break;
        case T_LREMOVEXATTR:
            v9fs_string_init(&path);
            v9fs_string_init(&name);
            retval = proxy_unmarshal(&in_iovec,
                                     PROXY_HDR_SZ, "ss", &path, &name);
            if (retval > 0) {
                retval = lremovexattr(path.data, name.data);
                if (retval < 0) {
                    retval = -errno;
                }
            }
            v9fs_string_free(&path);
            v9fs_string_free(&name);
            break;
        case T_GETVERSION:
            retval = do_getversion(&in_iovec, &out_iovec);
            break;
        default:
            goto err_out;
            break;
        }

        if (process_reply(sock, header.type, &out_iovec, retval) < 0) {
            goto err_out;
        }
    }
err_out:
    g_free(in_iovec.iov_base);
    g_free(out_iovec.iov_base);
    return -1;
}

int main(int argc, char **argv)
{
    int sock;
    uid_t own_u;
    gid_t own_g;
    char *rpath = NULL;
    char *sock_name = NULL;
    struct stat stbuf;
    int c, option_index;
#ifdef FS_IOC_GETVERSION
    int retval;
    struct statfs st_fs;
#endif

    is_daemon = true;
    sock = -1;
    own_u = own_g = -1;
    while (1) {
        option_index = 0;
        c = getopt_long(argc, argv, "p:nh?f:s:u:g:", helper_opts,
                        &option_index);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'p':
            rpath = strdup(optarg);
            break;
        case 'n':
            is_daemon = false;
            break;
        case 'f':
            sock = atoi(optarg);
            break;
        case 's':
            sock_name = strdup(optarg);
            break;
        case 'u':
            own_u = atoi(optarg);
            break;
        case 'g':
            own_g = atoi(optarg);
            break;
        case '?':
        case 'h':
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    /* Parameter validation */
    if ((sock_name == NULL && sock == -1) || rpath == NULL) {
        fprintf(stderr, "socket, socket descriptor or path not specified\n");
        usage(argv[0]);
        return -1;
    }

    if (sock_name && sock != -1) {
        fprintf(stderr, "both named socket and socket descriptor specified\n");
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (sock_name && (own_u == -1 || own_g == -1)) {
        fprintf(stderr, "owner uid:gid not specified, ");
        fprintf(stderr,
                "owner uid:gid specifies who can access the socket file\n");
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (lstat(rpath, &stbuf) < 0) {
        fprintf(stderr, "invalid path \"%s\" specified, %s\n",
                rpath, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISDIR(stbuf.st_mode)) {
        fprintf(stderr, "specified path \"%s\" is not directory\n", rpath);
        exit(EXIT_FAILURE);
    }

    if (is_daemon) {
        if (daemon(0, 0) < 0) {
            fprintf(stderr, "daemon call failed\n");
            exit(EXIT_FAILURE);
        }
        openlog(PROGNAME, LOG_PID, LOG_DAEMON);
    }

    do_log(LOG_INFO, "Started\n");
    if (sock_name) {
        sock = proxy_socket(sock_name, own_u, own_g);
        if (sock < 0) {
            goto error;
        }
    }

    get_version = false;
#ifdef FS_IOC_GETVERSION
    /* check whether underlying FS support IOC_GETVERSION */
    retval = statfs(rpath, &st_fs);
    if (!retval) {
        switch (st_fs.f_type) {
        case EXT2_SUPER_MAGIC:
        case BTRFS_SUPER_MAGIC:
        case REISERFS_SUPER_MAGIC:
        case XFS_SUPER_MAGIC:
            get_version = true;
            break;
        }
    }
#endif

    if (chdir("/") < 0) {
        do_perror("chdir");
        goto error;
    }
    if (chroot(rpath) < 0) {
        do_perror("chroot");
        goto error;
    }
    umask(0);

    if (init_capabilities() < 0) {
        goto error;
    }

    process_requests(sock);
error:
    do_log(LOG_INFO, "Done\n");
    closelog();
    return 0;
}
