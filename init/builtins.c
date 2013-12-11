/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <linux/kd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <linux/loop.h>
#include <cutils/partition_utils.h>
#include <cutils/android_reboot.h>
#include <sys/system_properties.h>
#include <fs_mgr.h>

#include <selinux/selinux.h>
#include <selinux/label.h>

#include "init.h"
#include "keywords.h"
#include "property_service.h"
#include "devices.h"
#include "init_parser.h"
#include "util.h"
#include "log.h"
#include "libubi.h"

#include "make_ext4fs.h"

#include <private/android_filesystem_config.h>

#define DEFAULT_CTRL_DEV "/dev/ubi_ctrl"

void add_environment(const char *name, const char *value);

extern int init_module(void *, unsigned long, const char *);
extern int e2fsck_main(int argc, char *argv[]);

static int write_file(const char *path, const char *value)
{
    int fd, ret, len;

    fd = open(path, O_WRONLY|O_CREAT|O_NOFOLLOW, 0600);

    if (fd < 0)
        return -errno;

    len = strlen(value);

    do {
        ret = write(fd, value, len);
    } while (ret < 0 && errno == EINTR);

    close(fd);
    if (ret < 0) {
        return -errno;
    } else {
        return 0;
    }
}

static int _open(const char *path)
{
    int fd;

    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        fd = open(path, O_WRONLY | O_NOFOLLOW);

    return fd;
}

static int _chown(const char *path, unsigned int uid, unsigned int gid)
{
    int fd;
    int ret;

    fd = _open(path);
    if (fd < 0) {
        return -1;
    }

    ret = fchown(fd, uid, gid);
    if (ret < 0) {
        int errno_copy = errno;
        close(fd);
        errno = errno_copy;
        return -1;
    }

    close(fd);

    return 0;
}

static int _chmod(const char *path, mode_t mode)
{
    int fd;
    int ret;

    fd = _open(path);
    if (fd < 0) {
        return -1;
    }

    ret = fchmod(fd, mode);
    if (ret < 0) {
        int errno_copy = errno;
        close(fd);
        errno = errno_copy;
        return -1;
    }

    close(fd);

    return 0;
}

static int insmod(const char *filename, char *options)
{
    void *module;
    unsigned size;
    int ret;

    module = read_file(filename, &size);
    if (!module)
        return -1;

    ret = init_module(module, size, options);

    free(module);

    return ret;
}

