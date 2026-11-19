// SPDX-License-Identifier: GPL-2.0-only
// Diagnostic transport: preserve logs in the next initramfs, without a reset.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static void die(const char *what) { perror(what); exit(1); }
static void put(int fd,const void *p,size_t n) {
 const char *s=p;while(n) {ssize_t w=write(fd,s,n);if(w<0&&errno==EINTR)continue;if(w<=0)die("write");s+=w;n-=w;}
}
static void pad(int fd) {static const char zero[4]={0};off_t n=lseek(fd,0,SEEK_CUR);if(n<0)die("seek");put(fd,zero,(-n)&3);}
static void entry(int fd,const char *name,const void *data,size_t n) {
 char h[111];int len=snprintf(h,sizeof(h),"070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",1,0100644,0,0,1,0,(unsigned)n,0,0,0,0,(unsigned)strlen(name)+1,0);
 if(len!=110||n>16*1024*1024)die("cpio header");put(fd,h,110);put(fd,name,strlen(name)+1);pad(fd);put(fd,data,n);pad(fd);
}
static void file_entry(int fd,const char *path,const char *name) {
 char *data=malloc(1024*1024);size_t used=0;if(!data)die("malloc");int in=open(path,O_RDONLY|O_CLOEXEC);
 if(in>=0) {while(used<1024*1024) {ssize_t n=read(in,data+used,1024*1024-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=n;}close(in);}
 else used=snprintf(data,1024*1024,"unavailable: %s: %s\n",path,strerror(errno));
 entry(fd,name,data,used);free(data);
}
int main(int argc,char **argv) {
 const char *stage=argc>1?argv[1]:"unspecified";
 const char *cmd=getenv("POLARIS_LOG_RESCUE_CMDLINE");
 if(!cmd)cmd="rdinit=/init console=tty0 acpi=off efi=noruntime,novamap clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=15";
 int k=open("/rescue/Image",O_RDONLY|O_CLOEXEC);if(k<0)die("rescue Image");
 int init=memfd_create("polaris-log-rescue",MFD_CLOEXEC);if(init<0)die("memfd_create");
 int src=open("/rescue/initrd-prefix.cpio",O_RDONLY|O_CLOEXEC);if(src<0)die("rescue initramfs");
 char buf[65536];ssize_t n;while((n=read(src,buf,sizeof(buf)))>0)put(init,buf,n);if(n<0)die("read initramfs");close(src);
 entry(init,"previous-boot-stage.txt",stage,strlen(stage));
 char *log=malloc(4*1024*1024);if(!log)die("malloc dmesg");
 int bytes=klogctl(3,log,4*1024*1024);if(bytes<0)die("klogctl");
 entry(init,"previous-boot-dmesg.txt",log,bytes);free(log);
 file_entry(init,"/proc/cmdline","previous-boot-cmdline.txt");
 file_entry(init,"/sys/kernel/debug/devices_deferred","previous-boot-deferred.txt");
 file_entry(init,"/run/reimuks/usb.log","previous-boot-usb.txt");
 file_entry(init,"/proc/interrupts","previous-boot-interrupts.txt");
 entry(init,"TRAILER!!!","",0);if(lseek(init,0,SEEK_SET)<0)die("rewind");
 fprintf(stderr,"POLARIS_LOG_RESCUE: stage=%s log=%d bytes; loading rescue kernel\n",stage,bytes);
 if(syscall(SYS_kexec_file_load,k,init,strlen(cmd)+1,cmd,0UL)<0)die("kexec_file_load");
 fprintf(stderr,"POLARIS_LOG_RESCUE: loaded; executing\n");
 sync();reboot(RB_KEXEC);die("reboot kexec");
}
