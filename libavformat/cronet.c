#include "libavformat/avformat.h"
#include "libavformat/url.h"
#include "libavutil/avstring.h"
#include "libavutil/fifo.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "cronet.h"
//#include "../../utils/sofa_log.h"

#define TAG "sofa_iocronet"
#define LOGI(tag, ...)  av_log(NULL, AV_LOG_INFO, __VA_ARGS__)
#define LOGE(tag, ...)  av_log(NULL, AV_LOG_ERROR, __VA_ARGS__)

static const AVOption options[] = {
        { "cronet_timeout", "Timeout in us.",              OFFSET(timeout),        AV_OPT_TYPE_INT,     { .i64 = -1 },                           -1,  INT_MAX,      .flags = D|E },
        { "seekable",       "Whether seek is available.",  OFFSET(seekable),       AV_OPT_TYPE_BOOL,    { .i64 = -1 },                           -1,  1,            .flags = D|E },
        { "fifo_size",      "FIFO size.",                  OFFSET(fifo_size),      AV_OPT_TYPE_INT,     {.i64 = CRONET_DEFAULT_BUFFER_SIZE},      0,  INT_MAX,      .flags = D|E },
        { "max_fifo_size",  "Max FIFO size.",              OFFSET(max_fifo_size),  AV_OPT_TYPE_INT,     {.i64 = CRONET_MAX_BUFFER_SIZE},          0,  INT_MAX,      .flags = D|E },
        { NULL }
};

#define CRONET_CLASS(flavor)                        \
static const AVClass flavor ## _context_class = {   \
    .class_name = # flavor,                         \
    .item_name  = av_default_item_name,             \
    .option     = options,                          \
    .version    = LIBAVUTIL_VERSION_INT,            \
}

CRONET_CLASS(cronet);
CRONET_CLASS(cronets);


static void stop_read(CronetContext *s, int status) {
    if (s != NULL) {
        pthread_mutex_lock(&s->fifo_mutex);
        s->is_open = status;
        pthread_cond_signal(&s->fifo_cond);
        pthread_mutex_unlock(&s->fifo_mutex);
    }
}
#if APPLICATION
void av_application_on_cronet_event(AVApplicationContext *h, int event_type, AVAppCronetEvent *event)
{
    if (h && h->func_on_app_event)
        h->func_on_app_event(h, event_type, (void *)event, sizeof(AVAppCronetEvent));
}

void av_application_did_cronet_metrics(AVApplicationContext *h, void *obj, char *metrics) {
    AVAppCronetEvent event = {0};

    if (!h || !obj) {
        return;
    }

    event.obj = obj;
    av_strlcpy(event.metrics, metrics, sizeof(event.metrics));
    av_application_on_cronet_event(h, AVAPP_EVENT_DID_CRONET_METRICS, &event);
}
#endif
// Constants.
const char *CRONET_FFMPEG_USER_AGENT = "QFCronetFFmpeg/1.0.20";

// Global cronet engine, not thread safe.
static Cronet_EnginePtr cronet_engine = NULL;
static CronetRuntimeContext *cronetRuntimeCtx = NULL;
char *cronet_protocol = "unknown";

// Pre-defination.
static void cronet_execute(Cronet_ExecutorPtr self, Cronet_RunnablePtr runnable);

static void cronet_uninit_task_queue(CronetTaskQueue *queue);

static void on_redirect_received(Cronet_UrlRequestCallbackPtr self,
                                 Cronet_UrlRequestPtr request,
                                 Cronet_UrlResponseInfoPtr info,
                                 Cronet_String newLocationUrl);

static void on_response_started(Cronet_UrlRequestCallbackPtr self,
                                Cronet_UrlRequestPtr request,
                                Cronet_UrlResponseInfoPtr info);

static void on_read_completed(Cronet_UrlRequestCallbackPtr self,
                              Cronet_UrlRequestPtr request,
                              Cronet_UrlResponseInfoPtr info,
                              Cronet_BufferPtr buffer,
                              uint64_t bytes_read);

static void on_succeeded(Cronet_UrlRequestCallbackPtr self,
                         Cronet_UrlRequestPtr request,
                         Cronet_UrlResponseInfoPtr info);

static void on_failed(Cronet_UrlRequestCallbackPtr self,
                      Cronet_UrlRequestPtr request,
                      Cronet_UrlResponseInfoPtr info,
                      Cronet_ErrorPtr error);

static void on_canceled(Cronet_UrlRequestCallbackPtr self,
                        Cronet_UrlRequestPtr request,
                        Cronet_UrlResponseInfoPtr info);

static CronetTask* create_task(CronetTaskType type) {
    CronetTask *task    = av_mallocz(sizeof(CronetTask));
    task->type          = type;

    pthread_mutex_init(&task->invoke_mutex, NULL);
    pthread_cond_init(&task->invoke_cond, NULL);
    return task;
}

static CronetTask* create_context_task(CronetTaskType type,
                                       URLContext *url_context) {
    CronetTask *task    = create_task(type);
    task->url_context   = url_context;
    return task;
}

static CronetTask* create_runnable_task(Cronet_RunnablePtr runnable) {
    CronetTask *task    = create_task(kCronetTaskType_Runnable);
    task->runnable      = runnable;
    return task;
}

static CronetTask* create_open_task(URLContext *url_context, const char *url, uint64_t start, uint64_t end) {
    CronetTask *task    = create_context_task(kCronetTaskType_FF_Open, url_context);
    task->url           = av_strdup(url);
    task->start         = start;
    task->end           = end;
    return task;
}

static CronetTask* create_close_task(URLContext *url_context) {
    return create_context_task(kCronetTaskType_FF_Close, url_context);
}

static CronetTask* create_reset_task(URLContext *url_context) {
    return create_context_task(kCronetTaskType_FF_Reset, url_context);
}

static CronetTask* create_range_task(URLContext *url_context,
                                     int64_t start,
                                     int64_t end) {
    CronetTask *task    = create_context_task(kCronetTaskType_FF_Range, url_context);
    task->start         = start;
    task->end           = end;
    return task;
}

static void destroy_task(CronetTask *task) {
    if (task == NULL) {
        return;
    }

    if (task->runnable != NULL) {
        Cronet_Runnable_Destroy(task->runnable);
    }

    if (task->url != NULL) {
        av_freep(&task->url);
    }

    pthread_mutex_destroy(&task->invoke_mutex);
    pthread_cond_destroy(&task->invoke_cond);

    av_freep(&task);
}