static int setkey(struct kbentry *kbe)
{
    int fd, ret;

    fd = open("/dev/tty0", O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;

    ret = ioctl(fd, KDSKBENT, kbe);

    close(fd);
    return ret;
}

static int __ifupdown(const char *interface, int up)
{
    struct ifreq ifr;
    int s, ret;

    strlcpy(ifr.ifr_name, interface, IFNAMSIZ);

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;

    ret = ioctl(s, SIOCGIFFLAGS, &ifr);
    if (ret < 0) {
        goto done;
    }

    if (up)
        ifr.ifr_flags |= IFF_UP;
    else
        ifr.ifr_flags &= ~IFF_UP;

    ret = ioctl(s, SIOCSIFFLAGS, &ifr);
    
done:
    close(s);
    return ret;
}

static void service_start_if_not_disabled(struct service *svc)
{
    if (!(svc->flags & SVC_DISABLED)) {
        service_start(svc, NULL);
    }
}

int do_chdir(int nargs, char **args)
{
    chdir(args[1]);
    return 0;
}

int do_chroot(int nargs, char **args)
{
    chroot(args[1]);
    return 0;
}

int do_class_start(int nargs, char **args)
{
        /* Starting a class does not start services
         * which are explicitly disabled.  They must
         * be started individually.
         */
    service_for_each_class(args[1], service_start_if_not_disabled);
    return 0;
}

int do_class_stop(int nargs, char **args)
{
    service_for_each_class(args[1], service_stop);
    return 0;
}

int do_class_reset(int nargs, char **args)
{
    service_for_each_class(args[1], service_reset);
    return 0;
}

int do_domainname(int nargs, char **args)
{
    return write_file("/proc/sys/kernel/domainname", args[1]);
}

int do_exec(int nargs, char **args)
{
    return -1;
}

int do_export(int nargs, char **args)
{
    add_environment(args[1], args[2]);
    return 0;
}

int do_hostname(int nargs, char **args)
{
    return write_file("/proc/sys/kernel/hostname", args[1]);
}

int do_ifup(int nargs, char **args)
{
    return __ifupdown(args[1], 1);
}


static int do_insmod_inner(int nargs, char **args, int opt_len)
{
    char options[opt_len + 1];
    int i;

    options[0] = '\0';
    if (nargs > 2) {
        strcpy(options, args[2]);
        for (i = 3; i < nargs; ++i) {
            strcat(options, " ");
            strcat(options, args[i]);
        }
    }

    return insmod(args[1], options);
}

int do_insmod(int nargs, char **args)
{
    int i;
    int size = 0;

    if (nargs > 2) {
        for (i = 2; i < nargs; ++i)
            size += strlen(args[i]) + 1;
    }

    return do_insmod_inner(nargs, args, size);
}

int do_mkdir(int nargs, char **args)
{
    mode_t mode = 0755;
    int ret;

    /* mkdir <path> [mode] [owner] [group] */

    if (nargs >= 3) {
        mode = strtoul(args[2], 0, 8);
    }

    ret = make_dir(args[1], mode);
    /* chmod in case the directory already exists */
    if (ret == -1 && errno == EEXIST) {
        ret = _chmod(args[1], mode);
    }
    if (ret == -1) {
        return -errno;
    }

    if (nargs >= 4) {
        uid_t uid = decode_uid(args[3]);
        gid_t gid = -1;

        if (nargs == 5) {
            gid = decode_uid(args[4]);
        }

        if (_chown(args[1], uid, gid) < 0) {
            return -errno;
        }

        /* chown may have cleared S_ISUID and S_ISGID, chmod again */
        if (mode & (S_ISUID | S_ISGID)) {
            ret = _chmod(args[1], mode);
            if (ret == -1) {
                return -errno;
            }
        }
    }

    return 0;
}

static struct {
    const char *name;
    unsigned flag;
} mount_flags[] = {
    { "noatime",    MS_NOATIME },
    { "noexec",     MS_NOEXEC },
    { "nosuid",     MS_NOSUID },
    { "nodev",      MS_NODEV },
    { "nodiratime", MS_NODIRATIME },
    { "ro",         MS_RDONLY },
    { "rw",         0 },
    { "remount",    MS_REMOUNT },
    { "bind",       MS_BIND },
    { "rec",        MS_REC },
    { "unbindable", MS_UNBINDABLE },
    { "private",    MS_PRIVATE },
    { "slave",      MS_SLAVE },
    { "shared",     MS_SHARED },
    { "defaults",   0 },
    { 0,            0 },
};

#define DATA_MNT_POINT "/data"

/* mount <type> <device> <path> <flags ...> <options> */
int do_mount(int nargs, char **args)
{
    char tmp[64];
    char *source, *target, *system;
    char *options = NULL;
    unsigned flags = 0;
    int n, i;
    int wait = 0;

    for (n = 4; n < nargs; n++) {
        for (i = 0; mount_flags[i].name; i++) {
            if (!strcmp(args[n], mount_flags[i].name)) {
                flags |= mount_flags[i].flag;
                break;
            }
        }

        if (!mount_flags[i].name) {
            if (!strcmp(args[n], "wait"))
                wait = 1;
            /* if our last argument isn't a flag, wolf it up as an option string */
            else if (n + 1 == nargs)
                options = args[n];
        }
    }

    system = args[1];
    source = args[2];
    target = args[3];

    if (!strncmp(source, "mtd@", 4)) {
        n = mtd_name_to_number(source + 4);
        if (n < 0) {
            return -1;
        }

        sprintf(tmp, "/dev/block/mtdblock%d", n);

        if (wait)
            wait_for_file(tmp, COMMAND_RETRY_TIMEOUT);
        if (mount(tmp, target, system, flags, options) < 0) {
            return -1;
        }

        goto exit_success;
    } else if (!strncmp(system, "ubifs", 5)) {
        if (mount(source, target, system, flags, options) < 0) {
        	printf("ubifs mount failed and remount here\n");
        	if (mount(source, target, system, flags, options) < 0) {
            		return -1;
            	}
        }
        goto exit_success;
    } else if (!strncmp(source, "loop@", 5)) {
        int mode, loop, fd;
        struct loop_info info;

        mode = (flags & MS_RDONLY) ? O_RDONLY : O_RDWR;
        fd = open(source + 5, mode);
        if (fd < 0) {
            return -1;
        }

        for (n = 0; ; n++) {
            sprintf(tmp, "/dev/block/loop%d", n);
            loop = open(tmp, mode);
            if (loop < 0) {
                return -1;
            }

            /* if it is a blank loop device */
            if (ioctl(loop, LOOP_GET_STATUS, &info) < 0 && errno == ENXIO) {
                /* if it becomes our loop device */
                if (ioctl(loop, LOOP_SET_FD, fd) >= 0) {
                    close(fd);

                    if (mount(tmp, target, system, flags, options) < 0) {
                        ioctl(loop, LOOP_CLR_FD, 0);
                        close(loop);
                        return -1;
                    }

                    close(loop);
                    goto exit_success;
                }
            }

            close(loop);
        }

        close(fd);
        ERROR("out of loopback devices");
        return -1;
    } else {
        if (wait)
            wait_for_file(source, COMMAND_RETRY_TIMEOUT);

        if (!strncmp(source, "inand@", 6)) {
            do{
                n = inand_name_to_number(source + 6);
                INFO("inand_name_to_number: %d\n" , n);
                usleep(200000);
                //sched_yield();
            }while(n < 0);

            sprintf(tmp, "/dev/block/cardblkinand%d", n);
        }else{
            strcpy(tmp,  source);
        }

	//ERROR("try mount %s to target %s\n", tmp, target);

	int mount_result = mount(tmp, target, system, flags, options);
        if ( mount_result < 0){
            ERROR("mount %s to target failed\n", tmp, target );
        }

	//if mount cache fail,then format the cache and mount cache again
	if( mount_result < 0 && strncmp(system, "ext4", 4) == 0 && strncmp(target, "/cache", 6) == 0 ) 
	{
		ERROR("mount cache fail,try format\n");
		
		int result = -1;

		//format cache
#ifdef HAVE_SELINUX
                result = make_ext4fs(tmp, 0, target, sehandle);
#else
                result = make_ext4fs(tmp, 0, target, NULL);
#endif

		if (result != 0) {
                        ERROR("format_volume: make_extf4fs failed on %s, err[%s]\n", tmp, strerror(errno) );
                }

		//mount after format
		result = mount(tmp, target, system, flags, options);
                if (result) {
                        ERROR("re-mount failed on %s, %s, %s, flag=0x%x, err[%s]\n", tmp, target, system, flags, strerror(errno) );
                        return -2;
                }
	}

        //if mount data fail,then set prop ro.init.mountdatafail to true,for notify user data has been destory
        if( mount_result < 0 && strncmp(target, "/data", 5) == 0 )
        {
            char mountdata_value[PROP_VALUE_MAX];
            int ret;
            ret = property_get("ro.init.mountdatafail", mountdata_value);
            ERROR("mount data fail,set prop,mountdata_value:%s\n", (ret ? "" : mountdata_value ));
            property_set("ro.init.mountdatafail", "true");
        }
    }

exit_success:
    return 0;

}

int do_mount_all(int nargs, char **args)
{
    pid_t pid;
    int ret = -1;
    int child_ret = -1;
    int status;
    const char *prop;
    struct fstab *fstab;

    if (nargs != 2) {
        return -1;
    }

    /*
     * Call fs_mgr_mount_all() to mount all filesystems.  We fork(2) and
     * do the call in the child to provide protection to the main init
     * process if anything goes wrong (crash or memory leak), and wait for
     * the child to finish in the parent.
     */
    pid = fork();
    if (pid > 0) {
        /* Parent.  Wait for the child to return */
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            ret = WEXITSTATUS(status);
        } else {
            ret = -1;
        }
    } else if (pid == 0) {
        /* child, call fs_mgr_mount_all() */
        klog_set_level(6);  /* So we can see what fs_mgr_mount_all() does */
        fstab = fs_mgr_read_fstab(args[1]);
        child_ret = fs_mgr_mount_all(fstab);
        fs_mgr_free_fstab(fstab);
        if (child_ret == -1) {
            ERROR("fs_mgr_mount_all returned an error\n");
        }
        exit(child_ret);
    } else {
        /* fork failed, return an error */
        return -1;
    }

    /* ret is 1 if the device is encrypted, 0 if not, and -1 on error */
    if (ret == 1) {
        property_set("ro.crypto.state", "encrypted");
        property_set("vold.decrypt", "1");
    } else if (ret == 0) {
        property_set("ro.crypto.state", "unencrypted");
        /* If fs_mgr determined this is an unencrypted device, then trigger
         * that action.
         */
        action_for_each_trigger("nonencrypted", action_add_queue_tail);
    }

    return ret;
}

