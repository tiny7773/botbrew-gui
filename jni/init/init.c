#include <libgen.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <alloca.h>
#include <malloc.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "strnstr.h"

#define ENV_PATH	"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/games:/usr/games:/botbrew/bin:/usr/lib/busybox"

struct config {
	char *target;
	char *cwd;
	char *const *argv;
};

struct mountspec {
	char *src;
	char *dst;
	char *type;
	unsigned long flags;
	void *data;
	unsigned long remount_flags;
};

static struct mountspec foreign_mounts[] = {
	{NULL,"/proc","proc",0,NULL,0},
	{"/dev","/dev",NULL,MS_BIND,NULL,0},
	{"/dev/pts","/dev/pts",NULL,MS_BIND,NULL,0},
	{NULL,"/sys","sysfs",0,NULL,0},
	{NULL,"/android","tmpfs",MS_NODEV|MS_NOEXEC|MS_NOATIME,"size=1M,mode=0755",0},
	{"/cache","/android/cache",NULL,MS_BIND,NULL,0},
	{"/data","/android/data",NULL,MS_BIND,NULL,0},
	{"/datadata","/android/datadata",NULL,MS_BIND,NULL,0},
	{"/emmc","/android/emmc",NULL,MS_BIND,NULL,0},
	{"/sd-ext","/android/sd-ext",NULL,MS_BIND,NULL,0},
	{"/sdcard","/android/sdcard",NULL,MS_BIND,NULL,0},
	{"/system","/android/system",NULL,MS_BIND,NULL,MS_REMOUNT|MS_NODEV|MS_NOATIME},
	{"/usbdisk","/android/usbdisk",NULL,MS_BIND,NULL,0},
	{"/system/xbin","/android/system/xbin",NULL,MS_BIND,NULL,MS_REMOUNT|MS_NODEV|MS_NOATIME}
};
static int n_foreign_mounts = sizeof(foreign_mounts)/sizeof(foreign_mounts[0]);