static void cronet_init_task_queue(CronetTaskQueue *queue) {
    if (queue == NULL) {
        return;
    }

    if (queue->head != NULL) {
        cronet_uninit_task_queue(queue);
    }

    queue->head = create_task(kCronetTaskType_Runnable);
    queue->tail = queue->head;
    queue->size = 0;
}

static void cronet_uninit_task_queue(CronetTaskQueue *queue) {
    CronetTask *task = NULL;
    if (queue == NULL) {
        return;
    }

    task = queue->head;
    while (task != NULL) {
        CronetTask *next = task->next;
        destroy_task(task);
        task = next;
    }

    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
}

static void cronet_enqueue_task(CronetTaskQueue *queue,
                                CronetTask *task) {
    LOGI(TAG, "cronet enqueue new task :%d, queue size :%d\n", task->type,  queue->size);
    if (queue == NULL || queue->tail == NULL || task == NULL) {
        return;
    }

    queue->tail->next   = task;
    queue->tail         = task;

    ++queue->size;
}

static CronetTask* cronet_dequeue_task(CronetTaskQueue *queue) {
    CronetTask *task = NULL;
    if (queue == NULL || queue->head == NULL || queue->size == 0) {
        return NULL;
    }

    task = queue->head->next;
    if (task == NULL) {
        return NULL;
    }

    queue->head->next   = task->next;

    if (queue->tail == task) {
        queue->tail = queue->head;
    }

    --queue->size;

    return task;
}

// Invoke a task on task thread, will automatically
// delete the task, DO NOT delete it manually.
static int invoke_task(CronetTask *task) {
    LOGI(TAG, "invoke task:%d\n", task->type);
    int posted = 0;
    int ret = AVERROR_UNKNOWN;
    URLContext *h = NULL;
    CronetContext *s = NULL;
    CronetRuntimeContext *cronet_runtime_context = NULL;

    if (task == NULL) {
        return AVERROR(EINVAL);
    }

    h = task->url_context;
    if (h == NULL || h->priv_data == NULL) {
        LOGI(TAG, "qh:h null\n");
        destroy_task(task);
        return AVERROR(EINVAL);
    }

    s = h->priv_data;
    cronet_runtime_context = s->cronet_runtime_ctx;
    if (cronet_runtime_context == NULL) {
        LOGI(TAG, "qh:cronet_runtime_context null\n");
        destroy_task(task);
        return AVERROR(EINVAL);
    }

    // Try to post to task queue.
    pthread_mutex_lock(&cronet_runtime_context->executor_task_mutex);
    if (!cronet_runtime_context->executor_stop_thread_loop) {
        cronet_enqueue_task(&cronet_runtime_context->executor_task_queue, task);
        pthread_cond_signal(&cronet_runtime_context->executor_task_cond);
        posted = 1;
    }
    LOGI(TAG, "pushed into queue, invoke task type:%d\n", task->type);
    pthread_mutex_unlock(&cronet_runtime_context->executor_task_mutex);

    // Wait for return value.
    pthread_mutex_lock(&task->invoke_mutex);
    while (posted && !task->invoke_returned) {
        pthread_cond_wait(&task->invoke_cond,
                          &task->invoke_mutex);
    }
    pthread_mutex_unlock(&task->invoke_mutex);

    if (posted) {
        ret = task->invoke_return_value;
    }
    LOGI(TAG, "invoke task got ret:%d, invoke task type:%d\n", ret, task->type);

    destroy_task(task);
    return ret;
}

static void post_return_value(CronetTask *task, int ret) {
    URLContext *h = NULL;
    CronetContext *s = NULL;
    CronetRuntimeContext *cronet_runtime_context = NULL;

    if (task == NULL) {
        return;
    }

    h = task->url_context;
    if (h == NULL || h->priv_data == NULL) {
        destroy_task(task);
        return;
    }

    s = h->priv_data;
    cronet_runtime_context = s->cronet_runtime_ctx;
    if (cronet_runtime_context == NULL) {
        destroy_task(task);
        return;
    }

    pthread_mutex_lock(&task->invoke_mutex);
    task->invoke_returned       = 1;
    task->invoke_return_value   = ret;
    pthread_cond_signal(&task->invoke_cond);
    pthread_mutex_unlock(&task->invoke_mutex);
}

static void process_runnable_task(CronetTask *task) {
    Cronet_RunnablePtr runnable = NULL;
    if (task == NULL) {
        return;
    }

    runnable = task->runnable;
    if (runnable != NULL) {
        LOGI(TAG, "process runable task type:%d\n",task->type);
        Cronet_Runnable_Run(runnable);
    }

    destroy_task(task);
}