int do_swapon_all(int nargs, char **args)
{
    struct fstab *fstab;
    int ret;

    fstab = fs_mgr_read_fstab(args[1]);
    ret = fs_mgr_swapon_all(fstab);
    fs_mgr_free_fstab(fstab);

    return ret;
}

int do_setcon(int nargs, char **args) {
    if (is_selinux_enabled() <= 0)
        return 0;
    if (setcon(args[1]) < 0) {
        return -errno;
    }
    return 0;
}

int do_setenforce(int nargs, char **args) {
    if (is_selinux_enabled() <= 0)
        return 0;
    if (security_setenforce(atoi(args[1])) < 0) {
        return -errno;
    }
    return 0;
}

int do_setkey(int nargs, char **args)
{
    struct kbentry kbe;
    kbe.kb_table = strtoul(args[1], 0, 0);
    kbe.kb_index = strtoul(args[2], 0, 0);
    kbe.kb_value = strtoul(args[3], 0, 0);
    return setkey(&kbe);
}

int do_setprop(int nargs, char **args)
{
    const char *name = args[1];
    const char *value = args[2];
    char prop_val[PROP_VALUE_MAX];
    int ret;

    ret = expand_props(prop_val, value, sizeof(prop_val));
    if (ret) {
        ERROR("cannot expand '%s' while assigning to '%s'\n", value, name);
        return -EINVAL;
    }
    property_set(name, prop_val);
    return 0;
}

