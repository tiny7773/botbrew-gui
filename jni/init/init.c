#include <libgen.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <alloca.h>
#include <malloc.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <mntent.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <linux/fs.h>
#include <linux/loop.h>

#include "strnstr.h"

#define ENV_PATH	"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/games:/usr/games:/botbrew/bin:/usr/lib/busybox"
#define LOOP_MAX	4096

struct mountspec {
	const char *src;
	const char *dst;
	const char *type;
	unsigned long flags;
	const void *data;
	unsigned long remount_flags;
};

static struct mountspec foreign_mounts[] = {
	{NULL,"/proc","proc",0,NULL,0},
	{"/dev","/dev",NULL,MS_BIND|MS_REC,NULL,0},
	{NULL,"/sys","sysfs",0,NULL,0},
	{NULL,"/run","tmpfs",0,"size=10%,mode=0755",0},
	{"/mnt","/mnt",NULL,MS_BIND|MS_REC,NULL,0},
	{NULL,"/android","tmpfs",MS_NODEV|MS_NOEXEC|MS_NOATIME,"size=1M,mode=0755",0},
	{"/cache","/android/cache",NULL,MS_BIND|MS_REC,NULL,0},
	{"/data","/android/data",NULL,MS_BIND|MS_REC,NULL,0},
	{"/datadata","/android/datadata",NULL,MS_BIND|MS_REC,NULL,0},
	{"/sd-ext","/android/sd-ext",NULL,MS_BIND|MS_REC,NULL,0},
	{"/system","/android/system",NULL,MS_BIND|MS_REC,NULL,MS_REMOUNT|MS_NODEV|MS_NOATIME},
	{NULL,NULL,NULL,0,NULL,0}
};

static struct mountspec local_mounts[] = {
	{"/etc","/botbrew/etc",NULL,MS_BIND|MS_REC,NULL,0},
	{"/home","/botbrew/home",NULL,MS_BIND|MS_REC,NULL,0},
	{"/root","/botbrew/root",NULL,MS_BIND|MS_REC,NULL,0},
	{"/var","/botbrew/var",NULL,MS_BIND|MS_REC,NULL,0},
	{"/usr/share","/botbrew/share",NULL,MS_BIND|MS_REC,NULL,0},
	{"/run","/botbrew/run",NULL,MS_BIND|MS_REC,NULL,0},
	{NULL,NULL,NULL,0,NULL,0}
};

typedef struct mntent_node {
	char *buf;
	struct mntent *mnt;
	struct mntent_node *next;
} mntent_node;

typedef struct mntent_dnode {
	char *buf;
	struct mntent *mnt;
	struct mntent_dnode *prev;
	struct mntent_dnode *next;
} mntent_dnode;

struct mntent *getmntent_r(FILE *f, struct mntent *mnt, char *linebuf, int buflen);

static void usage(char *progname) {
	fprintf(stderr,
		"Usage: %s [options] [--] [<command>...]\n"
		"\n"
		"Available options:\n"
		"\t-t <target>\t| --target=<target>\tSpecify chroot directory or image\n"
		"\t-r\t\t| --remount\t\tRemount chroot directory\n"
		"\t-u\t\t| --unmount\t\tUnmount chroot directory and exit\n",
	progname);
	exit(EXIT_FAILURE);
}

static void privdrop(void) {
	gid_t gid = getgid();
	uid_t uid = getuid();
	setgroups(1,&gid);
#ifdef linux
	setregid(gid,gid);
	setreuid(uid,uid);
#else
	setegid(gid);
	setgid(gid);
	seteuid(uid);
	setuid(uid);
#endif
}

static pid_t child_pid = 0;
static void sighandler(int signo) {
	if(child_pid != 0) kill(child_pid,signo);
}

static char *strconcat(const char *a, const char *b) {
	size_t len_a = strlen(a);
	size_t len_b = strlen(b);
	char *res = (char*)malloc(len_a+len_b+1);
	memcpy(res,a,len_a);
	memcpy(res+len_a,b,len_b+1);	// includes null terminator
	return res;
}

static void mkdir_p(const char *dir, mode_t mode) {
	char *tmp = (char*)malloc(PATH_MAX), *p;
	strcpy(tmp,dir);
	size_t len = strlen(tmp);
	while(tmp[len-1] == '/') {
		tmp[len-1] = 0;
		len--;
	}
	for(p = tmp+1; *p; p++) if(*p == '/') {
		*p = 0;
		mkdir(tmp,mode);
		*p = '/';
	}
	mkdir(tmp,mode);
	free(tmp);
}