static void process_open_task(CronetTask *task) {
//    task->url = "sofacronetio://testlive.hd.sohu.com/2.mp4";
    URLContext *h               = NULL;
    CronetContext *s            = NULL;
    const char *uri             = NULL;
    char *url                   = NULL;
    int url_len                 = 0;
    const char *http_scheme     = "http://";
    const char *https_scheme    = "https://";
    const char *scheme          = NULL;
    const char *deli            = "://";
    char *p                     = NULL;
    char hostname[1024]         = {0};
    char protocol[1024]         = {0};
    char path[1024]             = {0};
    int port                    = 0;
    Cronet_RESULT result        = Cronet_RESULT_SUCCESS;
    Cronet_UrlRequestParamsPtr request_params   = NULL;
    Cronet_HttpHeaderPtr header                 = NULL;
    char range_header[CRONET_HTTP_HEADER_SIZE]  = {0};
    CronetRuntimeContext *cronet_runtime_context= NULL;
    int ret                     = 0;
    uint64_t start              = 0;
    uint64_t end                = 0;

    do {
        if (cronet_engine == NULL) {
            ret = AVERROR_UNKNOWN;
            break;
        }

        if (task == NULL) {
            ret = AVERROR(EINVAL);
            break;
        }

        h   = task->url_context;
        uri = task->url;

        if (h == NULL || h->priv_data == NULL || uri == NULL) {
            ret = AVERROR(EINVAL);
            break;
        }

        s = h->priv_data;
        cronet_runtime_context = s->cronet_runtime_ctx;
        if (cronet_runtime_context == NULL || cronet_runtime_context->executor == NULL) {
            ret = AVERROR_UNKNOWN;
            break;
        }

        av_url_split(protocol, sizeof(protocol), NULL, 0, hostname, sizeof(hostname), &port, path, sizeof(path), uri);
        if (strlen(protocol) > 0 && strncmp(protocol, "sofacronetio", strlen(protocol)) == 0) {
            scheme = http_scheme;
        } else if (strlen(protocol) > 0 && strncmp(protocol, "cronet", strlen(protocol)) == 0) {
            scheme = http_scheme;
        } else if (strlen(protocol) > 0 && strncmp(protocol, "cronets", strlen(protocol)) == 0) {
            scheme = https_scheme;
        } else {
            ret = AVERROR_PROTOCOL_NOT_FOUND;
            break;
        }

        //Replace protocol with certain scheme.
        p       = strstr(uri, deli);
        p       += strlen(deli);
        url_len = strlen(scheme) + strlen(p) + 1;
        url     = (char*)av_mallocz(url_len);
        strcpy(url, scheme);
        strcat(url, p);

        if (s->seekable == 1) {
            h->is_streamed = 0;
        } else {
            h->is_streamed = 1;
        }

        s->file_size    = UINT64_MAX;
        s->offset       = 0;
        s->read_pos     = 0;
        s->write_pos    = 0;
        s->location     = av_strdup(url);
        s->is_open      = 0;
        s->header_read_finished = 0;
        s->max_retry    = 3;

        // Setup fifo buffer.
        s->fifo = av_fifo_alloc(s->fifo_size);
        pthread_mutex_init(&s->fifo_mutex, NULL);
        pthread_cond_init(&s->fifo_cond, NULL);

        // Setup callback.
        s->callback = Cronet_UrlRequestCallback_CreateWith(on_redirect_received,
                                                           on_response_started,
                                                           on_read_completed,
                                                           on_succeeded,
                                                           on_failed,
                                                           on_canceled);
        Cronet_UrlRequestCallback_SetClientContext(s->callback, h);

        // Setup request.
        s->request = Cronet_UrlRequest_Create();

        request_params = Cronet_UrlRequestParams_Create();
        // Setup request param.

        start = task->start;
        end = task->end;

        header = Cronet_HttpHeader_Create();
        Cronet_HttpHeader_name_set(header, "Range");
        if (start != -1 || end != -1) {
            LOGI(TAG, "process open task set range header\n");
            if (start < 0) start = 0;
            if (end != -1) {
                av_strlcatf(range_header, sizeof(range_header), "bytes=%"PRIu64"-%"PRIu64, start, end);
            } else {
                av_strlcatf(range_header, sizeof(range_header), "bytes=%"PRIu64"-", start);
            }
            Cronet_HttpHeader_value_set(header, range_header);
            Cronet_UrlRequestParams_request_headers_add(request_params, header);
        } else {
            // Note: we send this on purpose even when start is 0 when we're probing,
            // since it allows us to detect more reliably if a (non-conforming)
            // server supports seeking by analysing the reply headers.

            // from ffmpeg
            start = 0;
            av_strlcatf(range_header, sizeof(range_header), "bytes=%"PRIu64"-", start);
            Cronet_HttpHeader_value_set(header, range_header);
            Cronet_UrlRequestParams_request_headers_add(request_params, header);
        }
//        Cronet_HttpHeaderPtr header1 = Cronet_HttpHeader_Create();
//        Cronet_HttpHeader_name_set(header1, "alt-svc");
//        Cronet_HttpHeader_value_set(header1, "quic=\":443\"; ma=2592000; v=\"48,47,46,44,43,39\"");
//        Cronet_UrlRequestParams_request_headers_add(request_params, header1);

        Cronet_UrlRequestParams_http_method_set(request_params, "GET");
        result = Cronet_UrlRequest_InitWithParams(s->request,
                                                  cronet_engine,
                                                  s->location,
                                                  request_params,
                                                  s->callback,
                                                  cronet_runtime_context->executor);
        if (result != Cronet_RESULT_SUCCESS) {
            LOGE(TAG, "Cronet_UrlRequest_InitWithParams error %d\n", result);
            ret = AVERROR_UNKNOWN;
            break;
        }

        // Starting request.
        result = Cronet_UrlRequest_Start(s->request);
        if (result != Cronet_RESULT_SUCCESS) {
            LOGE(TAG, "Cronet_UrlRequest_Start error %d\n", result);
            ret = AVERROR_UNKNOWN;
            break;
        }

        s->current_task = task;

        s->is_open = 1;
    } while (0);

    if (ret != 0 && s != NULL) {
        if (s->request != NULL) {
            Cronet_UrlRequest_Destroy(s->request);
            s->request = NULL;
        }

        if (s->callback != NULL) {
            Cronet_UrlRequestCallback_Destroy(s->callback);
            s->callback = NULL;
        }

        if (s->fifo != NULL) {
            av_fifo_freep(&s->fifo);
            pthread_mutex_destroy(&s->fifo_mutex);
            pthread_cond_destroy(&s->fifo_cond);
        }
        post_return_value(task, ret);
    }

    if (request_params != NULL) {
        Cronet_UrlRequestParams_Destroy(request_params);
    }

    if (url != NULL) {
        av_free(url);
    }

    if (header != NULL) {
        Cronet_HttpHeader_Destroy(header);
    }

    //post_return_value(task, ret);
}

static void process_close_task(CronetTask *task) {
    URLContext *h       = NULL;
    CronetContext *s    = NULL;
    int ret             = 0;
    int do_cancel       = 0;
    do {
        if (cronet_engine == NULL) {
            ret = AVERROR_UNKNOWN;
            break;
        }

        if (task == NULL) {
            ret = AVERROR(EINVAL);
            break;
        }

        h = task->url_context;

        if (h == NULL || h->priv_data == NULL) {
            ret = AVERROR(EINVAL);
            break;
        }

        s = h->priv_data;
        if (!s->is_open) {
            break;
        }

        stop_read(s, 2);
        if (s->request != NULL) {
            if (s->closing_task == NULL) {
                Cronet_UrlRequest_Cancel(s->request);
                s->closing_task = task;
                do_cancel = 1;
            } else {
                // Already closing, just return.
                break;
            }
        }

        if (s->fifo != NULL) {
            av_fifo_freep(&s->fifo);
            pthread_mutex_destroy(&s->fifo_mutex);
            pthread_cond_destroy(&s->fifo_cond);
        }

        s->file_size    = UINT64_MAX;
        s->offset       = 0;
        s->read_pos     = 0;
        s->write_pos    = 0;
        s->is_open      = 0;

        if (s->location != NULL) {
            av_freep(&s->location);
        }
    } while (0);

    if (!do_cancel) {
        post_return_value(task, ret);
    }
}

