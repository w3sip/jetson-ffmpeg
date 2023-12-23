
#include "nvmpi.h"
#include "NvVideoDecoder.h"
#include "NvLogging.h"
#include "nvUtils2NvBuf.h"
#include <vector>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <queue>
#include <mutex>
#include <condition_variable>

#define CHUNK_SIZE 4000000
#define MAX_BUFFERS 32

//---------------------------------------------------------------------------------------------------------------------
static int loggingEnabled()
{
    const char* var = getenv("NVMPI_DEBUG");
    if (var) {
        log_level = LOG_LEVEL_DEBUG;
    }
    return var ? 1 : 0;
}
static const int gNVMPIDebug = loggingEnabled();

#define LOG_ERR(msg)        cout << msg << endl;
#define LOG_DBG(msg)        if ( gNVMPIDebug ) { cout << msg << endl; }
#define VER "0.03"

#define TEST_ERROR(condition, message, errorCode)    \
    if (condition) {                                 \
        LOG_ERR( message );                          \
        return errorCode;                            \
    }

#define TEST_ERRORV(condition, message)              \
    if (condition) {                                 \
        LOG_ERR( message );                          \
        return;                                      \
    }

#define WARN_ERROR(condition, message)               \
    if (condition) {                                 \
        LOG_ERR( message );                          \
    }

using namespace std;

//---------------------------------------------------------------------------------------------------------------------
class NVMPI_framePool
{
    int                         m_bufCount = 0; //total number of allocated buffers
    int*                        m_dstDmaFd = NULL;
#ifdef WITH_NVUTILS
    NvBufSurface**              m_dstDmaSurface = NULL;
#endif

    std::mutex                  m_emptyBufMtx;
    std::mutex                  m_filledBufMtx;
    std::queue<int>             m_emptyBuf;     //list of buffers available to fill
    std::queue<int>             m_filledBuf;    //filled buffers to consume
    unsigned long long          m_timestamp[MAX_BUFFERS];

public:
    int init(const int& imgW, const int& imgH, const NvBufferColorFormat& cFmt, const int& bufNumber);
    void deinit();

    int dqEmptyBuf();
    void qEmptyBuf(int bIndex);

    int dqFilledBuf();
    void qFilledBuf(int bIndex);

    long long getTimestamp(int bIndex) const { return m_timestamp[bIndex]; }
    void setTimestamp(int bIndex, long long t) { m_timestamp[bIndex] = t; }
#ifdef WITH_NVUTILS
    NvBufSurface* getDmaSurface(int bIndex) { return m_dstDmaSurface[bIndex]; }
#else
    int getDmaFd(int bIndex) { return m_dstDmaFd[bIndex]; }
#endif
};

//---------------------------------------------------------------------------------------------------------------------
int NVMPI_framePool::init(const int& imgW, const int& imgH, const NvBufferColorFormat& cFmt, const int& bufNumber)
{
    TEST_ERROR( bufNumber <= 0, "Invalid parameter to init() - bufNumber=" << bufNumber, false );

    int ret=0;
    m_bufCount = bufNumber;
    m_dstDmaFd = new int[m_bufCount]; //set default to -1
    for (int index = 0; index < m_bufCount; index++) {
        m_dstDmaFd[index] = 0;
    }
#ifdef WITH_NVUTILS
    m_dstDmaSurface = new NvBufSurface*[m_bufCount];
#endif

    NvBufferCreateParams input_params = {0};
    /* Create PitchLinear output buffer for transform. */
    input_params.width = imgW;
    input_params.height = imgH;
    input_params.layout = NvBufferLayout_Pitch;
    input_params.colorFormat = cFmt;
#ifdef WITH_NVUTILS
    input_params.memType = NVBUF_MEM_SURFACE_ARRAY;
    input_params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;

    ret = NvBufSurf::NvAllocate(&input_params, m_bufCount, m_dstDmaFd);
    TEST_ERROR(ret < 0, "NvAllocate failed, err=" << ret, false);
    for (int index = 0; index < m_bufCount; index++) {
        ret = NvBufSurfaceFromFd(m_dstDmaFd[index], (void**)(&(m_dstDmaSurface[index])));
        TEST_ERROR(ret < 0, "Failed to get surface for buffer, err=" << ret, false);
    }
#else
    input_params.payloadType = NvBufferPayload_SurfArray;
    input_params.nvbuf_tag = NvBufferTag_VIDEO_DEC;

    for (int index = 0; index < m_bufCount; index++) {
        ret = NvBufferCreateEx(&(m_dstDmaFd[index]), &input_params);
        TEST_ERROR(ret < 0, "Failed to create buffers, err=" << ret, -1);
    }
#endif

    std::unique_lock<std::mutex> lk(m_emptyBufMtx);
    for (int index = 0; index < m_bufCount; index++) {
        m_emptyBuf.push(index);
    }

    return 0;
}