static char *loopdev_get(const char *filepath) {
	char *devpath = (char*)malloc(PATH_MAX);
	struct loop_info64 loopinfo;
	mode_t mode = 0660 | S_IFBLK;
	int devfd = -1;
	int success = 0;
	int i;
	for(i = 0; i < LOOP_MAX; i++) {
		unsigned int dev = (0xff&i)|((i<<12)&0xfff00000)|(7<<8);
		sprintf(devpath,"/dev/block/loop%d",i);
		if((mknod(devpath,mode,dev) < 0)&&(errno != EEXIST)) break;
		if((devfd = open(devpath,O_RDWR)) < 0) break;
		if(ioctl(devfd,LOOP_GET_STATUS64,&loopinfo) < 0) {
			if(errno == ENXIO) success = 1;
			break;
		}
		close(devfd);
	}
	devpath = (char*)realloc(devpath,strlen(devpath)+1);
	int filefd = -1;
	if((filefd = open(filepath,O_RDWR)) < 0) {
		close(devfd);
		free(devpath);
		return NULL;
	}
	if(ioctl(devfd,LOOP_SET_FD,filefd) < 0) {
		close(filefd);
		close(devfd);
		free(devpath);
		return NULL;
	}
	memset(&loopinfo,0,sizeof(loopinfo));
	strlcpy((char*)loopinfo.lo_file_name,filepath,LO_NAME_SIZE);
	if(ioctl(devfd,LOOP_SET_STATUS64,&loopinfo) < 0) {
		close(filefd);
		close(devfd);
		free(devpath);
		return NULL;
	}
	close(filefd);
	close(devfd);
	return devpath;
}

static int loopdev_del(const char *devpath) {
	int devfd = open(devpath,O_RDONLY);
	if(devfd < 0) return -1;
	if(ioctl(devfd,LOOP_CLR_FD,0) < 0) {
		close(devfd);
		return -1;
	}
	close(devfd);
	return 0;
}

static int loopdev_mount(const char *source, const char *target, const char *filesystemtype, unsigned long mountflags, const void *data) {
	const char *devpath = loopdev_get(source);
	if(!devpath) return -1;
	int res = mount(devpath,target,filesystemtype,mountflags,data);
	if(res) loopdev_del(devpath);
	return res;
}

static int loopdev_umount2(const char *target, int flags) {
	FILE *mntfp = fopen("/proc/self/mounts","r");
	struct mntent *mntent;
	while(mntent = getmntent(mntfp)) {
		if(strcmp(mntent->mnt_dir,target) == 0) {
			int res = umount2(target,flags);
			loopdev_del(mntent->mnt_fsname);
			fclose(mntfp);
			return res;
		}
	}
	fclose(mntfp);
	return -1;
}