static void usage(char *progname) {
	fprintf(stderr,
		"Usage: %s [options] [--] [<command>...]\n"
		"\n"
		"Available options:\n"
		"\t-d <directory>\t| --dir=<directory>\tSpecify chroot directory\n",
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

static int main_clone(struct config *config) {
	char *initsh = alloca(snprintf(NULL,0,"%s/init.sh",config->target)+1);
	sprintf(initsh,"%s/init.sh",config->target);
	char cwd[PATH_MAX];
	if(getcwd(cwd,sizeof(cwd)) == NULL) {
		fprintf(stderr,"whoops: cannot get working directory\n");
		return EXIT_FAILURE;
	}
	if(chdir(config->target)) {
		fprintf(stderr,"whoops: cannot chdir to namespace\n");
		return EXIT_FAILURE;
	}
	size_t target_len = strlen(config->target);
	int i;
	struct stat st;
	for(i = 0; i < n_foreign_mounts; i++) {
		struct mountspec m = foreign_mounts[i];
		if((m.src)&&(stat(m.src,&st) != 0)) continue;
		size_t dst_len = strlen(m.dst);
		char *dst = malloc(target_len+dst_len+1);
		memcpy(dst,config->target,target_len);
		memcpy(dst+target_len,m.dst,dst_len+1);	// includes null terminator
		mkdir(dst,0755);
		if(mount(m.src,dst,m.type,m.flags,m.data)) rmdir(dst);
		else if(m.remount_flags) mount(dst,dst,NULL,m.remount_flags|MS_REMOUNT,NULL);
		free(dst);
	}
	if(chroot(".")) {
		fprintf(stderr,"whoops: cannot chroot\n");
		return EXIT_FAILURE;
	}
	if((chdir(cwd))&&(chdir("/"))) {
		fprintf(stderr,"whoops: cannot chdir to chroot\n");
		return EXIT_FAILURE;
	}
	privdrop();
	char *env_path = getenv("PATH");
	if((env_path)&&(env_path[0])) {
		size_t env_path_len = strlen(env_path);
		char *newpath = malloc(sizeof(ENV_PATH)+env_path_len);
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
	setenv("BOTBREW_PREFIX",config->target,1);
	if(config->argv == NULL) {
		char *argv0[2];
		argv0[0] = "/init.sh";
		argv0[1] = 0;
		config->argv = (char**)&argv0;
	}
	if(execvp(config->argv[0],config->argv)) {
		int i = 1;
		fprintf(stderr,"whoops: cannot run `%s",config->argv[0]);
		while(config->argv[i]) fprintf(stderr," %s",config->argv[i++]);
		fprintf(stderr,"'\n");
		return errno;
	}
	return 0;	// wtf
}

int main(int argc, char *argv[]) {
	struct config config;
	struct stat st;
	char apath[PATH_MAX];
	// get absolute path
	config.target = realpath(dirname(argv[0]),apath);
	uid_t uid = getuid();
	int c;
	while(1) {
		static struct option long_options[] = {
			{"dir",required_argument,0,'d'},
			{0,0,0,0}
		};
		int option_index = 0;
		c = getopt_long(argc,argv,"d:",long_options,&option_index);
		if(c == -1) break;
		switch(c) {
			case 'd':
				// prevent privilege escalation: only superuser can chroot to arbitrary directories
				if(uid) {
					fprintf(stderr,"whoops: --dir is only available for uid=0\n");
					return EXIT_FAILURE;
				}
				config.target = realpath(optarg,apath);
				break;
			default:
				usage(argv[0]);
		}
	}
	config.argv = (optind==argc)?NULL:(argv+optind);
	// prevent privilege escalation: fail if link/symlink is not owned by superuser
	if(uid) {
		if(lstat(argv[0],&st)) {
			fprintf(stderr,"whoops: cannot stat `%s'\n",argv[0]);
			return EXIT_FAILURE;
		}
		if(st.st_uid) {
			fprintf(stderr,"whoops: `%s' is not owned by uid=0\n",argv[0]);
			return EXIT_FAILURE;
		}
	}
	// check if directory exists
	if((stat(config.target,&st))||(!S_ISDIR(st.st_mode))) {
		fprintf(stderr,"whoops: `%s' is not a directory\n",config.target);
		return EXIT_FAILURE;
	}
	if((st.st_uid)||(st.st_gid)) chown(config.target,0,0);
	if((st.st_mode&S_IWGRP)||(st.st_mode&S_IWOTH)) chmod(config.target,0755);
	// check if directory mounted
	FILE *fp;
	char *haystack;
	size_t len;
	size_t target_len = strlen(config.target);
	char *needle = malloc(target_len+3);
	needle[0] = needle[target_len+1] = ' ';
	memcpy(needle+1,config.target,target_len+1);	// includes null terminator
	int mounted = 0;
	if(fp = fopen("/proc/self/mounts","r")) while(haystack = fgetln(fp,&len)) if(strnstr(haystack,needle,len)) {
		mounted = 1;
		break;
	}
	free(needle);
	if(!mounted) {
		if(geteuid()) {
			fprintf(stderr,"whoops: superuser privileges required for first invocation of `%s'\n",argv[0]);
			return EXIT_FAILURE;
		}
		mount(config.target,config.target,NULL,MS_BIND,NULL);
		mount(config.target,config.target,NULL,MS_REMOUNT|MS_NODEV|MS_NOATIME,NULL);
		if(!stat(argv[0],&st)) {
			// setuid
			if((st.st_uid)||(st.st_gid)) chown(argv[0],0,0);
			if((st.st_mode&S_IWGRP)||(st.st_mode&S_IWOTH)||!(st.st_mode&S_ISUID)) chmod(argv[0],04755);
		}
	}
	// clone with new namespace
	long stacksz = sysconf(_SC_PAGESIZE);
	void *stack = alloca(stacksz)+stacksz;
#ifdef __i386__
	pid_t pid = __sys_clone(main_clone,stack,SIGCHLD|CLONE_NEWNS|CLONE_FILES,(void*)&config);
#else
	pid_t pid = clone(main_clone,stack,SIGCHLD|CLONE_NEWNS|CLONE_FILES,(void*)&config);
#endif
	if(pid < 0) {
		fprintf(stderr,"whoops: cannot clone\n");
		return EXIT_FAILURE;
	} else {
		struct sigaction act;
		int ret;
		privdrop();
		memset(&act,0,sizeof(act));
		act.sa_handler = sighandler;
		child_pid = pid;
		sigaction(SIGABRT,&act,0);
		sigaction(SIGALRM,&act,0);
		sigaction(SIGHUP,&act,0);
		sigaction(SIGINT,&act,0);
		sigaction(SIGQUIT,&act,0);
		sigaction(SIGTERM,&act,0);
		sigaction(SIGUSR1,&act,0);
		sigaction(SIGUSR2,&act,0);
		sigaction(SIGCONT,&act,0);
		sigaction(SIGSTOP,&act,0);
		sigaction(SIGTSTP,&act,0);
		sigaction(SIGPOLL,&act,0);
		sigaction(SIGPROF,&act,0);
		sigaction(SIGURG,&act,0);
		sigaction(SIGVTALRM,&act,0);
		sigaction(SIGXCPU,&act,0);
		while((waitpid(pid,&ret,0)<0)&&(errno == EINTR));
		child_pid = 0;
		return WIFEXITED(ret)?WEXITSTATUS(ret):EXIT_FAILURE;
	}
}