static void process_reset_task(CronetTask *task) {
    URLContext *h       = NULL;
    CronetContext *s    = NULL;
    int ret             = 0;
    int do_cancel       = 0;
    do {
        if (cronet_engine == NULL) {
            ret = AVERROR_UNKNOWN;
            break;
        }

        if (task == NULL) {
            ret = AVERROR(EINVAL);
            break;
        }

        h = task->url_context;

        if (h == NULL || h->priv_data == NULL) {
            ret = AVERROR(EINVAL);
            break;
        }

        s = h->priv_data;

        if (!s->is_open) {
            break;
        }

        stop_read(s, 2);
        if (s->fifo == NULL) {
            ret = AVERROR(EINVAL);
            break;
        }

        if (s->request != NULL) {
            if (s->closing_task == NULL) {
                Cronet_UrlRequest_Cancel(s->request);
                s->closing_task = task;
                do_cancel = 1;
            } else {
                // Already closing, just return.
                break;
            }
        }

        s->read_pos     = 0;
        s->write_pos    = 0;
        s->is_open      = 0;
        s->current_task = task;


        av_fifo_reset(s->fifo);
        LOGI(TAG, "process reset task fifo size :%d\n", av_fifo_size(s->fifo));
    } while (0);

    if (!do_cancel) {
        LOGI(TAG, "process reset task reset_ret:%d\n", ret);
        post_return_value(task, ret);
    }
}

static void process_range_task(CronetTask *task) {
    LOGI(TAG, "begin process range task\n");
    URLContext *h           = NULL;
    CronetContext *s        = NULL;
    int64_t start           = 0;
    int64_t end             = 0;
    Cronet_RESULT result    = Cronet_RESULT_SUCCESS;
    Cronet_UrlRequestParamsPtr request_params = NULL;
    Cronet_HttpHeaderPtr header = NULL;
    CronetRuntimeContext *cronet_runtime_context = NULL;
    char range_header[CRONET_HTTP_HEADER_SIZE] = {0};
    int ret = 0;

    do {
        if (cronet_engine == NULL) {
            ret = AVERROR_UNKNOWN;
            break;
        }

        if (task == NULL) {
            ret = AVERROR(EINVAL);
            break;
        }

        h       = task->url_context;
        start   = task->start;
        end     = task->end;

        if (h == NULL || h->priv_data == NULL) {
            ret = AVERROR(EINVAL);
            break;
        }

        if (end > start) {
            ret = AVERROR(EINVAL);
            break;
        }

        s = h->priv_data;
        s->header_read_finished = 0;
        if (cronet_runtime_context == NULL || cronet_runtime_context->executor == NULL) {
            ret = AVERROR_UNKNOWN;
            break;
        }

        // Setup callback.
        s->callback = Cronet_UrlRequestCallback_CreateWith(on_redirect_received,
                                                           on_response_started,
                                                           on_read_completed,
                                                           on_succeeded,
                                                           on_failed,
                                                           on_canceled);
        Cronet_UrlRequestCallback_SetClientContext(s->callback, h);

        // Setup request.
        s->request = Cronet_UrlRequest_Create();

        s->current_task = task;

        // Setup request param.
        request_params = Cronet_UrlRequestParams_Create();

        // Set method.
        Cronet_UrlRequestParams_http_method_set(request_params, "GET");

        // Set range header.
        header = Cronet_HttpHeader_Create();
        Cronet_HttpHeader_name_set(header, "Range");
        if (end != -1) {
            av_strlcatf(range_header, sizeof(range_header), "bytes=%"PRIu64"-%"PRIu64, start, end);
        } else {
            av_strlcatf(range_header, sizeof(range_header), "bytes=%"PRIu64"-", start);
        }
        Cronet_HttpHeader_value_set(header, range_header);
        Cronet_UrlRequestParams_request_headers_add(request_params, header);

        result = Cronet_UrlRequest_InitWithParams(s->request,
                                                  cronet_engine,
                                                  s->location,
                                                  request_params,
                                                  s->callback,
                                                  cronet_runtime_context->executor);
        if (result != Cronet_RESULT_SUCCESS) {
            LOGE(TAG, "Cronet_UrlRequest_InitWithParams error %d.\n", result);
            ret = AVERROR_UNKNOWN;
            break;
        }

        // Starting request.
        result = Cronet_UrlRequest_Start(s->request);
        if (result != Cronet_RESULT_SUCCESS) {
            LOGE(TAG, "Cronet_UrlRequest_Start error %d\n", result);
            ret = AVERROR_UNKNOWN;
            break;
        }


        if (s->error_state != 1) {
            s->read_pos     = start;
            s->write_pos    = start;
        }

        s->is_open      = 1;

    } while (0);

    if (ret != 0) {
        if (s->request != NULL) {
            Cronet_UrlRequest_Destroy(s->request);
            s->request = NULL;
        }

        if (s->callback != NULL) {
            Cronet_UrlRequestCallback_Destroy(s->callback);
            s->callback = NULL;
        }
        post_return_value(task, ret);
    }

    if (request_params != NULL) {
        Cronet_UrlRequestParams_Destroy(request_params);
    }

    if (header != NULL) {
        Cronet_HttpHeader_Destroy(header);
    }

    //post_return_value(task, ret);
    LOGI(TAG, "finish process range task\n");
}

static void process_task(CronetTask *task) {
    if (task == NULL) {
        return;
    }

    LOGI(TAG, "msg queue msg type:%d\n", task->type);
    switch (task->type) {
        case kCronetTaskType_Runnable:
            process_runnable_task(task);
            break;
        case kCronetTaskType_FF_Open:
            process_open_task(task);
            break;
        case kCronetTaskType_FF_Close:
            process_close_task(task);
            break;
        case kCronetTaskType_FF_Reset:
            process_reset_task(task);
            break;
        case kCronetTaskType_FF_Range:
            process_range_task(task);
            break;
        default:
            break;
    }
}

static void cronet_execute(Cronet_ExecutorPtr self, Cronet_RunnablePtr runnable) {
    CronetRuntimeContext* cronet_runtime_context = Cronet_Executor_GetClientContext(self);
    if (runnable == NULL || cronet_runtime_context == NULL) {
        return;
    }

    pthread_mutex_lock(&cronet_runtime_context->executor_task_mutex);
    if (!cronet_runtime_context->executor_stop_thread_loop) {
        cronet_enqueue_task(&cronet_runtime_context->executor_task_queue,
                            create_runnable_task(runnable));
        pthread_cond_signal(&cronet_runtime_context->executor_task_cond);
    }
    pthread_mutex_unlock(&cronet_runtime_context->executor_task_mutex);
}