//---------------------------------------------------------------------------------------------------------------------
void NVMPI_framePool::deinit()
{
    std::unique_lock<std::mutex> lkEmpty(m_emptyBufMtx);
    std::unique_lock<std::mutex> lkFilled(m_filledBufMtx);

    int ret = 0;
    int numBufPopped = 0;
    while(!m_emptyBuf.empty()) {
        m_emptyBuf.pop();
        numBufPopped++;
    }
    while(!m_filledBuf.empty()) {
        m_filledBuf.pop();
        numBufPopped++;
    }

    //TODO protection from this situation.
    WARN_ERROR(numBufPopped < m_bufCount, "FIXME! Trying to deinit mvmpi decoder framepool, but some buffers are missing. It can couse seg. fault if buffer still in use");

    for (int index = 0; index < m_bufCount; index++) {
        if (m_dstDmaFd[index] != 0) {
            ret = NvBufferDestroy(m_dstDmaFd[index]);
            WARN_ERROR(ret < 0, "Failed to Destroy NvBuffer");
        }
    }

    delete[] m_dstDmaFd;
    m_dstDmaFd = NULL;
#ifdef WITH_NVUTILS
    delete[] m_dstDmaSurface;
    m_dstDmaSurface = NULL;
#endif

    m_bufCount = 0;
}

//---------------------------------------------------------------------------------------------------------------------
void NVMPI_framePool::qFilledBuf(int bIndex)
{
    std::unique_lock<std::mutex> lk(m_filledBufMtx);
    m_filledBuf.push(bIndex);
}

//---------------------------------------------------------------------------------------------------------------------
//TODO block and wait on mutex before reaching timeout or getting buffer
int NVMPI_framePool::dqFilledBuf()
{
    int ret = -1;
    std::unique_lock<std::mutex> lk(m_filledBufMtx);
    if(!m_filledBuf.empty()) {
        ret = m_filledBuf.front();
        m_filledBuf.pop();
    }
    return ret;
}

//---------------------------------------------------------------------------------------------------------------------
void NVMPI_framePool::qEmptyBuf(int bIndex)
{
    std::unique_lock<std::mutex> lk(m_emptyBufMtx);
    m_emptyBuf.push(bIndex);
}

//---------------------------------------------------------------------------------------------------------------------
//TODO block and wait on mutex before reaching timeout or getting buffer
int NVMPI_framePool::dqEmptyBuf()
{
    int ret = -1;
    std::unique_lock<std::mutex> lk(m_emptyBufMtx);
    if(!m_emptyBuf.empty()) {
        ret = m_emptyBuf.front();
        m_emptyBuf.pop();
    }
    return ret;
}

//---------------------------------------------------------------------------------------------------------------------
class nvmpictx
{
    NvVideoDecoder*                 m_decoder{nullptr};
    bool                            m_terminated{false};
    int                             m_index{0};
    unsigned int                    m_codedWidth{0};
    unsigned int                    m_codedHeight{0};

    int                             m_numberCaptureBuffers{0};

    int                             m_dmaBufferFd[MAX_BUFFERS];
    unsigned long                   m_packetsSubmitted{0};
    int                             m_framesRead{0};
    int64_t                         m_prevFrameTs{0};

#ifdef WITH_NVUTILS
    NvBufSurface*                   m_dmaBufferSurface[MAX_BUFFERS];
    NvBufSurfTransformConfigParams  m_session;
#else
    NvBufferSession                 m_session;
#endif
    NvBufferTransformParams         m_transformParams;
    NvBufferRect                    m_srcRect;
    NvBufferRect                    m_destRect;

    nvPixFormat                     m_outputPixfmt;
    unsigned int                    m_decoderPixfmt{0};
    std::thread                     m_thread;

    NVMPI_framePool                 m_framePool;

    //frame size params
    unsigned int                    m_frameSize[MAX_NUM_PLANES];
    unsigned int                    m_frameLinesize[MAX_NUM_PLANES];
    unsigned int                    m_frameHeight[MAX_NUM_PLANES];

#if SAVE_BITSTREAM
    FILE*                           m_bitstreamFile;
#endif

private:
    static void dec_capture_loop_fcn(void *arg);

