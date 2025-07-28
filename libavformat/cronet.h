#ifndef SOFAPLAYER_SOFA_CRONET_H
#define SOFAPLAYER_SOFA_CRONET_H

#include "libavformat/url.h"
#include <pthread.h>
#include <cronet_c.h>
#include "libavutil/fifo.h"
#if APPLICATION
#include "libavutil/application.h"
#endif

struct CronetTask;
typedef struct CronetRuntimeContext CronetRuntimeContext;

typedef struct CronetContext {
    const AVClass *class;
    int timeout;
    int seekable;
    int fifo_size;
    int max_fifo_size;
    AVFifoBuffer *fifo;
    pthread_mutex_t fifo_mutex;
    pthread_cond_t fifo_cond;
    char *location;     // Request uri.
    uint64_t file_size;  // File total size.
    int64_t offset;     // Start offset in file of current request.
    int64_t read_pos;   // Current read offset in file of current request.
    int64_t write_pos;  // Current write offset in file of current request.
    Cronet_UrlRequestPtr request;
    Cronet_UrlRequestCallbackPtr callback;
    Cronet_BufferPtr    buffer;
    struct CronetTask *closing_task;
    struct CronetTask *current_task;
    int is_open;
    int buffer_overflow;
    int error_state;
    int header_read_finished;
    int64_t app_ctx_intptr;
#if APPLICATION    
    AVApplicationContext *app_ctx;
#endif    
    int max_retry;
    CronetRuntimeContext *cronet_runtime_ctx;
} CronetContext;

typedef enum CronetTaskType {
    kCronetTaskType_Runnable = 0,
    kCronetTaskType_FF_Open,
    kCronetTaskType_FF_Close,
    kCronetTaskType_FF_Reset,
    kCronetTaskType_FF_Range
} CronetTaskType;

typedef struct CronetTask {
    CronetTaskType      type;
    Cronet_RunnablePtr  runnable;
    URLContext          *url_context;
    char                *url;
    int64_t             start;
    int64_t             end;
    pthread_mutex_t     invoke_mutex;
    pthread_cond_t      invoke_cond;
    int                 invoke_returned;
    int                 invoke_return_value;
    struct CronetTask   *next;
} CronetTask;

typedef struct CronetTaskQueue {
    CronetTask          *head;
    CronetTask          *tail;
    int                 size;
} CronetTaskQueue;

typedef struct CronetRuntimeContext {
    Cronet_ExecutorPtr  executor;
    pthread_t           executor_thread;
    pthread_mutex_t     executor_task_mutex;
    pthread_cond_t      executor_task_cond;
    CronetTaskQueue     executor_task_queue;
    int                 executor_stop_thread_loop;
} CronetRuntimeContext;

typedef struct AVAppCronetEvent {
    void *obj;
    char metrics[8192];
} AVAppCronetEvent;

#define OFFSET(x) offsetof(CronetContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
#define CRONET_DEFAULT_BUFFER_SIZE 524288
#define CRONET_MAX_BUFFER_SIZE  33554432
#define CRONET_HTTP_HEADER_SIZE 128

#define AVAPP_EVENT_DID_CRONET_METRICS   8


int64_t cronet_seek(URLContext *h, int64_t off, int whence);

int cronet_write(URLContext *h, const uint8_t *buf, int size);

int cronet_read(URLContext *h, uint8_t *buf, int size);

int cronet_close(URLContext *h);

int cronet_open(URLContext *h, const char *uri, int flags, AVDictionary ** option);

int av_format_cronet_init(const char *hosts[], int len);
void av_format_cronet_uninit(void);
CronetRuntimeContext* create_cronet_runtime_context(void);
void destroy_cronet_runtime_context(CronetRuntimeContext* cronet_runtime_context);
char* av_format_cronet_get_current_protocol(void);
#endif //SOFAPLAYER_SOFA_CRONET_H