int do_setrlimit(int nargs, char **args)
{
    struct rlimit limit;
    int resource;
    resource = atoi(args[1]);
    limit.rlim_cur = atoi(args[2]);
    limit.rlim_max = atoi(args[3]);
    return setrlimit(resource, &limit);
}

int do_start(int nargs, char **args)
{
    struct service *svc;
    svc = service_find_by_name(args[1]);
    if (svc) {
        service_start(svc, NULL);
    }
    return 0;
}

int do_stop(int nargs, char **args)
{
    struct service *svc;
    svc = service_find_by_name(args[1]);
    if (svc) {
        service_stop(svc);
    }
    return 0;
}

int do_restart(int nargs, char **args)
{
    struct service *svc;
    svc = service_find_by_name(args[1]);
    if (svc) {
        service_restart(svc);
    }
    return 0;
}

int do_powerctl(int nargs, char **args)
{
    char command[PROP_VALUE_MAX];
    int res;
    int len = 0;
    int cmd = 0;
    char *reboot_target;

    res = expand_props(command, args[1], sizeof(command));
    if (res) {
        ERROR("powerctl: cannot expand '%s'\n", args[1]);
        return -EINVAL;
    }

    if (strncmp(command, "shutdown", 8) == 0) {
        cmd = ANDROID_RB_POWEROFF;
        len = 8;
    } else if (strncmp(command, "reboot", 6) == 0) {
        cmd = ANDROID_RB_RESTART2;
        len = 6;
    } else {
        ERROR("powerctl: unrecognized command '%s'\n", command);
        return -EINVAL;
    }

    if (command[len] == ',') {
        reboot_target = &command[len + 1];
    } else if (command[len] == '\0') {
        reboot_target = "";
    } else {
        ERROR("powerctl: unrecognized reboot target '%s'\n", &command[len]);
        return -EINVAL;
    }

    return android_reboot(cmd, 0, reboot_target);
}