    //empty frame queue and free buffers memory
    void        deinitFramePool();
    //alloc frame buffers based on m_frameSize data in nvmpictx
    int         initFramePool();

    //get dst_dma buffer params and set corresponding frame size and linesize in nvmpictx
    int         updateFrameSizeParams();
    int         updateBufferTransformParams();

    int         initDecoderCapturePlane(v4l2_format &format);
    /* deinitPlane unmaps the buffers and calls REQBUFS with count 0 */
    void        deinitDecoderCapturePlane();
    void        deinitDecoderOutputPlane();
    void        captureLoop();
    int         respondToResolutionEvent(v4l2_format &format, v4l2_crop &crop);

public:
    int         init(nvCodingType codingType, nvPixFormat pixFormat);
    int         putPacket(nvPacket* packet);
    int         getFrame(nvFrame* frame, bool wait);
    void        close();
};

//---------------------------------------------------------------------------------------------------------------------
NvBufferColorFormat getNvColorFormatFromV4l2Format(v4l2_format &format)
{
    NvBufferColorFormat ret_cf = NvBufferColorFormat_NV12;
    switch (format.fmt.pix_mp.colorspace) {
    case V4L2_COLORSPACE_SMPTE170M:
        if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT) {
            // "Decoder colorspace ITU-R BT.601 with standard range luma (16-235)"
            ret_cf = NvBufferColorFormat_NV12;
        } else {
            //"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
            ret_cf = NvBufferColorFormat_NV12_ER;
        }
        break;
    case V4L2_COLORSPACE_REC709:
        if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT) {
            //"Decoder colorspace ITU-R BT.709 with standard range luma (16-235)";
            ret_cf = NvBufferColorFormat_NV12_709;
        } else {
            //"Decoder colorspace ITU-R BT.709 with extended range luma (0-255)";
            ret_cf = NvBufferColorFormat_NV12_709_ER;
        }
        break;
    case V4L2_COLORSPACE_BT2020:
        //"Decoder colorspace ITU-R BT.2020";
        ret_cf = NvBufferColorFormat_NV12_2020;
        break;
    default:
        if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT) {
            //"Decoder colorspace ITU-R BT.601 with standard range luma (16-235)";
            ret_cf = NvBufferColorFormat_NV12;
        } else {
            //"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
            ret_cf = NvBufferColorFormat_NV12_ER;
        }
        break;
    }
    return ret_cf;
}


//---------------------------------------------------------------------------------------------------------------------
int nvmpictx::initDecoderCapturePlane(v4l2_format &format)
{
    int ret=0;
    int32_t minimumDecoderCaptureBuffers;
    NvBufferCreateParams cParams = {0};

    ret=m_decoder->setCapturePlaneFormat(format.fmt.pix_mp.pixelformat,format.fmt.pix_mp.width,format.fmt.pix_mp.height);
    TEST_ERROR(ret < 0, "Error in setting decoder capture plane format, err=" << ret, -1);

    m_decoder->getMinimumCapturePlaneBuffers(minimumDecoderCaptureBuffers);
    TEST_ERROR(ret < 0, "Error while getting value of minimum capture plane buffers, err=" << ret, -1);

    /* Request (min + extra) buffers, export and map buffers. */
    m_numberCaptureBuffers = minimumDecoderCaptureBuffers + 5;

    cParams.colorFormat = getNvColorFormatFromV4l2Format(format);
    cParams.width = m_codedWidth;
    cParams.height = m_codedHeight;
    cParams.layout = NvBufferLayout_BlockLinear;
#ifdef WITH_NVUTILS
    cParams.memType = NVBUF_MEM_SURFACE_ARRAY;
    cParams.memtag = NvBufSurfaceTag_VIDEO_DEC;

    ret = NvBufSurf::NvAllocate(&cParams, m_numberCaptureBuffers, m_dmaBufferFd);
    TEST_ERROR(ret < 0, "Failed to create buffers, err=" << ret, -1);
    for (int index = 0; index < m_numberCaptureBuffers; index++) {
        ret = NvBufSurfaceFromFd(m_dmaBufferFd[index], (void**)(&(m_dmaBufferSurface[index])));
        TEST_ERROR(ret < 0, "Failed to get surface for buffer, err=" << ret, -1);
    }
#else
    cParams.payloadType = NvBufferPayload_SurfArray;
    cParams.nvbuf_tag = NvBufferTag_VIDEO_DEC;

    for (int index = 0; index < m_numberCaptureBuffers; index++) {
        ret = NvBufferCreateEx(&m_dmaBufferFd[index], &cParams);
        TEST_ERROR(ret < 0, "Failed to create buffers, err=" << ret, -1);
    }
#endif

    /* Request buffers on decoder capture plane. Refer ioctl VIDIOC_REQBUFS */
    m_decoder->capture_plane.reqbufs(V4L2_MEMORY_DMABUF, m_numberCaptureBuffers);
    TEST_ERROR(ret < 0, "Error in decoder capture plane streamon, err=" << ret, -1);

    /* Decoder capture plane STREAMON. Refer ioctl VIDIOC_STREAMON */
    m_decoder->capture_plane.setStreamStatus(true);
    TEST_ERROR(ret < 0, "Error in decoder capture plane streamon, err=" << ret, -1);

    /* Enqueue all the empty decoder capture plane buffers. */
    for (uint32_t i = 0; i < m_decoder->capture_plane.getNumBuffers(); i++) {
        struct v4l2_buffer v4l2_buf;
        struct v4l2_plane planes[MAX_PLANES];

        memset(&v4l2_buf, 0, sizeof(v4l2_buf));
        memset(planes, 0, sizeof(planes));

        v4l2_buf.index = i;
        v4l2_buf.m.planes = planes;
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        v4l2_buf.memory = V4L2_MEMORY_DMABUF;
        v4l2_buf.m.planes[0].m.fd = m_dmaBufferFd[i];

        ret = m_decoder->capture_plane.qBuffer(v4l2_buf, NULL);
        TEST_ERROR(ret < 0, "Error Qing buffer at output plane, err=" << ret, -1);
    }
    return 0;
}