static void *executor_thread_loop(void *arg) {
    CronetRuntimeContext* cronet_runtime_context = (CronetRuntimeContext*)arg;
    if (cronet_runtime_context == NULL) {
        return NULL;
    }

    // Process task in task queue.
    while (1) {
        CronetTask *task = NULL;

        // Wait for a task to run or stop signal.
        pthread_mutex_lock(&cronet_runtime_context->executor_task_mutex);
        while (cronet_runtime_context->executor_task_queue.size == 0 &&
               !cronet_runtime_context->executor_stop_thread_loop) {
            pthread_cond_wait(&cronet_runtime_context->executor_task_cond,
                              &cronet_runtime_context->executor_task_mutex);
        }

        if (cronet_runtime_context->executor_stop_thread_loop) {
            pthread_mutex_unlock(&cronet_runtime_context->executor_task_mutex);
            break;
        }

        if (cronet_runtime_context->executor_task_queue.size == 0) {
            pthread_mutex_unlock(&cronet_runtime_context->executor_task_mutex);
            continue;
        }

        task = cronet_dequeue_task(&cronet_runtime_context->executor_task_queue);
        pthread_mutex_unlock(&cronet_runtime_context->executor_task_mutex);

        process_task(task);
    }

    // Uninitialize task queue.
    pthread_mutex_lock(&cronet_runtime_context->executor_task_mutex);
    cronet_uninit_task_queue(&cronet_runtime_context->executor_task_queue);
    pthread_mutex_unlock(&cronet_runtime_context->executor_task_mutex);

    return NULL;
}

// Create cronet engine.
static Cronet_EnginePtr create_cronet_engine(const char *hosts[], int len) {
    LOGI(TAG, "QH Cronet %s\n", CRONET_FFMPEG_USER_AGENT);
    
    Cronet_RESULT result                    = Cronet_RESULT_SUCCESS;
    Cronet_EnginePtr cronet_engine          = Cronet_Engine_Create();
    Cronet_EngineParamsPtr engine_params    = Cronet_EngineParams_Create();
    Cronet_EngineParams_user_agent_set(engine_params, CRONET_FFMPEG_USER_AGENT);
//    Cronet_EngineParams_experimental_options_set(engine_params, "{\"QUIC\":{\"quic_version\":\"h3-43\", \"idle_connection_timeout_seconds\":10}}");
    // Enable HTTP2.
   Cronet_EngineParams_enable_http2_set(engine_params, 1);
    // Enable QUIC.
    Cronet_EngineParams_enable_quic_set(engine_params, 1);
    
    Cronet_EngineParams_enable_brotli_set(engine_params, 0);

    // bool h2 = Cronet_EngineParams_enable_http2_get(engine_params);
    // bool qc = Cronet_EngineParams_enable_quic_get(engine_params);
    // LOGI(TAG, "Cronet_EngineParams_enable_http2_get %i\n", h2);
    // LOGI(TAG, "Cronet_EngineParams_enable_quic_get %i\n", qc);
    
    // Cronet_String v = Cronet_Engine_GetVersionString(cronet_engine);
    // LOGI(TAG, "QH Cronet_Engine_GetVersionString %s\n", v);

    if (len > 0) {
        for (int i = 0; i < len; i++) {
            Cronet_String host = hosts[i];
            Cronet_QuicHintPtr element = Cronet_QuicHint_Create();
            Cronet_QuicHint_host_set(element, host);
            Cronet_QuicHint_port_set(element, 443);
            Cronet_QuicHint_alternate_port_set(element, 443);
            Cronet_EngineParams_quic_hints_add(engine_params, element);

            LOGI(TAG, "QH Cronet Cronet_EngineParams_quic_hints_add %s\n", host);
        }
    }
    
    // Start cronet engine.
    result = Cronet_Engine_StartWithParams(cronet_engine, engine_params);
    if (result != Cronet_RESULT_SUCCESS) {
        LOGE(TAG, "Cronet_Engine_StartWithParams error %d\n", result);
        Cronet_Engine_Destroy(cronet_engine);
        cronet_engine = NULL;
    }
    Cronet_EngineParams_Destroy(engine_params);
    return cronet_engine;
}

int av_format_cronet_init(const char *hosts[], int len) {
    int ret = 0;
    do {
        if (cronet_engine != NULL) {
            break;
        }

        //Setup Cronet_Engine.
        cronet_engine = create_cronet_engine(hosts, len);
        if (cronet_engine == NULL) {
            LOGE(TAG, "create_cronet_engine fail\n");
            ret = AVERROR(EINVAL);
            break;
        }
    } while (0);
    return ret;
}

void av_format_cronet_uninit(void) {
    if (cronet_engine != NULL) {
        Cronet_Engine_Shutdown(cronet_engine);
        Cronet_Engine_Destroy(cronet_engine);
        destroy_cronet_runtime_context(cronetRuntimeCtx);
        cronetRuntimeCtx = NULL;
        cronet_engine = NULL;
    }
}

CronetRuntimeContext* create_cronet_runtime_context(void) {
    // Alloc CronetRuntimeContext.
    CronetRuntimeContext* cronet_runtime_context = av_mallocz(sizeof(CronetRuntimeContext));
    cronet_runtime_context->executor_stop_thread_loop = 0;

    // Initialize executor task mutex and condition variable.
    pthread_mutex_init(&cronet_runtime_context->executor_task_mutex, NULL);
    pthread_cond_init(&cronet_runtime_context->executor_task_cond, NULL);
    // Initialize task queue.
    cronet_init_task_queue(&cronet_runtime_context->executor_task_queue);
    // Create executor thread.
    if (pthread_create(&cronet_runtime_context->executor_thread,
                        NULL,
                        executor_thread_loop,
                        cronet_runtime_context)) {
        LOGE(TAG, "Cronet pthread_create fail\n");
        cronet_uninit_task_queue(&cronet_runtime_context->executor_task_queue);
        pthread_mutex_destroy(&cronet_runtime_context->executor_task_mutex);
        pthread_cond_destroy(&cronet_runtime_context->executor_task_cond);
        av_freep(&cronet_runtime_context);
        return NULL;
    }

    // Setup Cronet_Executor.
    cronet_runtime_context->executor = Cronet_Executor_CreateWith(cronet_execute);
    Cronet_Executor_SetClientContext(cronet_runtime_context->executor, cronet_runtime_context);
    return cronet_runtime_context;
}