int do_trigger(int nargs, char **args)
{
    action_for_each_trigger(args[1], action_add_queue_tail);
    return 0;
}

int do_symlink(int nargs, char **args)
{
    return symlink(args[1], args[2]);
}

int do_rm(int nargs, char **args)
{
    return unlink(args[1]);
}

int do_rmdir(int nargs, char **args)
{
    return rmdir(args[1]);
}

int do_sysclktz(int nargs, char **args)
{
    struct timezone tz;

    if (nargs != 2)
        return -1;

    memset(&tz, 0, sizeof(tz));
    tz.tz_minuteswest = atoi(args[1]);   
    if (settimeofday(NULL, &tz))
        return -1;
    return 0;
}

int do_write(int nargs, char **args)
{
    const char *path = args[1];
    const char *value = args[2];
    char prop_val[PROP_VALUE_MAX];
    int ret;

    ret = expand_props(prop_val, value, sizeof(prop_val));
    if (ret) {
        ERROR("cannot expand '%s' while writing to '%s'\n", value, path);
        return -EINVAL;
    }
    return write_file(path, prop_val);
}

int do_copy(int nargs, char **args)
{
    char *buffer = NULL;
    int rc = 0;
    int fd1 = -1, fd2 = -1;
    struct stat info;
    int brtw, brtr;
    char *p;

    if (nargs != 3)
        return -1;

    if (stat(args[1], &info) < 0) 
        return -1;

    if ((fd1 = open(args[1], O_RDONLY)) < 0) 
        goto out_err;

    if ((fd2 = open(args[2], O_WRONLY|O_CREAT|O_TRUNC, 0660)) < 0)
        goto out_err;

    if (!(buffer = malloc(info.st_size)))
        goto out_err;

    p = buffer;
    brtr = info.st_size;
    while(brtr) {
        rc = read(fd1, p, brtr);
        if (rc < 0)
            goto out_err;
        if (rc == 0)
            break;
        p += rc;
        brtr -= rc;
    }

    p = buffer;
    brtw = info.st_size;
    while(brtw) {
        rc = write(fd2, p, brtw);
        if (rc < 0)
            goto out_err;
        if (rc == 0)
            break;
        p += rc;
        brtw -= rc;
    }

    rc = 0;
    goto out;
out_err:
    rc = -1;
out:
    if (buffer)
        free(buffer);
    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
    return rc;
}

int do_chown(int nargs, char **args) {
    /* GID is optional. */
    if (nargs == 3) {
        if (_chown(args[2], decode_uid(args[1]), -1) < 0)
            return -errno;
    } else if (nargs == 4) {
        if (_chown(args[3], decode_uid(args[1]), decode_uid(args[2])) < 0)
            return -errno;
    } else {
        return -1;
    }
    return 0;
}

static mode_t get_mode(const char *s) {
    mode_t mode = 0;
    while (*s) {
        if (*s >= '0' && *s <= '7') {
            mode = (mode<<3) | (*s-'0');
        } else {
            return -1;
        }
        s++;
    }
    return mode;
}

int do_chmod(int nargs, char **args) {
    mode_t mode = get_mode(args[1]);
    if (_chmod(args[2], mode) < 0) {
        return -errno;
    }
    return 0;
}

int do_restorecon(int nargs, char **args) {
    int i;

    for (i = 1; i < nargs; i++) {
        if (restorecon(args[i]) < 0)
            return -errno;
    }
    return 0;
}

