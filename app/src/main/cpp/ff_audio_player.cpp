//
// Created by xu fulong on 2022/9/4.
//

#include "ff_audio_player.h"

#define AUDIO_TAG "AudioPlayer"
#define BUFFER_SIZE (48000 * 10)

const char *FILTER_DESC = "superequalizer=6b=4:8b=5:10b=5";

int initFilter(const char *filterDesc, AVCodecContext *codecCtx, AVFilterGraph **graph,
                          AVFilterContext **src, AVFilterContext **sink) {
    int ret;
    char args[512];
    AVFilterContext *buffersrc_ctx;
    AVFilterContext *buffersink_ctx;
    AVRational time_base       = codecCtx->time_base;
    AVFilterInOut *inputs      = avfilter_inout_alloc();
    AVFilterInOut *outputs     = avfilter_inout_alloc();
    const AVFilter *buffersrc  = avfilter_get_by_name("abuffer");
    const AVFilter *buffersink = avfilter_get_by_name("abuffersink");

    AVFilterGraph *filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer audio source: the decoded frames from the decoder will be inserted here. */
    if (!codecCtx->channel_layout)
        codecCtx->channel_layout = static_cast<uint64_t>(av_get_default_channel_layout(
                codecCtx->channels));
    snprintf(args, sizeof(args),
             "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64 "",
             time_base.num, time_base.den, codecCtx->sample_rate,
             av_get_sample_fmt_name(codecCtx->sample_fmt), codecCtx->channel_layout);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, nullptr, filter_graph);
    if (ret < 0) {
        LOGE(AUDIO_TAG, "Cannot create buffer source:%d", ret);
        goto end;
    }
    /* buffer audio sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       nullptr, nullptr, filter_graph);
    if (ret < 0) {
        LOGE(AUDIO_TAG, "Cannot create buffer sink:%d", ret);
        goto end;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = nullptr;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = buffersink_ctx;
    inputs->pad_idx     = 0;
    inputs->next        = nullptr;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filterDesc,
                                        &inputs, &outputs, nullptr)) < 0) {
        LOGE(AUDIO_TAG, "avfilter_graph_parse_ptr error:%d", ret);
        goto end;
    }
    if ((ret = avfilter_graph_config(filter_graph, nullptr)) < 0) {
        LOGE(AUDIO_TAG, "avfilter_graph_config error:%d", ret);
        goto end;
    }
    *graph = filter_graph;
    *src   = buffersrc_ctx;
    *sink  = buffersink_ctx;
end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

int FFAudioPlayer::open(const char *path) {
    if (!path)
        return -1;

    int ret;
    const AVCodec *codec;
    inputFrame = av_frame_alloc();
    packet     = av_packet_alloc();
    out_buffer = new uint8_t [BUFFER_SIZE];

    // 打开输入流
    ret = avformat_open_input(&formatContext, path, nullptr, nullptr);
    if (ret < 0) {
        LOGE(AUDIO_TAG, "avformat_open_input error=%s", av_err2str(ret));
        return ret;
    }
    // 查找stream信息
    avformat_find_stream_info(formatContext, nullptr);
    // 找到音频index
    for (int i=0; i<formatContext->nb_streams; i++) {
        if (AVMEDIA_TYPE_AUDIO == formatContext->streams[i]->codecpar->codec_type) {
            audio_index = i;
            break;
        }
    }
    if (audio_index == -1) {
        return -1;
    }
    // 找到codec
    codec = avcodec_find_decoder(formatContext->streams[audio_index]->codecpar->codec_id);
    codecContext = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecContext, formatContext->streams[audio_index]->codecpar);
    // 打开解码器
    ret = avcodec_open2(codecContext, codec, nullptr);
    if (ret < 0) {
        LOGE(AUDIO_TAG, "avcodec_open2 error=%s", av_err2str(ret));
        return ret;
    }
    // 输入：采样率、声道布局、音频格式
    int in_sample_rate = codecContext->sample_rate;
    auto in_sample_fmt = codecContext->sample_fmt;
    int in_ch_layout   = (int)codecContext->channel_layout;
    out_sample_rate    = in_sample_rate; // 输出采样率等于输入采样率
    out_sample_fmt     = AV_SAMPLE_FMT_S16; // 16位
    out_ch_layout      = AV_CH_LAYOUT_STEREO; // 双声道
    out_channel        = codecContext->channels;
    // 初始化音频格式转换上下文swrContext
    swrContext = swr_alloc();
    swr_alloc_set_opts(swrContext, out_ch_layout, out_sample_fmt, out_sample_rate,
                       in_ch_layout, in_sample_fmt, in_sample_rate, 0, nullptr);
    swr_init(swrContext);

    filterFrame = av_frame_alloc();
    initFilter(FILTER_DESC, codecContext, &audioFilterGraph,
                   &audioSrcContext, &audioSinkContext);

    return 0;
}

int FFAudioPlayer::getChannel() const {
    return out_channel;
}

int FFAudioPlayer::getSampleRate() const {
    return out_sample_rate;
}

int FFAudioPlayer::decodeAudio() {
    int ret;
    // 读取音频数据(解封装)
    ret = av_read_frame(formatContext, packet);
    if (ret < 0) {
        return ret;
    }
    // 判断是否位音频帧
    if (packet->stream_index != audio_index) {
        return 0;
    }
    // 解码音频帧
    ret = avcodec_send_packet(codecContext, packet);
    if (ret < 0) {
        LOGE(AUDIO_TAG, "avcodec_send_packet=%s", av_err2str(ret));
    }
    ret = avcodec_receive_frame(codecContext, inputFrame);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            return 0;
        } else {
            return ret;
        }
    }
    // put into filter
    ret = av_buffersrc_add_frame(audioSrcContext, inputFrame);
    if (ret < 0) {
        LOGE(AUDIO_TAG, "av_buffersrc_add_frame error=%s", av_err2str(ret));
    }
    // drain from filter
    ret = av_buffersink_get_frame(audioSinkContext, filterFrame);
    if (ret == AVERROR(EAGAIN)) {
        return 0;
    } else if (ret == AVERROR_EOF) {
        LOGE(AUDIO_TAG, "enf of stream...");
        return ret;
    } else if (ret < 0) {
        LOGE(AUDIO_TAG, "av_buffersink_get_frame error:%s", av_err2str(ret));
        return ret;
    }

    // 音频格式转换
    swr_convert(swrContext, &out_buffer, BUFFER_SIZE,
            (const uint8_t **)(filterFrame->data), filterFrame->nb_samples);
    // 获取转换后缓冲区大小
    int buffer_size = av_samples_get_buffer_size(nullptr, out_channel,
                                                 filterFrame->nb_samples, out_sample_fmt, 1);
    LOGI(AUDIO_TAG, "buffer_size=%d", buffer_size);
    av_frame_unref(inputFrame);
    av_frame_unref(filterFrame);
    av_packet_unref(packet);
    return buffer_size;
}


uint8_t *FFAudioPlayer::getDecodeFrame() const {
    return out_buffer;
}

void FFAudioPlayer::close() {
    if (formatContext) {
        avformat_close_input(&formatContext);
    }
    if (codecContext) {
        avcodec_free_context(&codecContext);
    }
    if (packet) {
        av_packet_free(&packet);
    }
    if (inputFrame) {
        av_frame_free(&inputFrame);
    }
    if (swrContext) {
        swr_close(swrContext);
    }
    avfilter_free(audioSrcContext);
    avfilter_free(audioSinkContext);
    if (audioFilterGraph) {
        avfilter_graph_free(&audioFilterGraph);
    }
    delete[] out_buffer;
}