void destroy_cronet_runtime_context(CronetRuntimeContext* cronet_runtime_context) {
    // Not intialized.
    if (cronet_runtime_context == NULL) {
        return;
    }

    // Stop executor thread.
    pthread_mutex_lock(&cronet_runtime_context->executor_task_mutex);
    cronet_runtime_context->executor_stop_thread_loop = 1;
    pthread_cond_signal(&cronet_runtime_context->executor_task_cond);
    pthread_mutex_unlock(&cronet_runtime_context->executor_task_mutex);
    pthread_join(cronet_runtime_context->executor_thread, NULL);

    // Destroy executor task mutex and condition variable.
    pthread_mutex_destroy(&cronet_runtime_context->executor_task_mutex);
    pthread_cond_destroy(&cronet_runtime_context->executor_task_cond);

    // Destroy Cronet_Executor
    if (cronet_runtime_context->executor != NULL) {
        Cronet_Executor_Destroy(cronet_runtime_context->executor);
        cronet_runtime_context->executor = NULL;
    }

    // Destroy CronetRuntimeContext.
    av_freep(&cronet_runtime_context);
}

char* av_format_cronet_get_current_protocol(void) {
    return cronet_protocol;
}

static int process_headers(URLContext *h, Cronet_UrlResponseInfoPtr info) {
    uint32_t header_num = 0;
    const char *slash   = NULL;
    const char *p       = NULL;
    CronetContext *s    = NULL;

    if (h == NULL || info == NULL) {
        LOGE(TAG, "urlcontext h empty error\n");
        return 0;
    }

    int http_code = Cronet_UrlResponseInfo_http_status_code_get(info);
    if (http_code != 200 && http_code != 206) {
        LOGE(TAG, "process headers http code error:%d\n", http_code);
        return 0;
    }

    s = h->priv_data;
    if (s == NULL) {
        LOGE(TAG, "cronetcontext s empty error\n");
        return 0;
    }

    if (!s->is_open) {
        LOGE(TAG, "cronetcontext s is not open\n");
        return 0;
    }

    header_num = Cronet_UrlResponseInfo_all_headers_list_size(info);
    for (uint32_t i = 0; i < header_num; ++i) {
        Cronet_HttpHeaderPtr header = Cronet_UrlResponseInfo_all_headers_list_at(info, i);
        Cronet_String key           = Cronet_HttpHeader_name_get(header);
        Cronet_String value         = Cronet_HttpHeader_value_get(header);

        LOGI(TAG, "http headers, cronet key:%s, value:%s, seekable:%d\n", key, value, s->seekable);

        if (!av_strcasecmp(key, "Content-Range")) {
            p = value;
            if (!strncmp(p, "bytes ", 6)) {
                p += 6;
                s->offset       = strtoull(p, NULL, 10);
                s->read_pos     = s->offset;
                if ((slash = strchr(p, '/')) && strlen(slash) > 0) {
                    s->file_size = strtoull(slash + 1, NULL, 10);
                }
            }
            if (s->seekable == -1 && s->file_size != UINT64_MAX) {
                h->is_streamed = 0;
            }
        } else if (!av_strcasecmp(key, "Content-Length") && s->file_size == UINT64_MAX) {
            s->file_size = strtoull(value, NULL, 10);
        } else if (!av_strcasecmp(key, "Accept-Ranges") &&
                   !strncmp(value, "bytes", 5) &&
                   s->seekable == -1) {
            h->is_streamed = 0;
        }
    }

    LOGI(TAG, "http header processed, final file size is %"PRIu64"\n", s->file_size);
    return 1;
}



// Implementation of Cronet_UrlRequestCallback methods.
static void on_redirect_received(Cronet_UrlRequestCallbackPtr self,
                                 Cronet_UrlRequestPtr request,
                                 Cronet_UrlResponseInfoPtr info,
                                 Cronet_String newLocationUrl) {
    if (request != NULL) {
        Cronet_UrlRequest_FollowRedirect(request);
    }
}

static void on_response_started(Cronet_UrlRequestCallbackPtr self,
                                Cronet_UrlRequestPtr request,
                                Cronet_UrlResponseInfoPtr info) {
    LOGI(TAG, "begin header process\n");
    Cronet_String egotiated_protocol = Cronet_UrlResponseInfo_negotiated_protocol_get(info);
    LOGI(TAG, "QH Cronet egotiated_protocol:%s\n", egotiated_protocol);
    cronet_protocol = egotiated_protocol;
    Cronet_BufferPtr buffer = NULL;

    // Process headers.
    URLContext *h = (URLContext *)Cronet_UrlRequestCallback_GetClientContext(self);
    CronetContext *s = h->priv_data;

    if (s == NULL)
        return;

    if (s->header_read_finished == 1) {
        LOGI(TAG, "header has been read\n");
        return;
    }

    if (s->is_open == 0) {
        LOGE(TAG, "cronetcontext has been closed\n");
        return;
    }

    s->header_read_finished = 1;
    if (!process_headers(h, info)) {
        LOGI(TAG, "header process error\n");
        post_return_value(s->current_task, 1);
        return;
    }


    // Don't call Cronet_Buffer_Destroy manually, it will be
    // deleted automatically.

    buffer = Cronet_Buffer_Create();
    s->buffer = buffer;
    Cronet_Buffer_InitWithAlloc(buffer, CRONET_DEFAULT_BUFFER_SIZE);

    Cronet_UrlRequest_Read(request, s->buffer);
    LOGI(TAG, "header process finished, before notify :%d\n", s->fifo_size);

    post_return_value(s->current_task, 0);
    LOGI(TAG, "header process finished :%d\n", s->fifo_size);
}