int do_setsebool(int nargs, char **args) {
    const char *name = args[1];
    const char *value = args[2];
    SELboolean b;
    int ret;

    if (is_selinux_enabled() <= 0)
        return 0;

    b.name = name;
    if (!strcmp(value, "1") || !strcasecmp(value, "true") || !strcasecmp(value, "on"))
        b.value = 1;
    else if (!strcmp(value, "0") || !strcasecmp(value, "false") || !strcasecmp(value, "off"))
        b.value = 0;
    else {
        ERROR("setsebool: invalid value %s\n", value);
        return -EINVAL;
    }

    if (security_set_boolean_list(1, &b, 0) < 0) {
        ret = -errno;
        ERROR("setsebool: could not set %s to %s\n", name, value);
        return ret;
    }

    return 0;
}

int do_loglevel(int nargs, char **args) {
    if (nargs == 2) {
        klog_set_level(atoi(args[1]));
        return 0;
    }
    return -1;
}

int do_load_persist_props(int nargs, char **args) {
    if (nargs == 1) {
        load_persist_props();
        return 0;
    }
    return -1;
}

int do_wait(int nargs, char **args)
{
    if (nargs == 2) {
        return wait_for_file(args[1], COMMAND_RETRY_TIMEOUT);
    } else if (nargs == 3) {
        return wait_for_file(args[1], atoi(args[2]));
    } else
        return -1;
}
int do_ubiattach(int argc, char **args)
{
    INFO("\n=== do_ubiattach start ===\n");
    int err;
    libubi_t libubi;
    struct ubi_info ubi_info;
    struct ubi_dev_info dev_info;
    struct ubi_attach_request req;
    char *target;
    int n;

    target = args[1];
    if (!strncmp(target, "mtd@", 4)) {
        n = mtd_name_to_number(target + 4);
        if (n < 0) {
            INFO("do_ubiattach got wrong target(%s)\n", target);
            return -1;
        }
    }
    else{
        INFO("do_ubiattach got wrong target(%s)\n", target);
        return -1;
    }


    libubi = libubi_open();
    if (!libubi) {
        INFO("do_ubiattach open fail");
        return -1;
    }

    /*
     * Make sure the kernel is fresh enough and this feature is supported.
     */
    err = ubi_get_info(libubi, &ubi_info);
    if (err) {
        INFO("cannot get UBI information\n");
        goto out_libubi;
    }

    if (ubi_info.ctrl_major == -1) {
        INFO("MTD attach/detach feature is not supported by your kernel");
        goto out_libubi;
    }

    req.dev_num = UBI_DEV_NUM_AUTO;
    req.mtd_num = n;
    req.vid_hdr_offset = 0;
    req.mtd_dev_node = NULL;

    err = ubi_attach(libubi, DEFAULT_CTRL_DEV, &req);
    if (err) {
        INFO("cannot attach mtd%d", n);
        goto out_libubi;
    }

    /* Print some information about the new UBI device */
    /*
    err = ubi_get_dev_info1(libubi, req.dev_num, &dev_info);
    if (err) {
        INFO("cannot get information about newly created UBI device");
        goto out_libubi;
    }

    printf("UBI device number %d, total %d LEBs (", dev_info.dev_num, dev_info.total_lebs);
    ubiutils_print_bytes(dev_info.total_bytes, 0);
    printf("), available %d LEBs (", dev_info.avail_lebs);
    ubiutils_print_bytes(dev_info.avail_bytes, 0);
    printf("), LEB size ");
    ubiutils_print_bytes(dev_info.leb_size, 1);
    printf("\n");
    */

    libubi_close(libubi);
    return 0;

out_libubi:
    libubi_close(libubi);
    return -1;
}