//---------------------------------------------------------------------------------------------------------------------
void nvmpictx::deinitDecoderOutputPlane()
{
    while (m_decoder->output_plane.getNumQueuedBuffers() > 0 && !m_decoder->isInError()) {
        struct v4l2_buffer v4l2_buf;
        struct v4l2_plane planes[MAX_PLANES];

        memset(&v4l2_buf, 0, sizeof(v4l2_buf));
        memset(planes, 0, sizeof(planes));

        v4l2_buf.m.planes = planes;
        int ret = m_decoder->output_plane.dqBuffer(v4l2_buf, NULL, NULL, -1);
        WARN_ERROR(ret < 0, "Failed to dequeue buffer" );
    }
}


//---------------------------------------------------------------------------------------------------------------------
void nvmpictx::deinitDecoderCapturePlane()
{
    m_decoder->capture_plane.setStreamStatus(false);
    m_decoder->capture_plane.deinitPlane();
    for (int index = 0; index < m_numberCaptureBuffers; index++) { //V4L2_MEMORY_DMABUF
        if (m_dmaBufferFd[index] != 0) {
            int ret = NvBufferDestroy(m_dmaBufferFd[index]);
            WARN_ERROR(ret < 0, "Failed to Destroy NvBuffer, err=" << ret);
        }
    }
}

//---------------------------------------------------------------------------------------------------------------------
int nvmpictx::updateFrameSizeParams()
{
#ifdef WITH_NVUTILS
    NvBufSurfacePlaneParams parm;
    NvBufSurfaceParams dst_dma_surface_params;
    dst_dma_surface_params = m_framePool.getDmaSurface(0)->surfaceList[0];
    parm = dst_dma_surface_params.planeParams;
#else
    NvBufferParams parm;
    int ret = NvBufferGetParams(m_framePool.getDmaFd(0), &parm);
    TEST_ERROR(ret < 0, "Failed to get dst dma buf params", ret);
#endif

    m_frameLinesize[0] = parm.width[0];
    m_frameLinesize[1] = parm.width[1];
    m_frameLinesize[2] = parm.width[2];
    m_frameSize[0]	   = parm.psize[0];
    m_frameSize[1]	   = parm.psize[1];
    m_frameSize[2] 	   = parm.psize[2];
    m_frameHeight[0]   = parm.height[0];
    m_frameHeight[1]   = parm.height[1];
    m_frameHeight[2]   = parm.height[2];
    return 0;
}