static void on_read_completed(Cronet_UrlRequestCallbackPtr self,
                              Cronet_UrlRequestPtr request,
                              Cronet_UrlResponseInfoPtr info,
                              Cronet_BufferPtr buffer,
                              uint64_t bytes_read) {
//    Cronet_String egotiated_protocol = Cronet_UrlResponseInfo_negotiated_protocol_get(info);
//    LOGI(TAG, "QH read egotiated_protocol:%s\n", egotiated_protocol);
    char *buf           = NULL;
    int size            = 0;
    int capacity        = 0;
    int write_len       = 0;
    URLContext *h       = NULL;
    CronetContext *s    = NULL;

    LOGI(TAG, "read completed callback, bytes:%"PRIu64"\n", bytes_read);

    h = (URLContext *)Cronet_UrlRequestCallback_GetClientContext(self);
    if (h == NULL) {
        return;
    }

    s = h->priv_data;
    if (s == NULL) {
        return;
    }

    if (!s->is_open) {
        return;
    }

    buf     = (char*)Cronet_Buffer_GetData(buffer);
    size    = (int)bytes_read;

    pthread_mutex_lock(&s->fifo_mutex);
    do {
        if (s->fifo == NULL) {
            break;
        }

        // Try to write.
        write_len = av_fifo_generic_write(s->fifo, (uint8_t *)buf, size, NULL);
        if (write_len != size) {
            // Should not happen.
            LOGI(TAG, "FIFO written %d, expect %d.\n", write_len, size);
        }
#if APPLICATION        
        av_application_did_io_tcp_read(s->app_ctx, (void*)h, write_len);
#endif
        // Notify read thread.
        pthread_cond_signal(&s->fifo_cond);

        // Update write_pos.
        s->write_pos += write_len;
        if(av_fifo_space(s->fifo) < CRONET_DEFAULT_BUFFER_SIZE ) {
            s->buffer_overflow = 1;
        }

        // Continue reading.
        if (s->buffer_overflow == 0) {
            if (s->file_size == UINT64_MAX || s->write_pos <= s->file_size) {
                Cronet_UrlRequest_Read(request, buffer);
            }
        } else {
            LOGE(TAG, "buffer overflow\n");
        }
    } while (0);
    pthread_mutex_unlock(&s->fifo_mutex);
}

static void on_succeeded(Cronet_UrlRequestCallbackPtr self,
                         Cronet_UrlRequestPtr request,
                         Cronet_UrlResponseInfoPtr info) {
    URLContext *h       = NULL;
    CronetContext *s    = NULL;
    do {
        h = (URLContext *)Cronet_UrlRequestCallback_GetClientContext(self);
        if (h == NULL) {
            break;
        }

        s = h->priv_data;
        if (s == NULL) {
            break;
        }

        if (s->request != NULL) {
            Cronet_UrlRequest_Destroy(s->request);
            s->request = NULL;
        }

        if (s->callback != NULL) {
            Cronet_UrlRequestCallback_Destroy(s->callback);
            s->callback = NULL;
        }

        //s->is_open = 2;
        stop_read(s, 2);
    } while (0);

    LOGI(TAG, "Cronet request success.\n");
}

static void on_failed(Cronet_UrlRequestCallbackPtr self,
                      Cronet_UrlRequestPtr request,
                      Cronet_UrlResponseInfoPtr info,
                      Cronet_ErrorPtr error) {
    URLContext *h       = NULL;
    CronetContext *s    = NULL;
    do {
        h = (URLContext *)Cronet_UrlRequestCallback_GetClientContext(self);
        if (h == NULL) {
            break;
        }

        s = h->priv_data;
        if (s == NULL) {
            break;
        }

        if (s->request != NULL) {
            Cronet_UrlRequest_Destroy(s->request);
            s->request = NULL;
        }

        if (s->callback != NULL) {
            Cronet_UrlRequestCallback_Destroy(s->callback);
            s->callback = NULL;
        }

        if (s->header_read_finished == 0) {
            LOGE(TAG, "range task failed notify\n");
            post_return_value(s->current_task, 1);
        }

        //s->is_open = 2;
        s->error_state = 1;
        stop_read(s, 2);
    } while (0);

    //LOGE(TAG, "Cronet Request error %d %s.", Cronet_Error_error_code_get(error), Cronet_Error_message_get(error));
    LOGE(TAG, "Cronet Request Failed\n");
}

static void on_canceled(Cronet_UrlRequestCallbackPtr self,
                        Cronet_UrlRequestPtr request,
                        Cronet_UrlResponseInfoPtr info) {
    URLContext *h       = NULL;
    CronetContext *s    = NULL;
    do {
        h = (URLContext *)Cronet_UrlRequestCallback_GetClientContext(self);
        if (h == NULL) {
            break;
        }

        s = h->priv_data;
        if (s == NULL) {
            break;
        }

        if (s->request != NULL) {
            Cronet_UrlRequest_Destroy(s->request);
            s->request = NULL;
        }

        if (s->callback != NULL) {
            Cronet_UrlRequestCallback_Destroy(s->callback);
            s->callback = NULL;
        }

        if (s->header_read_finished == 0) {
            LOGE(TAG, "range task canceled notify\n");
            post_return_value(s->current_task, 1);
        }

        s->is_open = 0;
    } while (0);
    LOGI(TAG, "cronet canceled\n");
    post_return_value(s->closing_task, 0);
    s->closing_task = NULL;
}

int cronet_open(URLContext *h, const char *uri, int flags, AVDictionary **options) {
    int ret = 0;
    int64_t start = -1, end = -1;
    CronetContext *s    = h->priv_data;
    if (cronetRuntimeCtx == NULL) {
        cronetRuntimeCtx = create_cronet_runtime_context();
        s->cronet_runtime_ctx = cronetRuntimeCtx;
    }
    else if (s->cronet_runtime_ctx == NULL) {
        s->cronet_runtime_ctx = cronetRuntimeCtx;
    }
    if (cronet_engine == NULL) {
        ret = AVERROR_UNKNOWN;
    } else {
        AVDictionaryEntry *ent = av_dict_get(*options, "start", NULL, 0);
        if (ent != NULL) {
            start = (int64_t)strtol(ent->value, NULL, 10);
        }
        ent = av_dict_get(*options, "end", NULL, 0);
        if (ent != NULL) {
            end = (int64_t)strtol(ent->value, NULL, 10);
        }
        ret = invoke_task(create_open_task(h, uri, start, end));
        if (ret == 1) {
            LOGE(TAG, "cronet_open failed:%d\n", ret);
            ret = AVERROR(EIO);
        }
    }
    av_dict_free(options);
    return ret;
}

int cronet_close(URLContext *h) {
    int ret = 0;
    if (cronet_engine == NULL) {
        ret = AVERROR_UNKNOWN;
    } else {
        ret = invoke_task(create_close_task(h));
    }
    LOGI(TAG, "cronet close\n");
    return ret;
}