static void dynamic_remount(const char *src, const char *tmp) {
	FILE *fp = fopen("/proc/self/mounts","r");
	if(fp) {
		struct stat st;
		char *buf = (char*)malloc(PATH_MAX);
		char *src_real = realpath(src,buf);
		char *src_slash = strconcat(src_real,"/");
		size_t src_len = strlen(src_real);
		size_t src_slash_len = src_len+1;
		free(buf);
		mntent_dnode *head = NULL, *node = (mntent_dnode*)malloc(sizeof(mntent_dnode));
		mntent_dnode *tail = node;
		buf = node->buf = (char*)malloc(3*PATH_MAX);
		struct mntent *mnt = node->mnt = (struct mntent*)malloc(sizeof(struct mntent));
		// set up staging area
		mkdir(tmp,0755);
		mount(NULL,tmp,"tmpfs",0,"size=1M");
		mount(NULL,tmp,NULL,MS_PRIVATE,NULL);
		// build linked list of interesting mounts
		while(getmntent_r(fp,mnt,buf,3*PATH_MAX)) if(strncmp(mnt->mnt_dir,src_slash,src_slash_len) == 0) {
			node->mnt = mnt;
			node->buf = buf;
			node->next = head;
			if(head) head->prev = node;
			head = node;
			node = (mntent_dnode*)malloc(sizeof(mntent_dnode));
			buf = node->buf = (char*)malloc(3*PATH_MAX);
			mnt = node->mnt = (struct mntent*)malloc(sizeof(struct mntent));
		}
		fclose(fp);
		free(node->mnt);
		free(node->buf);
		free(node);
		free(src_slash);
		head->prev = 0;
		if(tail == node) tail = 0;
		// iterate while setting up bind mounts
		while(node = tail) {
			mnt = node->mnt;
			char *tmp_mnt = strconcat(tmp,mnt->mnt_dir+src_len);
			mkdir_p(tmp_mnt,0755);
			if(stat(mnt->mnt_dir,&st) == 0) chmod(tmp_mnt,st.st_mode);
			mount(NULL,mnt->mnt_dir,NULL,MS_SHARED,NULL);
			mount(mnt->mnt_dir,tmp_mnt,NULL,MS_BIND,NULL);
			mount(NULL,mnt->mnt_dir,NULL,MS_SLAVE,NULL);
			free(tmp_mnt);
			tail = node->prev;
		}
		// iterate while unmounting and cleaning up
		while(node = head) {
			mnt = node->mnt;
			umount2(mnt->mnt_dir,MNT_DETACH);
			head = node->next;
			free(node->mnt);
			free(node->buf);
			free(node);
		}
		// make sure subdirectories exist
		DIR *dp = opendir(src);
		if(dp) {
			struct dirent *ep;
			char *tmp_slash = strconcat(tmp,"/"), *tmp_path;
			while(ep = readdir(dp)) if((ep->d_name[0] != '.')&&(stat(ep->d_name,&st) == 0)) {
				tmp_path = strconcat(tmp_slash,ep->d_name);
				mkdir(tmp_path,st.st_mode);
				free(tmp_path);
			}
			closedir(dp);
			free(tmp_slash);
		}
		// commit staging area
		umount2(src,MNT_DETACH);
		mount(NULL,tmp,NULL,MS_SHARED|MS_REC,NULL);
		mount(tmp,src,NULL,MS_BIND|MS_REC,NULL);
		mount(NULL,tmp,NULL,MS_SLAVE|MS_REC,NULL);
		umount2(tmp,MNT_DETACH);
		rmdir(tmp);
	}
}

static void fix_mnt_symlink(const char *src, const char *dst, ...) {
	va_list args;
	va_start(args,dst);
	char *name, *dst_name, *src_name;
	while(name = va_arg(args,char*)) {
		dst_name = strconcat(dst,name);
		unlink(dst_name);
		src_name = strconcat(src,name);
		symlink(src_name,dst_name);
		free(src_name);
		free(dst_name);
	}
	va_end(args);
}

static void mount_setup(char *target, int loopdev) {
	// prepare self-mount
	if(!loopdev) mount(target,target,NULL,MS_BIND,NULL);
	mount(target,target,NULL,MS_REMOUNT|MS_NODEV|MS_NOATIME,NULL);
	int i = 0;
	struct stat st;
	char *dst;
	// prepare foreign mounts
	while(1) {
		struct mountspec m = foreign_mounts[i++];
		if(m.dst == NULL) break;
		if(m.src) {
			if(stat(m.src,&st) != 0) continue;
			if(m.flags&MS_BIND) mount(NULL,m.src,NULL,MS_SHARED|MS_REC,NULL);
		}
		dst = strconcat(target,m.dst);
		mkdir(dst,0755);
		mount(NULL,target,NULL,MS_UNBINDABLE,NULL);
		if(mount(m.src,dst,m.type,m.flags,m.data)) rmdir(dst);
		else {
			if(m.remount_flags) mount(dst,dst,NULL,m.remount_flags|MS_REMOUNT,NULL);
			mount(NULL,dst,NULL,MS_UNBINDABLE,NULL);
		}
		free(dst);
	}
	// share self mount
	mount(NULL,target,NULL,MS_SHARED|MS_REC,NULL);
	// make temporary directories
	dst = strconcat(target,"/run/tmp");
	mkdir(dst,01777);
	free(dst);
	dst = strconcat(target,"/run/lock");
	mkdir(dst,01777);
	free(dst);
	i = 0;
	char *src;
	// prepare local mounts
	while(1) {
		struct mountspec m = local_mounts[i++];
		if(m.dst == NULL) break;
		if(m.src) {
			src = strconcat(target,m.src);
			if(stat(src,&st) != 0) {
				free(src);
				continue;
			}
		} else src = NULL;
		dst = strconcat(target,m.dst);
		mkdir(dst,0755);
		if((src)&&(m.flags&MS_BIND)) mount(NULL,src,NULL,MS_SHARED|MS_REC,NULL);
		if(mount(src,dst,m.type,m.flags,m.data)) rmdir(dst);
		else if(m.remount_flags) mount(dst,dst,NULL,m.remount_flags|MS_REMOUNT,NULL);
		free(dst);
		free(src);
	}
}