//---------------------------------------------------------------------------------------------------------------------
int nvmpictx::updateBufferTransformParams()
{
    m_srcRect = { 0, 0, m_codedWidth, m_codedHeight };
    m_destRect = { 0, 0, m_codedWidth, m_codedHeight };

    memset(&m_transformParams,0,sizeof(m_transformParams));
    m_transformParams.transform_flag = NVBUFFER_TRANSFORM_FILTER;
    m_transformParams.transform_flip = NvBufferTransform_None;
    m_transformParams.transform_filter = NvBufferTransform_Filter_Smart;
    //ctx->m_transformParams.transform_filter = NvBufSurfTransformInter_Nearest;
#ifdef WITH_NVUTILS
    m_transformParams.src_rect = &m_srcRect;
    m_transformParams.dst_rect = &m_destRect;
#else
    m_transformParams.src_rect = m_srcRect;
    m_transformParams.dst_rect = m_destRect;
    m_transformParams.session = m_session;
#endif
    return 0;
}

//---------------------------------------------------------------------------------------------------------------------
void nvmpictx::deinitFramePool()
{
    m_framePool.deinit();
}

//---------------------------------------------------------------------------------------------------------------------
int nvmpictx::initFramePool()
{
    NvBufferColorFormat cFmt = m_outputPixfmt==NV_PIX_NV12
                    ? NvBufferColorFormat_NV12
                    : NvBufferColorFormat_YUV420;
    return m_framePool.init(m_codedWidth, m_codedHeight, cFmt, MAX_BUFFERS);
}

//---------------------------------------------------------------------------------------------------------------------
int nvmpictx::respondToResolutionEvent(v4l2_format &format, v4l2_crop &crop)
{
    int ret=0;

    /* Get capture plane format from the decoder.
       This may change after resolution change event.
       Refer ioctl VIDIOC_G_FMT */
    ret = m_decoder->capture_plane.getFormat(format);
    TEST_ERROR(ret < 0, "Error: Could not get format from decoder capture plane", ret);

    /* Get the display resolution from the decoder.
       Refer ioctl VIDIOC_G_CROP */
    ret = m_decoder->capture_plane.getCrop(crop);
    TEST_ERROR(ret < 0, "Error: Could not get crop from decoder capture plane", ret);

    m_codedWidth=crop.c.width;
    m_codedHeight=crop.c.height;

    //init/reinit DecoderCapturePlane
    deinitDecoderCapturePlane();

    ret = initDecoderCapturePlane(format);
    TEST_ERROR( ret < 0, "Error: failed to init capture plane", ret);


    /* override default seesion. Without overriding m_session we wil
       get seg. fault if decoding in forked process*/
#ifdef WITH_NVUTILS
    m_session.compute_mode = NvBufSurfTransformCompute_VIC;
    m_session.gpu_id = 0;
    m_session.cuda_stream = 0;
    NvBufSurfTransformSetSessionParams(&m_session);
#else
    m_session = NvBufferSessionCreate();
#endif

    //alloc frame pool buffers (dst_dma buffers). TODO: check if already allocated and deinit pool first
    ret = initFramePool();
    TEST_ERROR( ret < 0, "Error: failed to init frame pool", ret);

    //get dst_dma buffer params and set corresponding frame size and linesize in nvmpictx
    ret = updateFrameSizeParams();
    TEST_ERROR( ret < 0, "Error: failed to update frame size params", ret);

    //reset buffer transformation params based on new resolution data
    ret = updateBufferTransformParams();
    TEST_ERROR( ret < 0, "Error: failed to update buffer transform params", ret);

    return 0;
}

//---------------------------------------------------------------------------------------------------------------------
void nvmpictx::dec_capture_loop_fcn(void *arg)
{
    LOG_DBG("Capture loop is running!");
    nvmpictx* ctx=(nvmpictx*)arg;
    ctx->captureLoop();
}

