#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../bar-icons.c"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool release_writer;
static char fifo_path[512], png_path[512];
static void *writer(void *unused) {
    pthread_mutex_lock(&gate);
    while (!release_writer) pthread_cond_wait(&ready,&gate);
    pthread_mutex_unlock(&gate);
    int input=open(png_path,O_RDONLY), output=open(fifo_path,O_WRONLY);
    assert(input>=0 && output>=0);
    char data[4096]; ssize_t n;
    while ((n=read(input,data,sizeof(data)))>0) assert(write(output,data,n)==n);
    close(input); close(output);
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);
    snprintf(png_path,sizeof(png_path),"%s/icon.png",argv[1]);
    snprintf(fifo_path,sizeof(fifo_path),"%s/slow.png",argv[1]);
    cairo_surface_t *image=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,18,18);
    cairo_t *cr=cairo_create(image); cairo_set_source_rgb(cr,.2,.8,.4); cairo_paint(cr); cairo_destroy(cr);
    assert(cairo_surface_write_to_png(image,png_path)==CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(image);
    assert(mkfifo(fifo_path,0600)==0);
    pthread_t thread; assert(pthread_create(&thread,NULL,writer,NULL)==0);
    struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
    assert(bar_icon_get(fifo_path,18)==NULL);
    for(int i=0;i<1000;i++) {
        assert(bar_icon_get(fifo_path,18)==NULL);
        bar_icon_get(png_path,18);
    }
    assert(g_hash_count==2); // queued loads are deduplicated
    clock_gettime(CLOCK_MONOTONIC,&b);
    double ms=(b.tv_sec-a.tv_sec)*1000.0+(b.tv_nsec-a.tv_nsec)/1e6;
    // The worker cannot complete the FIFO until this UI thread opens the gate.
    pthread_mutex_lock(&gate); release_writer=true; pthread_cond_signal(&ready); pthread_mutex_unlock(&gate);
    for(int i=0;i<20 && !bar_icon_get(png_path,18);i++) {
        struct pollfd fd={.fd=bar_icons_get_notify_fd(),.events=POLLIN};
        assert(poll(&fd,1,1000)>0); bar_icons_notify_done();
    }
    assert(bar_icon_get(fifo_path,18)); assert(bar_icon_get(png_path,18));
    assert(cairo_image_surface_get_width(bar_icon_get(png_path,18))==18);
    pthread_join(thread,NULL);
    // Negative results are cached, not resubmitted on every frame.
    bar_icon_get("maindeck-nonexistent-icon-regression",18);
    struct pollfd fd={.fd=bar_icons_get_notify_fd(),.events=POLLIN};
    assert(poll(&fd,1,5000)>0); bar_icons_notify_done();
    int cached=g_hash_count;
    for(int i=0;i<1000;i++) assert(!bar_icon_get("maindeck-nonexistent-icon-regression",18));
    assert(g_hash_count==cached);
    for (int size=19;size<39;size++) bar_icon_get(png_path,size);
    bar_icons_cleanup(); // cancel queued work and join any active decode safely
    assert(g_hash_count==0 && bar_icons_get_notify_fd()==-1);
    printf("PASS: UI queues 2000 requests in %.3f ms while icon IO is blocked; publication, deduplication, negative cache and shutdown OK\n",ms);
}