int do_ubidetach(int argc, char **args)
{
    int err;
    libubi_t libubi;
    struct ubi_info ubi_info;

    char *target;
    int mtdn = -1, devn = -1;
    char *dev = NULL;

    target = args[1];
    if (!strncmp(target, "mtd@", 4)) {
        mtdn = mtd_name_to_number(target + 4);
        if (mtdn < 0) {
            INFO("do_ubiattach got wrong target(%s)\n", target);
            return -1;
        }
    }
    else if (!strncmp(target, "devn@", 5)){
        devn = atoi(target + 5);
        if (devn < 0) {
            INFO("do_ubiattach got wrong target(%s)\n", target);
            return -1;
        }
    }
    else if (!strncmp(target, "dev@", 4)){
        dev = target + 4;
    }
    else{
        INFO("do_ubiattach got wrong target(%s)\n", target);
        return -1;
    }


    libubi = libubi_open();
    if (!libubi) {
        INFO("cannot open libubi");
        return -1;
    }

    /*
     * Make sure the kernel is fresh enough and this feature is supported.
     */
    err = ubi_get_info(libubi, &ubi_info);
    if (err) {
        INFO("cannot get UBI information");
        goto out_libubi;
    }

    if (ubi_info.ctrl_major == -1) {
        INFO("MTD detach/detach feature is not supported by your kernel");
        goto out_libubi;
    }

    if (devn != -1) {
        err = ubi_remove_dev(libubi, DEFAULT_CTRL_DEV, devn);
        if (err) {
            INFO("cannot remove ubi%d", devn);
            goto out_libubi;
        }
    }
    else if (mtdn != -1) {
        err = ubi_detach_mtd(libubi, DEFAULT_CTRL_DEV, mtdn);
        if (err) {
            INFO("cannot detach mtd%d", mtdn);
            goto out_libubi;
        }
    }
    else if (dev != NULL) {
        err = ubi_detach(libubi, DEFAULT_CTRL_DEV, dev);
        if (err) {
            INFO("cannot detach \"%s\"", dev);
            goto out_libubi;
        }
    }

    libubi_close(libubi);
    return 0;

out_libubi:
    libubi_close(libubi);
    return -1;
}
int do_e2fsck(int nargs, char **args) {

    if (nargs == 3) {
        ERROR("Before e2fsck_main...\n");

        e2fsck_main(nargs, args);

        ERROR("After e2fsck_main...\n");

    } else {
        ERROR("e2fsck bad args %d.", nargs);
    }

    return 0;
}

int do_confirm_formated(int nargs, char **args) {
    //ERROR("enter do_confirm_formated\n");
    int flags = MS_NOATIME | MS_NODIRATIME | MS_NOSUID | MS_NODEV;
    char options[] = "noauto_da_alloc";

    if( !strncmp( args[1], "ext4", 4 ) ) {
	//ERROR("do_confirm_formated ext4\n");
	int result = mount(args[2], args[3], "ext4", flags, options);

	if ( result != 0 ){
		ERROR("do_confirm_formated mount fail,maybe firstboot, need format, try format now\n");
                int fd = -1;

#ifdef HAVE_SELINUX
                result = make_ext4fs(args[2], 0, args[3], sehandle);
#else
                result = make_ext4fs(args[2], 0, args[3], NULL);
#endif
                if (result != 0) {
		    ERROR("do_confirm_formated mount make_extf4fs fail on %s, err[%s]\n", args[2], strerror(errno));
                    return -1;
                }

                fd = open(args[2], O_RDWR);
                if(fd > 0){
                    fsync(fd);
                    close(fd);//sync to fs
                }

                //just try
                result = mount(args[2], args[3], "ext4", flags, options);
                if( result != 0 ) {
                    ERROR("do_confirm_formated re-mount failed on %s, %s, err[%s]\n", args[2], args[3], strerror(errno) );
                    return -2;
                }
	}
	
	if( result == 0 ) {
		result = umount(args[3]);
    		if(result != 0) {
        		ERROR("do_confirm_formated, umount fail!\n");
    		}	
	}
    }  
 
    return 0;
}

int do_display_logo(int nargs, char **args) {
    //ERROR("do_display_logo\n");
     
    int result = load_565rle_image_ex(args[1]);
    if(result != 0) {
        ERROR("do_display_logo load image fail\n");
    }

    return result;
}