//---------------------------------------------------------------------------------------------------------------------
void nvmpictx::captureLoop()
{
    struct v4l2_format v4l2Format;
    struct v4l2_crop v4l2Crop;
    struct v4l2_event v4l2Event;
    int ret,bIndex=0;
    //std::thread transformWorkersPool[3];

    /* Need to wait for the first Resolution change event, so that
       the decoder knows the stream resolution and can allocate appropriate
       buffers when we call REQBUFS. */
    do {
        /* Refer ioctl VIDIOC_DQEVENT */
        ret = m_decoder->dqEvent(v4l2Event, 500);
        if (ret < 0) {
            if (errno == EAGAIN) {
                continue;
            } else {
               LOG_ERR("Error " << errno << " in dequeueing decoder event");
               m_terminated=true;
            }
        }
    } while ((v4l2Event.type != V4L2_EVENT_RESOLUTION_CHANGE) && !m_terminated);

    /* Received the resolution change event, now can do respondToResolutionEvent. */
    if (!m_terminated) {
        if ( respondToResolutionEvent(v4l2Format, v4l2Crop ) < 0) {
            m_terminated = true;
        }
    }

    while (!m_terminated && !m_decoder->isInError()) {
        NvBuffer *dec_buffer;

        // Check for Resolution change again.
        ret = m_decoder->dqEvent(v4l2Event, false);
        if (ret == 0) {
            switch (v4l2Event.type) {
            case V4L2_EVENT_RESOLUTION_CHANGE:
                if ( respondToResolutionEvent(v4l2Format, v4l2Crop) < 0 ) {
                    m_terminated = true;
                }
                continue;
            }
        }

        /* Decoder capture loop */
        while(!m_terminated) {
            struct v4l2_buffer v4l2_buf;
            struct v4l2_plane planes[MAX_PLANES];

            memset(&v4l2_buf, 0, sizeof(v4l2_buf));
            memset(planes, 0, sizeof(planes));
            v4l2_buf.m.planes = planes;

            /* Dequeue a filled buffer. */
            if (m_decoder->capture_plane.dqBuffer(v4l2_buf, &dec_buffer, NULL, 0)) {
                if (errno == EAGAIN) {
                    if (v4l2_buf.flags & V4L2_BUF_FLAG_LAST) {
                        LOG_ERR("Got EoS at capture plane");
                        m_terminated=true;
                    }

                    // This buffer didn't result in a frame - re-queue it and continue the loop
                    LOG_DBG("No frame at capture plane: buffer=" << (void*)dec_buffer << " bufferIndex=" << v4l2_buf.index);

                    usleep(100);
                } else {
                    LOG_ERR("Error " << errno << " while calling dequeue at capture plane");
                    m_terminated=true;
                }
                break;
            }

            int64_t pts = (v4l2_buf.timestamp.tv_usec % 1000000) + (v4l2_buf.timestamp.tv_sec * 1000000UL);

            dec_buffer->planes[0].fd = m_dmaBufferFd[v4l2_buf.index];

            bIndex = m_framePool.dqEmptyBuf();

            LOG_DBG("Received decoded frame at capture plane: buffer=" << (void*)dec_buffer << " bufferIndex=" << v4l2_buf.index << " pts=" << pts << " ts=" << v4l2_buf.timestamp.tv_sec << "." << v4l2_buf.timestamp.tv_usec << " index=" << bIndex );

            if(bIndex != -1) {
#ifdef WITH_NVUTILS
                ret = NvBufSurfTransform(m_dmaBufferSurface[v4l2_buf.index], m_framePool.getDmaSurface(bIndex), &m_transformParams);
#else
                ret = NvBufferTransform(dec_buffer->planes[0].fd, m_framePool.getDmaFd(bIndex), &m_transformParams);
#endif
                if (ret < 0 ) {
                    LOG_ERR( "Transform failed, err=" << ret );
                    m_terminated = true;
                    break;
                }
                m_framePool.setTimestamp(bIndex, pts);
                m_framePool.qFilledBuf(bIndex);
            } else {
                LOG_ERR( "No empty buffers available to transform, Frame skipped!" );
            }

            v4l2_buf.m.planes[0].m.fd = m_dmaBufferFd[v4l2_buf.index];
            if (m_decoder->capture_plane.qBuffer(v4l2_buf, NULL) < 0) {
                LOG_ERR("Error while queueing buffer at decoder capture plane");
                m_terminated = true;
            }
        }
    }

#ifndef WITH_NVUTILS
    NvBufferSessionDestroy(m_session);
#endif
    LOG_DBG("Decoder thread exited");
}


//---------------------------------------------------------------------------------------------------------------------
//TODO: accept in nvmpi_create_decoder stream params (width and height, etc...) from ffmpeg.
nvmpictx* nvmpi_create_decoder(nvCodingType codingType, nvPixFormat pixFormat)
{
    nvmpictx* ctx=new nvmpictx;
    if ( ctx->init(codingType, pixFormat) < 0 ) {
        ctx->close();
        delete ctx;
        ctx = nullptr;
    }
#ifdef WITH_NVUTILS
    LOG_DBG("Initialized NVMPI decoder, ver." << VER << "(nvutils)");
#else
    LOG_DBG("Initialized NVMPI decoder, ver." << VER );
#endif
    return ctx;
}