int cronet_read(URLContext *h, uint8_t *buf, int size) {
    LOGI(TAG, "cronet_read");
    int ret             = 0;
    int avail           = 0;
    CronetContext *s    = h->priv_data;
    pthread_mutex_lock(&s->fifo_mutex);
    LOGI(TAG, "will read\n");
    int attempts = 0;
    do {
        //if (!s->is_open || s->fifo == NULL) {
        if (s == NULL || s->fifo == NULL) {
            ret = AVERROR_UNKNOWN;
            LOGE(TAG, "fifo is null cronet read exit\n");
            break;
        }

//        if (attempts++ > s->max_retry) {
//            ret = AVERROR_UNKNOWN;
//            LOGE(TAG, "max retry exceed\n");
//            break;
//        }

        if (!h->is_streamed) {
            if (s->error_state == 1 && s->is_open == 2) {
                LOGE(TAG, "redo range\n");
                av_usleep(100 * 1000);

                s->header_read_finished = 0;
                int range_ret = invoke_task(create_range_task(h, s->write_pos, -1));
                LOGE(TAG, "range_ret:%d\n", range_ret);
                if (range_ret == 1) {
                    s->error_state = 1;
                    s->is_open = 2;
                } else {
                    s->error_state = 0;
                    s->is_open = 1;
                }

            }
        }


        if (s->file_size != UINT64_MAX && s->read_pos >= s->file_size) {
            ret = AVERROR_EOF;
            LOGE(TAG, "eof cronet read exit\n");
            break;
        }

        avail = av_fifo_size(s->fifo);
        if (avail > 0) {
            // If some data available, return immediately.
            ret = FFMIN(avail, size);
            LOGI(TAG, "did read ,ret :%d\n", ret);
            av_fifo_generic_read(s->fifo, buf, ret, NULL);

            //Update read_pos.
            s->read_pos += ret;

            if (s->request) {
                if (s->is_open == 1 && s->error_state == 0 &&
                s->buffer_overflow == 1 && av_fifo_space(s->fifo) >= CRONET_DEFAULT_BUFFER_SIZE) {
                    LOGE(TAG, "read again\n");
                    s->buffer_overflow = 0;
                    Cronet_UrlRequest_Read(s->request, s->buffer);
                }
            }
            LOGI(TAG, "normal cronet read exit\n");

            break;
        }

        if (s->is_open == 2) {
            if (s->error_state == 1) {
                LOGE(TAG, "cronet read eio\n");
                ret = AVERROR(EIO);
                break;
            } else {
                ret = AVERROR_EOF;
                break;
            }
        }
        // Otherwise, wait for data.
        if (s->timeout == -1) {
            LOGE(TAG, "dead wait\n");
            pthread_cond_wait(&s->fifo_cond, &s->fifo_mutex);
        } else {
            LOGE(TAG, "con wait\n");
            int64_t t = av_gettime() + s->timeout;
            struct timespec tv = { .tv_sec  =  t / 1000000,
                    .tv_nsec = (t % 1000000) * 1000 };
            pthread_cond_timedwait(&s->fifo_cond, &s->fifo_mutex, &tv);
        }
        // Continue to read in next loop after got notified.
        LOGI(TAG, "continue\n");
        continue;
    } while (1);

    LOGI(TAG, "final cronet read exit\n");

    pthread_mutex_unlock(&s->fifo_mutex);
    return ret;
}

int cronet_write(URLContext *h, const uint8_t *buf, int size) {
    // Not implemented yet.
    return AVERROR(ENOSYS);
}

static int request_range(URLContext *h, int64_t start, int64_t end) {
    int ret = -1;
    do {
        LOGI(TAG, "will do reset task\n");
        ret = invoke_task(create_reset_task(h));
        if (ret != 0) {
            LOGE(TAG, "reset task error\n");
            break;
        }
        LOGI(TAG, "did do reset task\n");
        ret = invoke_task(create_range_task(h, start, end));
        LOGI(TAG, "do range task exit\n");
    } while (0);
    return ret;
}

int64_t cronet_seek(URLContext *h, int64_t off, int whence) {
    LOGI(TAG, "in cronet seek, streamed:%d\n", h->is_streamed);
    int64_t ret         = -1;
    int seek_from_net   = 0;
    CronetContext *s = h->priv_data;
    pthread_mutex_lock(&s->fifo_mutex);
    do {
        if (s == NULL || s->fifo == NULL) {
            ret = AVERROR_UNKNOWN;
            break;
        }

        if (whence == AVSEEK_SIZE) {
            ret = s->file_size;
            break;
        }

        if (((whence == SEEK_CUR && off == 0) || (whence == SEEK_SET && off == s->read_pos))) {
            ret = s->read_pos;
            break;
        }

        if ((s->file_size == UINT64_MAX && whence == SEEK_END)) {
            ret = AVERROR(ENOSYS);
            break;
        }

        if (whence == SEEK_CUR) {
            off += s->read_pos;
        } else if (whence == SEEK_END) {
            off += s->file_size;
        } else if (whence != SEEK_SET) {
            ret = AVERROR(EINVAL);
            break;
        }

        if (off < 0) {
            ret = AVERROR(EINVAL);
            break;
        }

        if (off && h->is_streamed) {
            ret = AVERROR(ENOSYS);
            break;
        }

        ret = off;

        // Check seek from buffer or net.
        if (off > s->read_pos && off < s->write_pos) {
            LOGI(TAG, "seek from buffer\n");
            av_fifo_drain(s->fifo, off - s->read_pos);
            s->read_pos = off;
        } else if (off < s->read_pos || off >= s->write_pos) {
            seek_from_net = 1;
            break;
        }
    } while (0);
    pthread_mutex_unlock(&s->fifo_mutex);

    if (seek_from_net && request_range(h, off, -1) != 0) {
        LOGE(TAG, "range from net error\n");
        ret = AVERROR(EINVAL);
    }

    LOGI(TAG, "seek return %"PRIi64"\n", ret);
    return ret;
}

const URLProtocol ff_cronet_protocol = {
        .name                = "cronet",
        .url_open2           = cronet_open,
        .url_close           = cronet_close,
        .url_read            = cronet_read,
        .url_write           = cronet_write,
        .url_seek            = cronet_seek,
        .priv_data_size      = sizeof(CronetContext),
        .priv_data_class     = &cronet_context_class,
        .flags               = URL_PROTOCOL_FLAG_NETWORK,
        .default_whitelist   = "cronet,cronets"
};

const URLProtocol ff_cronets_protocol = {
        .name                = "cronets",
        .url_open2           = cronet_open,
        .url_close           = cronet_close,
        .url_read            = cronet_read,
        .url_write           = cronet_write,
        .url_seek            = cronet_seek,
        .priv_data_size      = sizeof(CronetContext),
        .priv_data_class     = &cronets_context_class,
        .flags               = URL_PROTOCOL_FLAG_NETWORK,
        .default_whitelist   = "cronet,cronets"
};