static void mount_teardown(char *target, int loopdev) {
	FILE *fp = fopen("/proc/self/mounts","r");
	if(fp) {
		char *target_slash = strconcat(target,"/");
		size_t target_slash_len = strlen(target_slash);
		mntent_node *head = NULL, *node = (mntent_node*)malloc(sizeof(mntent_node));
		char *buf = node->buf = (char*)malloc(3*PATH_MAX);
		struct mntent *mnt = node->mnt = (struct mntent*)malloc(sizeof(struct mntent));
		// iterate, building a reversed linked list of interesting mounts
		while(mnt = getmntent_r(fp,mnt,buf,3*PATH_MAX)) {
			if(strncmp(mnt->mnt_dir,target_slash,target_slash_len) == 0) {
			} else if(strcmp(mnt->mnt_dir,target) == 0) {
			} else continue;
			mount(NULL,mnt->mnt_dir,NULL,MS_SLAVE,NULL);
			node->mnt = mnt;
			node->buf = buf;
			node->next = head;
			head = node;
			node = (mntent_node*)malloc(sizeof(mntent_node));
			buf = node->buf = (char*)malloc(3*PATH_MAX);
			mnt = node->mnt = (struct mntent*)malloc(sizeof(struct mntent));
		}
		free(node->mnt);
		free(node->buf);
		free(node);
		// iterate, unmounting and cleaning up
		while(node = head) {
			mnt = node->mnt;
			mount(NULL,mnt->mnt_dir,NULL,MS_SLAVE|MS_REC,NULL);
			if(
				(strncmp(mnt->mnt_fsname,"/dev/block/loop",sizeof("/dev/block/loop")-1) == 0)||
				(strncmp(mnt->mnt_fsname,"/dev/loop",sizeof("/dev/loop")-1) == 0)
			) loopdev_umount2(mnt->mnt_dir,MNT_DETACH);
			else umount2(mnt->mnt_dir,MNT_DETACH);
			head = node->next;
			free(node->mnt);
			free(node->buf);
			free(node);
		}
		free(target_slash);
		fclose(fp);
	} else {
		// fallback (deprecated)
		mount(NULL,target,NULL,MS_SLAVE|MS_REC,NULL);
		if(loopdev) loopdev_umount2(target,MNT_DETACH);
		else umount2(target,MNT_DETACH);
	}
}

static int copy(char *src, char *dst) {
	if((!src)||(!dst)) return -1;
	struct stat st;
	int src_fd, dst_fd;
	void *src_ptr, *dst_ptr;
	if((src_fd = open(src,O_RDONLY)) < 0) return src_fd;
	fstat(src_fd,&st);
	if((dst_fd = open(dst,O_RDWR|O_CREAT|O_TRUNC,st.st_mode)) < 0) {
		close(src_fd);
		return dst_fd;
	}
	if((int)(src_ptr = mmap(0,st.st_size,PROT_READ,MAP_SHARED,src_fd,0)) < 0) {
		close(dst_fd);
		close(src_fd);
		return (int)src_ptr;
	}
	if((int)(dst_ptr = mmap(0,st.st_size,PROT_WRITE,MAP_SHARED,dst_fd,0)) < 0) {
		munmap(src_ptr,st.st_size);
		close(dst_fd);
		close(src_fd);
		return (int)dst_ptr;
	}
	lseek(dst_fd,st.st_size-1,SEEK_SET);
	write(dst_fd,"",1);
	memcpy(dst_ptr,src_ptr,st.st_size);
	munmap(dst_ptr,st.st_size);
	munmap(src_ptr,st.st_size);
	close(dst_fd);
	close(src_fd);
	return 0;
}