//---------------------------------------------------------------------------------------------------------------------
static int codingTypeToPixfmt(nvCodingType codingType)
{
    switch(codingType){
    case NV_VIDEO_CodingH264:   return V4L2_PIX_FMT_H264;
    case NV_VIDEO_CodingHEVC:   return V4L2_PIX_FMT_H265;
    case NV_VIDEO_CodingMPEG4:  return V4L2_PIX_FMT_MPEG4;
    case NV_VIDEO_CodingMPEG2:  return V4L2_PIX_FMT_MPEG2;
    case NV_VIDEO_CodingVP8:    return V4L2_PIX_FMT_VP8;
    case NV_VIDEO_CodingVP9:    return V4L2_PIX_FMT_VP9;
    default:                    return V4L2_PIX_FMT_H264;
    }
}


//---------------------------------------------------------------------------------------------------------------------
int nvmpictx::init(nvCodingType codingType, nvPixFormat pixFormat)
{
    int ret = -1;

    m_decoder = NvVideoDecoder::createVideoDecoder("dec0");
    TEST_ERROR(!m_decoder, "Could not create decoder",-1);

    ret = m_decoder->subscribeEvent(V4L2_EVENT_RESOLUTION_CHANGE, 0, 0);
    TEST_ERROR(ret < 0, "Could not subscribe to V4L2_EVENT_RESOLUTION_CHANGE", -1);

    m_decoderPixfmt = codingTypeToPixfmt(codingType);
    ret = m_decoder->setOutputPlaneFormat(m_decoderPixfmt, CHUNK_SIZE);
    TEST_ERROR(ret < 0, "Could not set output plane format", -1);

    ret = m_decoder->setFrameInputMode(0);
    TEST_ERROR(ret < 0, "Error in decoder setFrameInputMode for NALU", -1);

    //TODO: create option to enable max performace mode (?)
    //ret = m_decoder->setMaxPerfMode(true);
    //TEST_ERROR(ret < 0, "Error while setting decoder to max perf", ret);

    ret = m_decoder->output_plane.setupPlane(V4L2_MEMORY_USERPTR, 10, false, true);
    TEST_ERROR(ret < 0, "Error while setting up output plane", -1);

    ret = m_decoder->output_plane.setStreamStatus(true);
    TEST_ERROR(ret < 0, "Error in output plane stream on", -1);

    m_outputPixfmt=pixFormat;
    m_terminated=false;
    m_index=0;
    m_frameSize[0]=0;
    for(int index=0;index<MAX_BUFFERS;index++) {
        m_dmaBufferFd[index]=0;
    }
    m_numberCaptureBuffers=0;
    m_thread = std::thread(nvmpictx::dec_capture_loop_fcn, this);

#if SAVE_BITSTREAM
    const char* var = getenv("NVMPI_SAVE_BITSTREAM");
    if ( var ) {
        char name[2048];
        sprintf(name, "%s-%p", var, this);
        m_bitstreamFile = fopen( name, "w+b" );
        LOG_DBG("Saving bitstream to " << name);
    } else {
        LOG_DBG("Not saving bitstream" );
        m_bitstreamFile = NULL;
    }
#endif

    return 0;
}


//---------------------------------------------------------------------------------------------------------------------
int nvmpi_decoder_put_packet(nvmpictx* ctx,nvPacket* packet)
{
    return ctx->putPacket(packet);
}

//---------------------------------------------------------------------------------------------------------------------
int nvmpictx::putPacket(nvPacket* packet)
{
#if SAVE_BITSTREAM
    if ( m_bitstreamFile ) {
        fwrite( (char*)&nvmpimagic, 1, sizeof(long), m_bitstreamFile);
        fwrite( (char*)&m_packetsSubmitted, 1, sizeof(long), m_bitstreamFile);
        fwrite( (char*)&packet->payload_size, 1, sizeof(long), m_bitstreamFile);
        fwrite( (char*)&packet->pts, 1, sizeof(long), m_bitstreamFile);
        printf("%lu - Writing block of size=%lu\n", m_packetsSubmitted, packet->payload_size);
        fwrite( (char*)packet->payload, 1, packet->payload_size, m_bitstreamFile);
    }
#endif

    int ret;
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    NvBuffer *nvBuffer = nullptr;

    memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    memset(planes, 0, sizeof(planes));

    v4l2_buf.m.planes = planes;

    if (m_index < (int)m_decoder->output_plane.getNumBuffers()) {
        nvBuffer = m_decoder->output_plane.getNthBuffer(m_index);
        TEST_ERROR( nvBuffer == nullptr, "Failed to get Nth buffer", -1);
        v4l2_buf.index = m_index;
        m_index++;
    } else {
        ret = m_decoder->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
        TEST_ERROR (ret < 0, "Error DQing buffer at output plane", -1 );
    }

    memcpy(nvBuffer->planes[0].data,packet->payload,packet->payload_size);
    nvBuffer->planes[0].bytesused=packet->payload_size;
    v4l2_buf.m.planes[0].bytesused = nvBuffer->planes[0].bytesused;

    v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
    v4l2_buf.timestamp.tv_sec = packet->pts / 1000000;
    v4l2_buf.timestamp.tv_usec = packet->pts % 1000000;

    LOG_DBG("Submitting packet=" << m_packetsSubmitted << " timestamp=" << packet->pts << " buffer=" << (void*)nvBuffer << " bufferIndex=" << v4l2_buf.index << " ts=" << v4l2_buf.timestamp.tv_sec << "." << v4l2_buf.timestamp.tv_usec );

    ret = m_decoder->output_plane.qBuffer(v4l2_buf, NULL);
    TEST_ERROR (ret < 0, "Error Qing buffer at output plane", -1 );
    m_packetsSubmitted++;


    if (v4l2_buf.m.planes[0].bytesused == 0) {
        LOG_DBG("EOF in decoder!");
        m_terminated=true;
    }

    return 0;
}