int main(int argc, char *argv[]) {
	struct stat st;
	char apath[PATH_MAX];
	int remount = 0;
	int unmount = 0;
	char *loopmount = NULL;
	char *self = argv[0];
	uid_t uid = getuid();
	// get absolute path
	char *child_root = realpath(dirname(self),apath);
	int c;
	while(1) {
		static struct option long_options[] = {
			{"dir",required_argument,0,'d'},
			{"target",required_argument,0,'t'},
			{"remount",no_argument,0,'r'},
			{"unmount",no_argument,0,'u'},
			{0,0,0,0}
		};
		int option_index = 0;
		c = getopt_long(argc,argv,"d:t:ru",long_options,&option_index);
		if(c == -1) break;
		switch(c) {
			case 'd':
			case 't':
				// prevent privilege escalation: only superuser can chroot to arbitrary directories
				if(uid) {
					fprintf(stderr,"whoops: --dir is only available for uid=0\n");
					return EXIT_FAILURE;
				}
				child_root = realpath(optarg,apath);
				if(!child_root) {
					fprintf(stderr,"whoops: `%s' does not exist\n",optarg);
					return EXIT_FAILURE;
				}
				break;
			case 'r':
				remount = 1;
			case 'u':
				unmount = 1;
				break;
			default:
				usage(self);
		}
	}
	char *const *child_argv = (optind==argc)?NULL:(argv+optind);
	// prevent privilege escalation: fail if link/symlink is not owned by superuser
	if(uid) {
		if(lstat(self,&st)) {
			fprintf(stderr,"whoops: cannot stat `%s'\n",self);
			return EXIT_FAILURE;
		}
		if(st.st_uid) {
			fprintf(stderr,"whoops: `%s' is not owned by uid=0\n",self);
			return EXIT_FAILURE;
		}
	}
	// check if target exists
	if(stat(child_root,&st)) {
		fprintf(stderr,"whoops: `%s' does not exist\n",child_root);
		return EXIT_FAILURE;
	}
	if(S_ISREG(st.st_mode)) {
		loopmount = child_root;
		child_root = (char*)malloc(PATH_MAX);
		child_root = realpath(dirname(loopmount),child_root);
		child_root = (char*)realloc(child_root,strlen(child_root)+1);
	} else if(!S_ISDIR(st.st_mode)) {
		fprintf(stderr,"whoops: `%s' is not a directory\n",child_root);
		return EXIT_FAILURE;
	} else {
		if((st.st_uid)||(st.st_gid)) chown(child_root,0,0);
		if((st.st_mode&S_IWGRP)||(st.st_mode&S_IWOTH)) chmod(child_root,0755);
	}
	self = (char*)malloc(snprintf(NULL,0,"%s/init",child_root)+1);
	sprintf(self,"%s/init",child_root);
	// check if directory mounted
	int mounted = 0;
	int loopmounted = 0;
	FILE *fp1;
	if(fp1 = fopen("/proc/self/mounts","r")) {
		char *mntpt = child_root;
		struct mntent *mnt;
		while(mnt = getmntent(fp1)) {
			if(strcmp(mnt->mnt_dir,mntpt) != 0) continue;
			if(
				(strncmp(mnt->mnt_fsname,"/dev/block/loop",sizeof("/dev/block/loop")-1) == 0)||
				(strncmp(mnt->mnt_fsname,"/dev/loop",sizeof("/dev/loop")-1) == 0)
			) {
				loopmounted = 1;
				FILE *fp2;
				if(fp2 = fopen("/proc/self/mounts","r")) {
					char *mntpt_run = (char*)malloc(snprintf(NULL,0,"%s/run",mntpt)+1);
					sprintf(mntpt_run,"%s/run",mntpt);
					while(mnt = getmntent(fp2)) if(strcmp(mnt->mnt_dir,mntpt_run) == 0) {
						mounted = 1;
						break;
					}
					fclose(fp2);
					free(mntpt_run);
				}
			} else mounted = 1;
			break;
		}
		fclose(fp1);
	}
	// check if directory needs to be unmounted
	if(unmount) {
		if(geteuid()) {
			fprintf(stderr,"whoops: superuser privileges required to unmount\n");
			return EXIT_FAILURE;
		}
		mount_teardown(child_root,loopmounted);
		if(remount) mounted = 0;
		else return EXIT_SUCCESS;
	}
	if(!mounted) {
		// require superuser
		if(geteuid()) {
			fprintf(stderr,"whoops: superuser privileges required for first invocation of `%s'\n",self);
			return EXIT_FAILURE;
		}
		// prepare dynamic mounts
		dynamic_remount("/mnt","/data/.botbrew");
		if(loopmount) {
			// perform loopback mount
			if(loopdev_mount(loopmount,child_root,"ext4",0,NULL)) {
				fprintf(stderr,"whoops: cannot mount `%s'\n",loopmount);
			//	umount2("/data/.botbrew",MNT_DETACH);
			//	rmdir("/data/.botbrew");
				return EXIT_FAILURE;
			}
			loopmounted = 1;
		}
		// set up directory mappings
		char *child_mnt = strconcat(child_root,"/mnt");
		unlink(child_mnt);
		free(child_mnt);
		mount_setup(child_root,loopmounted);
		// fix symlinks
		fix_mnt_symlink("/mnt",child_root,"/emmc","/sdcard","/sdcard2","/usbdisk",NULL);
		// copy self
		if(strcmp(argv[0],self) != 0) {
			time_t mtime = stat(argv[0],&st)?0:st.st_mtime;
			if(!stat(self,&st)) {
				if((!S_ISDIR(st.st_mode))&&(st.st_mtime < mtime)) if(copy(argv[0],self) != 0) unlink(self);
			} else if(copy(argv[0],self)) unlink(self);
		}
		// chmod copy
		if(!stat(self,&st)) {
			// setuid
			if((st.st_uid)||(st.st_gid)) chown(self,0,0);
			if((st.st_mode&S_IWGRP)||(st.st_mode&S_IWOTH)||!(st.st_mode&S_ISUID)) chmod(self,04755);
		}
	}
	// do the chroot and chdir dance
	char cwd[PATH_MAX];
	if(getcwd(cwd,sizeof(cwd)) == NULL) {
		fprintf(stderr,"whoops: cannot get working directory\n");
		return EXIT_FAILURE;
	}
	if(chdir(child_root)) {
		fprintf(stderr,"whoops: cannot chdir to namespace\n");
		return EXIT_FAILURE;
	}
	if(chroot(".")) {
		fprintf(stderr,"whoops: cannot chroot\n");
		return EXIT_FAILURE;
	}
	if((chdir(cwd))&&(chdir("/"))) {
		fprintf(stderr,"whoops: cannot chdir to chroot\n");
		return EXIT_FAILURE;
	}
	// drop privileges
	privdrop();
	// configure environment
	char *env_path = getenv("PATH");
	if((env_path)&&(env_path[0])) {
		size_t env_path_len = strlen(env_path);
		char *newpath = (char*)malloc(sizeof(ENV_PATH)+env_path_len);
		char *append = strstr(env_path,"::");
		if(append) {	// break string at :: and insert
			append++;
			size_t prepend_len = append-env_path;
			memcpy(newpath,env_path,prepend_len);
			memcpy(newpath+prepend_len,ENV_PATH,sizeof(ENV_PATH)-1);
			memcpy(newpath+prepend_len+sizeof(ENV_PATH)-1,append,env_path_len-prepend_len+1);
		} else {
			memcpy(newpath,ENV_PATH":",sizeof(ENV_PATH":")-1);
			memcpy(newpath+sizeof(ENV_PATH":")-1,env_path,env_path_len+1);	// includes null terminator
		}
		setenv("PATH",newpath,1);
		free(newpath);
	} else setenv("PATH",ENV_PATH":/android/sbin:/system/sbin:/system/bin:/system/xbin",1);
	unsetenv("LD_LIBRARY_PATH");
	setenv("BOTBREW_PREFIX",child_root,1);
	if(loopmount) setenv("BOTBREW_IMAGE",loopmount,1);
	// run specified command or /init.sh
	if(child_argv == NULL) {
		const char *argv0[2];
		argv0[0] = "/init.sh";
		argv0[1] = 0;
		child_argv = (char**)&argv0;
	}
	if(execvp(child_argv[0],child_argv)) {
		int i = 1;
		fprintf(stderr,"whoops: cannot run `%s",child_argv[0]);
		while(child_argv[i]) fprintf(stderr," %s",child_argv[i++]);
		fprintf(stderr,"'\n");
		return errno;
	}
	// wtf
	return EXIT_FAILURE;
}