//---------------------------------------------------------------------------------------------------------------------
int nvmpi_decoder_get_frame(nvmpictx* ctx,nvFrame* frame,bool wait)
{
    return ctx->getFrame(frame, wait);
}

//---------------------------------------------------------------------------------------------------------------------
int nvmpictx::getFrame(nvFrame* frame,bool wait)
{
    int ret = 0;
    int bIndex = m_framePool.dqFilledBuf();
    if (bIndex<0) {
        return -1;
    }
#ifdef WITH_NVUTILS
    NvBufSurface *dSurf = m_framePool.getDmaSurface(bIndex);
    ret=NvBufSurface2Raw(dSurf,0,0,m_frameLinesize[0],m_frameHeight[0],frame->payload[0]);
    ret=NvBufSurface2Raw(dSurf,0,1,m_frameLinesize[1],m_frameHeight[1],frame->payload[1]);
    if (m_outputPixfmt==NV_PIX_YUV420) {
        ret=NvBufSurface2Raw(dSurf,0,2,m_frameLinesize[2],m_frameHeight[2],frame->payload[2]);
    }
#else
    int dFd = m_framePool.getDmaFd(bIndex);
    ret=NvBuffer2Raw(dFd,0,m_frameLinesize[0],m_frameHeight[0],frame->payload[0]);
    ret=NvBuffer2Raw(dFd,1,m_frameLinesize[1],m_frameHeight[1],frame->payload[1]);
    if (m_outputPixfmt==NV_PIX_YUV420) {
        ret=NvBuffer2Raw(dFd,2,m_frameLinesize[2],m_frameHeight[2],frame->payload[2]);
    }
#endif

    frame->timestamp = m_framePool.getTimestamp(bIndex);
    int64_t diff = frame->timestamp - m_prevFrameTs;

    LOG_DBG("Received frame=" << m_framesRead << " index=" << bIndex << " pts=" << frame->timestamp << " diff=" << diff << " flag=" << (diff>=0?"0":"1"));

    m_prevFrameTs = frame->timestamp;

    //return buffer to pool
    m_framePool.qEmptyBuf(bIndex);
    m_framesRead++;
    return ret;
}


//---------------------------------------------------------------------------------------------------------------------
void nvmpictx::close()
{
    LOG_DBG("Terminating the decoder");
    m_terminated=true;
    m_decoder->output_plane.setStreamStatus(false);
    m_decoder->capture_plane.setStreamStatus(false);
    if (m_thread.joinable()) {
        m_thread.join();
    } else {
        LOG_ERR("Thread is not joinable");
    }

    LOG_DBG("Closing decoder, packets=" << m_packetsSubmitted << " frames=" << m_framesRead);
    //deinit DstDmaBuffer and DecoderCapturePlane
    deinitDecoderCapturePlane();
    deinitDecoderOutputPlane();
    //empty frame queue and free buffers
    deinitFramePool();

    delete m_decoder;
    m_decoder = nullptr;

#if SAVE_BITSTREAM
    char name[2048];
    if ( m_bitstreamFile ) {
        fclose(m_bitstreamFile);
    }
    m_bitstreamFile = NULL;
#endif

}


//---------------------------------------------------------------------------------------------------------------------
int nvmpi_decoder_close(nvmpictx* ctx)
{
    ctx->close();
    delete ctx;
    return 0;
}


