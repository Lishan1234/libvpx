/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "./vpx_config.h"
#include "./vpx_version.h"
#include "./tools_common.h"

#include "vpx/internal/vpx_codec_internal.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "vpx_dsp/bitreader_buffer.h"
#include "vpx_dsp/vpx_dsp_common.h"
#include "vpx_util/vpx_thread.h"

#include "vp9/common/vp9_alloccommon.h"
#include "vp9/common/vp9_frame_buffers.h"

#include "vp9/decoder/vp9_decodeframe.h"

#include "vp9/vp9_dx_iface.h"
#include "vp9/vp9_iface_common.h"

#include <sys/param.h>
#include <vpx_util/vpx_write_yuv_frame.h>
#include <vpx_dsp/psnr.h>
#include <vpx_dsp/ssim.h>
#include <vpx_dsp_rtcd.h>
#include <android/log.h>

#include "../../main.hpp"

#define LOG_MAX 1000
#define TAG "vp9_dx_iface.c JNI"
#define _UNKNOWN   0
#define _DEFAULT   1
#define _VERBOSE   2
#define _DEBUG    3
#define _INFO        4
#define _WARN        5
#define _ERROR    6
#define _FATAL    7
#define _SILENT       8
#define LOGUNK(...) __android_log_print(_UNKNOWN,TAG,__VA_ARGS__)
#define LOGDEF(...) __android_log_print(_DEFAULT,TAG,__VA_ARGS__)
#define LOGV(...) __android_log_print(_VERBOSE,TAG,__VA_ARGS__)
#define LOGD(...) __android_log_print(_DEBUG,TAG,__VA_ARGS__)
#define LOGI(...) __android_log_print(_INFO,TAG,__VA_ARGS__)
#define LOGW(...) __android_log_print(_WARN,TAG,__VA_ARGS__)
#define LOGE(...) __android_log_print(_ERROR,TAG,__VA_ARGS__)
#define LOGF(...) __android_log_print(_FATAL,TAG,__VA_ARGS__)
#define LOGS(...) __android_log_print(_SILENT,TAG,__VA_ARGS__)

#define VP9_CAP_POSTPROC (CONFIG_VP9_POSTPROC ? VPX_CODEC_CAP_POSTPROC : 0)

//hyunho
#define DEBUG_LATENCY 1
#define DEBUG_LR  0
#define FRACTION_BIT (5)
#define FRACTION_SCALE (1 << FRACTION_BIT)
#define BILLION  1E9
#define LOG_MAX 1000


static vpx_codec_err_t decoder_init(vpx_codec_ctx_t *ctx,
                                    vpx_codec_priv_enc_mr_cfg_t *data) {
    // This function only allocates space for the vpx_codec_alg_priv_t
    // structure. More memory may be required at the time the stream
    // information becomes known.
    (void) data;

    if (!ctx->priv) {
        vpx_codec_alg_priv_t *const priv =
                (vpx_codec_alg_priv_t *) vpx_calloc(1, sizeof(*priv));
        if (priv == NULL) return VPX_CODEC_MEM_ERROR;

        ctx->priv = (vpx_codec_priv_t *) priv;
        ctx->priv->init_flags = ctx->init_flags;
        priv->si.sz = sizeof(priv->si);
        priv->flushed = 0;
        if (ctx->config.dec) {
            priv->cfg = *ctx->config.dec;
            ctx->config.dec = &priv->cfg;
        }
    }

    return VPX_CODEC_OK;
}

void free_snpe(vpx_codec_alg_priv_t *ctx){
    //snpe api
    snpe_free(ctx);

    //Free object inside vpx_codec
    free(ctx->snpe_object);
}


static vpx_codec_err_t decoder_destroy(vpx_codec_alg_priv_t *ctx) {

    /*** Chanju ***/
    free_snpe(ctx);
    /*** Chanju ***/

    if (ctx->pbi != NULL) {
        vp9_decoder_remove(ctx->pbi);
    }

    if (ctx->buffer_pool) {
        vp9_free_ref_frame_buffers(ctx->buffer_pool);
        vp9_free_internal_frame_buffers(&ctx->buffer_pool->int_frame_buffers);
    }

    vpx_free(ctx->buffer_pool);
    vpx_free(ctx);

    return VPX_CODEC_OK;
}

static int parse_bitdepth_colorspace_sampling(BITSTREAM_PROFILE profile,
                                              struct vpx_read_bit_buffer *rb) {
    vpx_color_space_t color_space;
    if (profile >= PROFILE_2) rb->bit_offset += 1;  // Bit-depth 10 or 12.
    color_space = (vpx_color_space_t) vpx_rb_read_literal(rb, 3);
    if (color_space != VPX_CS_SRGB) {
        rb->bit_offset += 1;  // [16,235] (including xvycc) vs [0,255] range.
        if (profile == PROFILE_1 || profile == PROFILE_3) {
            rb->bit_offset += 2;  // subsampling x/y.
            rb->bit_offset += 1;  // unused.
        }
    } else {
        if (profile == PROFILE_1 || profile == PROFILE_3) {
            rb->bit_offset += 1;  // unused
        } else {
            // RGB is only available in version 1.
            return 0;
        }
    }
    return 1;
}

static vpx_codec_err_t decoder_peek_si_internal(
        const uint8_t *data, unsigned int data_sz, vpx_codec_stream_info_t *si,
        int *is_intra_only, vpx_decrypt_cb decrypt_cb, void *decrypt_state) {
    int intra_only_flag = 0;
    uint8_t clear_buffer[10];

    if (data + data_sz <= data) return VPX_CODEC_INVALID_PARAM;

    si->is_kf = 0;
    si->w = si->h = 0;

    if (decrypt_cb) {
        data_sz = VPXMIN(sizeof(clear_buffer), data_sz);
        decrypt_cb(decrypt_state, data, clear_buffer, data_sz);
        data = clear_buffer;
    }

    // A maximum of 6 bits are needed to read the frame marker, profile and
    // show_existing_frame.
    if (data_sz < 1) return VPX_CODEC_UNSUP_BITSTREAM;

    {
        int show_frame;
        int error_resilient;
        struct vpx_read_bit_buffer rb = {data, data + data_sz, 0, NULL, NULL};
        const int frame_marker = vpx_rb_read_literal(&rb, 2);
        const BITSTREAM_PROFILE profile = vp9_read_profile(&rb);

        if (frame_marker != VP9_FRAME_MARKER) return VPX_CODEC_UNSUP_BITSTREAM;

        if (profile >= MAX_PROFILES) return VPX_CODEC_UNSUP_BITSTREAM;

        if (vpx_rb_read_bit(&rb)) {  // show an existing frame
            // If profile is > 2 and show_existing_frame is true, then at least 1 more
            // byte (6+3=9 bits) is needed.
            if (profile > 2 && data_sz < 2) return VPX_CODEC_UNSUP_BITSTREAM;
            vpx_rb_read_literal(&rb, 3);  // Frame buffer to show.
            return VPX_CODEC_OK;
        }

        // For the rest of the function, a maximum of 9 more bytes are needed
        // (computed by taking the maximum possible bits needed in each case). Note
        // that this has to be updated if we read any more bits in this function.
        if (data_sz < 10) return VPX_CODEC_UNSUP_BITSTREAM;

        si->is_kf = !vpx_rb_read_bit(&rb);
        show_frame = vpx_rb_read_bit(&rb);
        error_resilient = vpx_rb_read_bit(&rb);

        if (si->is_kf) {
            if (!vp9_read_sync_code(&rb)) return VPX_CODEC_UNSUP_BITSTREAM;

            if (!parse_bitdepth_colorspace_sampling(profile, &rb))
                return VPX_CODEC_UNSUP_BITSTREAM;
            vp9_read_frame_size(&rb, (int *) &si->w, (int *) &si->h);
        } else {
            intra_only_flag = show_frame ? 0 : vpx_rb_read_bit(&rb);

            rb.bit_offset += error_resilient ? 0 : 2;  // reset_frame_context

            if (intra_only_flag) {
                if (!vp9_read_sync_code(&rb)) return VPX_CODEC_UNSUP_BITSTREAM;
                if (profile > PROFILE_0) {
                    if (!parse_bitdepth_colorspace_sampling(profile, &rb))
                        return VPX_CODEC_UNSUP_BITSTREAM;
                }
                rb.bit_offset += REF_FRAMES;  // refresh_frame_flags
                vp9_read_frame_size(&rb, (int *) &si->w, (int *) &si->h);
            }
        }
    }
    if (is_intra_only != NULL) *is_intra_only = intra_only_flag;
    return VPX_CODEC_OK;
}

static vpx_codec_err_t decoder_peek_si(const uint8_t *data,
                                       unsigned int data_sz,
                                       vpx_codec_stream_info_t *si) {
    return decoder_peek_si_internal(data, data_sz, si, NULL, NULL, NULL);
}

static vpx_codec_err_t decoder_get_si(vpx_codec_alg_priv_t *ctx,
                                      vpx_codec_stream_info_t *si) {
    const size_t sz = (si->sz >= sizeof(vp9_stream_info_t))
                      ? sizeof(vp9_stream_info_t)
                      : sizeof(vpx_codec_stream_info_t);
    memcpy(si, &ctx->si, sz);
    si->sz = (unsigned int) sz;

    return VPX_CODEC_OK;
}

static void set_error_detail(vpx_codec_alg_priv_t *ctx,
                             const char *const error) {
    ctx->base.err_detail = error;
}

static vpx_codec_err_t update_error_state(
        vpx_codec_alg_priv_t *ctx, const struct vpx_internal_error_info *error) {
    if (error->error_code)
        set_error_detail(ctx, error->has_detail ? error->detail : NULL);

    return error->error_code;
}

static void init_buffer_callbacks(vpx_codec_alg_priv_t *ctx) {

    VP9_COMMON *const cm = &(ctx->pbi->common);
    BufferPool *const pool = cm->buffer_pool;

    cm->new_fb_idx = INVALID_IDX;
    cm->byte_alignment = ctx->byte_alignment;
    cm->skip_loop_filter = ctx->skip_loop_filter;


    if (ctx->get_ext_fb_cb != NULL && ctx->release_ext_fb_cb != NULL) {
        pool->get_fb_cb = ctx->get_ext_fb_cb;
        pool->release_fb_cb = ctx->release_ext_fb_cb;
        pool->cb_priv = ctx->ext_priv;
    } else {
        pool->get_fb_cb = vp9_get_frame_buffer;
        pool->release_fb_cb = vp9_release_frame_buffer;

        if (vp9_alloc_internal_frame_buffers(&pool->int_frame_buffers))
            vpx_internal_error(&cm->error, VPX_CODEC_MEM_ERROR,
                               "Failed to initialize internal frame buffers");

        pool->cb_priv = &pool->int_frame_buffers;
    }
}

static void set_default_ppflags(vp8_postproc_cfg_t *cfg) {
    cfg->post_proc_flag = VP8_DEBLOCK | VP8_DEMACROBLOCK;
    cfg->deblocking_level = 4;
    cfg->noise_level = 0;
}

static void set_ppflags(const vpx_codec_alg_priv_t *ctx, vp9_ppflags_t *flags) {
    flags->post_proc_flag = ctx->postproc_cfg.post_proc_flag;

    flags->deblocking_level = ctx->postproc_cfg.deblocking_level;
    flags->noise_level = ctx->postproc_cfg.noise_level;
}

/***Chanju***/
static void init_snpe( vpx_codec_alg_priv_t *ctx, mobinas_cfg_t * mobinas_cfg){
    snpe_cfg_t * snpe_object = malloc(sizeof(snpe_cfg_t));
    snpe_object->runtime = snpe_check_runtime();
    snpe_object->snpe_network = snpe_init_network(snpe_object->runtime, mobinas_cfg->model_quality);

    ctx->snpe_object = snpe_object;
}

/* Validate MobiNAS configuration */
static int is_valid_mobinas_cfg(const mobinas_cfg_t *mobinas_cfg) {
    if (mobinas_cfg == NULL) {
        fprintf(stderr, "%s: mobinas_cfg is NULL\n", __func__);
        return -1;
    }

    switch (mobinas_cfg->decode_mode)
    {
    case DECODE_BILINEAR:
        if (!mobinas_cfg->get_scale)
        {
            fprintf(stderr, "%s: get_scale is NULL\n", __func__);
            return -1;
        }
        if (!mobinas_cfg->bilinear_profile)
        {
            fprintf(stderr, "%s: bilinear_profile is NULL\n", __func__);
            return -1;
        }
        break;
    case DECODE_SR:
        switch (mobinas_cfg->dnn_mode)
        {
        case OFFLINE_DNN:
            //TODO: check a dnn file is valid
            break;
        case ONLINE_DNN:
            //TODO: check a dnn is valid
            break;
        }
        if (!mobinas_cfg->get_scale)
        {
            fprintf(stderr, "%s: get_scale is NULL\n", __func__);
            return -1;
        }
        break;
    case DECODE_CACHE:
        if (!mobinas_cfg->get_scale)
        {
            fprintf(stderr, "%s: get_scale is NULL\n", __func__);
            return -1;
        }
        if (!mobinas_cfg->bilinear_profile)
        {
            fprintf(stderr, "%s: bilinear_profile is NULL\n", __func__);
            return -1;
        }
        switch (mobinas_cfg->cache_policy)
        {
        case PROFILE_CACHE:
            if (!mobinas_cfg->cache_profile)
            {
                fprintf(stderr, "%s: cache_profile is NULL\n", __func__);
                return -1;
            }
            break;
        }
        switch (mobinas_cfg->dnn_mode)
        {
        case NO_DNN:
            fprintf(stderr, "%s: invalid dnn mode\n", __func__);
            return -1;
        case OFFLINE_DNN:
            //TODO: check a dnn file is valid
            break;
        case ONLINE_DNN:
            //TODO: check a dnn is valid
            break;
        }
        break;
    }

    if (mobinas_cfg->save_quality_result)
    {
        //TODO: check a compare file is valid
    }

    return 0;
}

//check whether mobinas_cfg is valid and runtime configuration
static vpx_codec_err_t load_mobinas_cfg(vpx_codec_alg_priv_t *ctx,  mobinas_cfg_t *mobinas_cfg) {
    assert(mobinas_cfg != NULL);

    if (is_valid_mobinas_cfg(mobinas_cfg)) {
        fprintf(stderr, "%s: invalid mobinas cfg config\n", __func__);
        return VPX_MOBINAS_ERROR;
    }

    ctx->mobinas_cfg = mobinas_cfg;

    if (ctx->mobinas_cfg->dnn_mode == ONLINE_DNN)
    {
        //TODO (chanju): check runtime availability
        //TODO (chanju): handle failure
        /***Chanju***/
        init_snpe(ctx, mobinas_cfg);
    }

    return VPX_CODEC_OK;
}


static vpx_codec_err_t init_decoder(vpx_codec_alg_priv_t *ctx) {
    char file_path[PATH_MAX] = {0};

    ctx->last_show_frame = -1;
    ctx->need_resync = 1;
    ctx->flushed = 0;

    ctx->buffer_pool = (BufferPool *) vpx_calloc(1, sizeof(BufferPool));
    if (ctx->buffer_pool == NULL) return VPX_CODEC_MEM_ERROR;

    ctx->pbi = vp9_decoder_create(ctx->buffer_pool);
    if (ctx->pbi == NULL) {
//        LOGE("decoder not allocated");
        set_error_detail(ctx, "Failed to allocate decoder");
        return VPX_CODEC_MEM_ERROR;
    }
    ctx->pbi->max_threads = ctx->cfg.threads;
    ctx->pbi->inv_tile_order = ctx->invert_tile_order;
//    LOGE("decoder allocated");



    // If postprocessing was enabled by the application and a
    // configuration has not been provided, default it.
    if (!ctx->postproc_cfg_set && (ctx->base.init_flags & VPX_CODEC_USE_POSTPROC))
        set_default_ppflags(&ctx->postproc_cfg);

    init_buffer_callbacks(ctx);


    /*******************Hyunho************************/
    if (ctx->mobinas_cfg == NULL) {
        set_error_detail(ctx, "mobinas_cfg is not allocated");
        return VPX_MOBINAS_ERROR;
    }

    const int num_threads = (ctx->pbi->max_threads > 1) ? ctx->pbi->max_threads : 1;
    if ((ctx->pbi->mobinas_worker_data = init_mobinas_worker(num_threads, ctx->mobinas_cfg)) == NULL) {
        set_error_detail(ctx, "Failed to allocate mobinas_worker_data");
        return VPX_MOBINAS_ERROR;
    }

    ctx->pbi->common.mobinas_cfg = ctx->mobinas_cfg;
    ctx->pbi->common.buffer_pool->mode = ctx->mobinas_cfg->decode_mode;

    if (ctx->mobinas_cfg->decode_mode == DECODE_BILINEAR)
        ctx->pbi->common.hr_bilinear_frame = (YV12_BUFFER_CONFIG *) vpx_calloc(1, sizeof(YV12_BUFFER_CONFIG));

    if (ctx->mobinas_cfg->save_quality_result) {
        switch(ctx->mobinas_cfg->decode_mode) {
        case DECODE:
            sprintf(file_path, "%s/%s/log/quality.log", ctx->mobinas_cfg->save_dir, ctx->mobinas_cfg->prefix);
            break;
        case DECODE_SR:
            sprintf(file_path, "%s/%s/log/quality_sr.log", ctx->mobinas_cfg->save_dir, ctx->mobinas_cfg->prefix);
            break;
        case DECODE_BILINEAR:
            sprintf(file_path, "%s/%s/log/quality_bilinear.log", ctx->mobinas_cfg->save_dir, ctx->mobinas_cfg->prefix);
            break;
        case DECODE_CACHE:
            switch(ctx->mobinas_cfg->cache_policy) {
            case KEY_FRAME_CACHE:
                sprintf(file_path, "%s/%s/log/quality_cache_key_frame.log", ctx->mobinas_cfg->save_dir, ctx->mobinas_cfg->prefix);
                break;
            case PROFILE_CACHE:
                sprintf(file_path, "%s/%s/log/quality_cache_%s.log", ctx->mobinas_cfg->save_dir, ctx->mobinas_cfg->prefix, ctx->mobinas_cfg->cache_profile->name);
                break;
            }

            switch(ctx->mobinas_cfg->dnn_mode){
                case NO_DNN:
                    sprintf(file_path, "%s/%s/log/quality_cache_no_dnn", ctx->mobinas_cfg->save_dir, ctx->mobinas_cfg->prefix);
                    break;
            }

            break;
        }

        if ((ctx->pbi->common.quality_log = fopen(file_path, "w")) == NULL) {
            fprintf(stderr, "%s: cannot open a file %s", __func__, file_path);
            ctx->mobinas_cfg->save_quality_result = 0;
        };

        ctx->pbi->common.hr_reference_frame = (YV12_BUFFER_CONFIG *) vpx_calloc(1, sizeof(YV12_BUFFER_CONFIG)); //adaptive cache o
        ctx->pbi->common.lr_reference_frame = (YV12_BUFFER_CONFIG *) vpx_calloc(1, sizeof(YV12_BUFFER_CONFIG)); //vp9_dx_iface x
    }
    /*******************Hyunho************************/


    /***Chanju***/
    ctx->pbi->common.snpe_object = ctx->snpe_object;
    ctx->pbi->common.test = 0;

    return VPX_CODEC_OK;
}

static INLINE void check_resync(vpx_codec_alg_priv_t *const ctx,
                                const VP9Decoder *const pbi) {
    // Clear resync flag if the decoder got a key frame or intra only frame.
    if (ctx->need_resync == 1 && pbi->need_resync == 0 &&
        (pbi->common.intra_only || pbi->common.frame_type == KEY_FRAME))
        ctx->need_resync = 0;
}

static vpx_codec_err_t decode_one(vpx_codec_alg_priv_t *ctx,
                                  const uint8_t **data, unsigned int data_sz,
                                  void *user_priv, int64_t deadline) {
    (void) deadline;

    // Determine the stream parameters. Note that we rely on peek_si to
    // validate that we have a buffer that does not wrap around the top
    // of the heap.
    if (!ctx->si.h) {
        int is_intra_only = 0;
        const vpx_codec_err_t res =
                decoder_peek_si_internal(*data, data_sz, &ctx->si, &is_intra_only,
                                         ctx->decrypt_cb, ctx->decrypt_state);
        if (res != VPX_CODEC_OK) return res;

        if (!ctx->si.is_kf && !is_intra_only) return VPX_CODEC_ERROR;
    }

    ctx->user_priv = user_priv;

    // Set these even if already initialized.  The caller may have changed the
    // decrypt config between frames.
    ctx->pbi->decrypt_cb = ctx->decrypt_cb;
    ctx->pbi->decrypt_state = ctx->decrypt_state;

    if (vp9_receive_compressed_data(ctx->pbi, data_sz, data)) {
        ctx->pbi->cur_buf->buf.corrupted = 1;
        ctx->pbi->need_resync = 1;
        ctx->need_resync = 1;
        return update_error_state(ctx, &ctx->pbi->common.error);
    }

    check_resync(ctx, ctx->pbi);

    return VPX_CODEC_OK;
}

static void save_serialized_intermediate_frame(VP9_COMMON *cm, int current_video_frame, int current_super_frame)
{
    char file_path[PATH_MAX] = {0};

    switch (cm->mobinas_cfg->decode_mode) {
    case DECODE:
        sprintf(file_path, "%s/%s/serialize/%d_%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, current_super_frame, cm->height);
        vpx_serialize_save(file_path, get_frame_new_buffer(cm));
        break;
    case DECODE_SR:
        fprintf(stderr, "%s: Not implemented", __func__);
        break;
    case DECODE_BILINEAR:
        fprintf(stderr, "%s: Not implemented", __func__);
        break;
    case DECODE_CACHE:
        sprintf(file_path, "%s/%s/serialize/%d_%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, current_super_frame, cm->height);
        vpx_serialize_save(file_path, get_sr_frame_new_buffer(cm));
#if DEBUG_LR
        sprintf(file_path, "%s/%s/serialize/%d_%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, current_super_frame, cm->height);
        vpx_serialize_save(file_path, get_frame_new_buffer(cm));
#endif
        break;
    }
}

static void save_serialized_final_frame(VP9_COMMON *cm, int current_video_frame)
{
    char file_path[PATH_MAX] = {0};

    switch (cm->mobinas_cfg->decode_mode) {
    case DECODE:
        sprintf(file_path, "%s/%s/serialize/%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, cm->height);
        vpx_serialize_save(file_path, get_frame_new_buffer(cm));
        break;
    case DECODE_SR:
        fprintf(stderr, "%s: Not implemented", __func__);
        break;
    case DECODE_BILINEAR:
        fprintf(stderr, "%s: Not implemented", __func__);
        break;
    case DECODE_CACHE:
        sprintf(file_path, "%s/%s/serialize/%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, cm->height);
        vpx_serialize_save(file_path, get_sr_frame_new_buffer(cm));
#if DEBUG_LR
        sprintf(file_path, "%s/%s/serialize/%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, cm->height);
        vpx_serialize_save(file_path, get_frame_new_buffer(cm));
#endif
        break;
    }
}

static void save_decoded_intermediate_frame(VP9_COMMON *cm, int current_video_frame,
                                            int current_super_frame)
{
    char file_path[PATH_MAX] = {0};

    switch (cm->mobinas_cfg->decode_mode) {
    case DECODE:
        sprintf(file_path, "%s/%s/frame/%d_%d_%dp.y", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, current_super_frame,
                cm->height);
        vpx_write_y_frame(file_path, get_frame_new_buffer(cm));
        break;
    case DECODE_SR:
        fprintf(stderr, "%s: Not implemented", __func__);
        break;
    case DECODE_BILINEAR:
        fprintf(stderr, "%s: Not implemented", __func__);
        break;
    case DECODE_CACHE:
        sprintf(file_path, "%s/%s/frame/%d_%d_%dp.y", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, current_super_frame,
                cm->height);
        vpx_write_y_frame(file_path, get_sr_frame_new_buffer(cm));
#if DEBUG_LR
            sprintf(file_path, "%s/%s/frame/%d_%d_%dp.y", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, current_super_frame, cm->height);
            vpx_write_y_frame(file_path, get_frame_new_buffer(cm));
#endif
        break;
    }
}

static void save_decoded_final_frame(VP9_COMMON *cm, int current_video_frame)
{
    char file_path[PATH_MAX] = {0};

    switch (cm->mobinas_cfg->decode_mode) {
        case DECODE:
        sprintf(file_path, "%s/%s/frame/%d_%dp.y", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, cm->height);
        vpx_write_y_frame(file_path, get_frame_new_buffer(cm));
        break;
    case DECODE_SR:
        fprintf(stderr, "%s: Not implemented", __func__);
        break;
    case DECODE_BILINEAR:
        fprintf(stderr, "%s: Not implemented", __func__);
        break;
    case DECODE_CACHE:
        sprintf(file_path, "%s/%s/frame/d_%dp.y", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, cm->height);
        vpx_write_y_frame(file_path, get_sr_frame_new_buffer(cm));
#if DEBUG_LR
            sprintf(file_path, "%s/%s/frame/%d_%dp.y", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->prefix, current_video_frame, cm->height);
            vpx_write_y_frame(file_path, get_frame_new_buffer(cm));
#endif
        break;
    }
}

static void save_intermediate_frame(VP9_COMMON *cm, int current_video_frame, int current_super_frame) {
    char file_path[PATH_MAX];

    switch (cm->mobinas_cfg->frame_type) {
    case DECODED_FRAME:
        save_decoded_intermediate_frame(cm, current_video_frame, current_super_frame);
        break;
    case SERIALIZED_FRAME:
        save_serialized_intermediate_frame(cm, current_video_frame, current_super_frame);
        break;
    case ALL_FRAME:
        save_decoded_intermediate_frame(cm, current_video_frame, current_super_frame);
        save_serialized_intermediate_frame(cm, current_video_frame, current_super_frame);
        break;
    }
}

static void save_final_frame(VP9_COMMON *cm, int current_video_frame) {
    char file_path[PATH_MAX];

    switch (cm->mobinas_cfg->frame_type) {
    case DECODED_FRAME:
        save_decoded_final_frame(cm, current_video_frame);
        break;
    case SERIALIZED_FRAME:
        save_serialized_final_frame(cm, current_video_frame);
        break;
    case ALL_FRAME:
        save_decoded_final_frame(cm, current_video_frame);
        save_serialized_final_frame(cm, current_video_frame);
        break;
    }
}

static void save_latency_result(VP9Decoder *pbi, int current_video_frame, int current_super_frame)
{
    char log[LOG_MAX];
    const int num_threads = (pbi->max_threads > 1) ? pbi->max_threads : 1;

    for (int i = 0; i < num_threads; ++i) {
        mobinas_worker_data_t *mwd = &pbi->mobinas_worker_data[i];

        //latency log
        memset(log, 0, sizeof(log));
        sprintf(log, "%d\t%d\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\n", current_video_frame,
                current_super_frame, pbi->common.latency.decode_frame,
                mwd->latency.interp_intra_block, mwd->latency.interp_inter_residual,
                mwd->latency.decode_intra_block, mwd->latency.decode_inter_block,
                mwd->latency.decode_inter_residual);
        fputs(log, mwd->latency_log);
    }
}

static void save_metadata_result(VP9Decoder *pbi, int current_video_frame, int current_super_frame)
{
    char log[LOG_MAX];
    const int num_threads = (pbi->max_threads > 1) ? pbi->max_threads : 1;

    for (int i = 0; i < num_threads; ++i) {
        mobinas_worker_data_t *mwd = &pbi->mobinas_worker_data[i];

        //metadata log
        memset(log, 0, sizeof(log));
        sprintf(log, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", current_video_frame, current_super_frame, pbi->common.apply_dnn, mwd->count, mwd->intra_count, mwd->inter_count,
                mwd->inter_noskip_count, mwd->adaptive_cache_count);
        fputs(log, mwd->metadata_log);
    }
}

static void save_cache_quality_result(VP9_COMMON *cm, int current_video_frame){
    char file_path[PATH_MAX] = {0};
    char log[LOG_MAX] = {0};
    PSNR_STATS psnr_sr, psnr_lr;

    //measure sr-cached frame quality
    int width = get_sr_frame_new_buffer(cm)->y_crop_width; //check: sr frame
    int height = get_sr_frame_new_buffer(cm)->y_crop_height; //check: sr frame

    sprintf(file_path, "%s/%s/serialize/%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->compare_file, current_video_frame, height);
    if(vpx_deserialize_load(cm->hr_reference_frame, file_path, width, height,
                            cm->subsampling_x, cm->subsampling_y, cm->byte_alignment))
    {
        vpx_internal_error(&cm->error, VPX_MOBINAS_ERROR,
                           "deserialize failed");
    }
    vpx_calc_psnr(get_sr_frame_new_buffer(cm), cm->hr_reference_frame, &psnr_sr); //check: sr frame
    sprintf(log, "%d\t%.2f\t%.2f\t%.2f\t%.2f\n", current_video_frame, psnr_sr.psnr[0], psnr_sr.psnr[1], psnr_sr.psnr[2], psnr_sr.psnr[3]);
    fputs(log, cm->quality_log);
    fprintf(stderr, "[PSNR] %d frame: %.2fdB\n", current_video_frame, psnr_sr.psnr[0]);

    //measure lr frame quality (it should be 100.0 dB)
#if DEBUG_LR
    printf("debug_lr is not false");
    width = get_frame_new_buffer(cm)->y_crop_width;
    height = get_frame_new_buffer(cm)->y_crop_height;
    sprintf(file_path, "%s/%s/serialize/%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->target_file, current_video_frame, height);
    if(vpx_deserialize_load(cm->lr_reference_frame, file_path, width, height,
                            cm->subsampling_x, cm->subsampling_y, cm->byte_alignment))
    {
        vpx_internal_error(&cm->error, VPX_MOBINAS_ERROR,
                           "deserialize failed");
    }
    vpx_calc_psnr(get_frame_new_buffer(cm), cm->lr_reference_frame, &psnr_lr);

    fprintf(stdout, "[PSNR] %d sr-cached frame: %.2fdB, lr frame: %.2fdB", current_video_frame, psnr_sr.psnr[0], psnr_lr.psnr[0]);

    //qualtiy log
    sprintf(log, "%d\t%.2f\t%.2f\t%.2f\t%.2f\n", current_video_frame, psnr_lr.psnr[0], psnr_lr.psnr[1], psnr_lr.psnr[2], psnr_lr.psnr[3]);
    fputs(log, cm->quality_log);
    fprintf(stderr, "[PSNR] %d frame: %.2fdB\n", current_video_frame, psnr_lr.psnr[0]);
#endif
}

static void save_sr_quality_result(VP9_COMMON *cm, int current_video_frame){
    char file_path[PATH_MAX] = {0};
    char log[LOG_MAX] = {0};
    PSNR_STATS psnr_sr, psnr_lr;

    //measure sr-cached frame quality
    int width = get_sr_frame_new_buffer(cm)->y_crop_width; //check: sr frame
    int height = get_sr_frame_new_buffer(cm)->y_crop_height; //check: sr frame

    sprintf(file_path, "%s/%s/serialize/%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->compare_file, current_video_frame, height);
    if(vpx_deserialize_load(cm->hr_reference_frame, file_path, width, height,
                            cm->subsampling_x, cm->subsampling_y, cm->byte_alignment))
    {
        vpx_internal_error(&cm->error, VPX_MOBINAS_ERROR,
                           "deserialize failed");
    }
    vpx_calc_psnr(get_sr_frame_new_buffer(cm), cm->hr_reference_frame, &psnr_sr); //check: sr frame
    sprintf(log, "%d\t%.2f\t%.2f\t%.2f\t%.2f\n", current_video_frame, psnr_sr.psnr[0], psnr_sr.psnr[1], psnr_sr.psnr[2], psnr_sr.psnr[3]);
    fputs(log, cm->quality_log);
    fprintf(stderr, "[PSNR] %d frame: %.2fdB\n", current_video_frame, psnr_sr.psnr[0]);
}


static void save_decoded_quality_result(VP9_COMMON *cm, int current_video_frame){
    char file_path[PATH_MAX] = {0};
    char log[LOG_MAX] = {0};
    PSNR_STATS psnr;

    int width = get_frame_new_buffer(cm)->y_crop_width; //check: sr frame
    int height = get_frame_new_buffer(cm)->y_crop_height; //check: sr frame

    sprintf(file_path, "%s/%s/serialize/%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->compare_file, current_video_frame, height);
    if(vpx_deserialize_load(cm->hr_reference_frame, file_path, width, height,
                            cm->subsampling_x, cm->subsampling_y, cm->byte_alignment))
    {
        vpx_internal_error(&cm->error, VPX_MOBINAS_ERROR,
                           "deserialize failed");
    }

    vpx_calc_psnr(get_frame_new_buffer(cm), cm->hr_reference_frame, &psnr); //check: sr frame

    //qualtiy log
    sprintf(log, "%d\t%.2f\t%.2f\t%.2f\t%.2f\n", current_video_frame, psnr.psnr[0], psnr.psnr[1], psnr.psnr[2], psnr.psnr[3]);
    fputs(log, cm->quality_log);
    fprintf(stderr, "[PSNR] %d frame: %.2fdB\n", current_video_frame, psnr.psnr[0]);

}

static void save_bilinear_quality_result(VP9_COMMON *cm, int current_video_frame) {
    char file_path[PATH_MAX] = {0};
    char log[LOG_MAX] = {0};
    PSNR_STATS psnr;

    int target_width = cm->hr_bilinear_frame->y_crop_width; //check: original frame
    int target_height = cm->hr_bilinear_frame->y_crop_height; //check: original frame

    sprintf(file_path, "%s/%s/serialize/%d_%dp.serialize", cm->mobinas_cfg->save_dir, cm->mobinas_cfg->compare_file,
            current_video_frame, target_height);
    if (vpx_deserialize_load(cm->hr_reference_frame, file_path, target_width, target_height,
                             cm->subsampling_x, cm->subsampling_y, cm->byte_alignment)) {
        vpx_internal_error(&cm->error, VPX_MOBINAS_ERROR,
                           "deserialize failed");
    }
    vpx_calc_psnr(cm->hr_bilinear_frame, cm->hr_reference_frame, &psnr);

    //qualtiy log
    sprintf(log, "%d\t%.2f\t%.2f\t%.2f\t%.2f\n", current_video_frame, psnr.psnr[0],
            psnr.psnr[1], psnr.psnr[2], psnr.psnr[3]);
    fputs(log, cm->quality_log);
    fprintf(stdout, "[PSNR] %d frame: %.2fdB\n", current_video_frame, psnr.psnr[0]);
}

static void save_quality_result(VP9_COMMON *cm, int current_video_frame) {
    switch (cm->mobinas_cfg->decode_mode) {
    case DECODE:
        save_decoded_quality_result(cm, current_video_frame);
        break;
    case DECODE_SR:
        fprintf(stderr, "%s: Not implemented", __func__);
        save_sr_quality_result(cm, current_video_frame);
        break;
    case DECODE_BILINEAR:
        save_bilinear_quality_result(cm, current_video_frame);
        break;
    case DECODE_CACHE:
        save_cache_quality_result(cm, current_video_frame);
        break;
    }
}

//TODO (hyunho - cache reset): read a cache reset profile
static vpx_codec_err_t decoder_decode(vpx_codec_alg_priv_t *ctx,
                                      const uint8_t *data, unsigned int data_sz,
                                      void *user_priv, long deadline) {

    const uint8_t *data_start = data;
    const uint8_t *const data_end = data + data_sz;
    vpx_codec_err_t res;
    uint32_t frame_sizes[8];
    int frame_count;
    VP9_COMMON *cm;

    if (data == NULL && data_sz == 0) {
        ctx->flushed = 1;
        return VPX_CODEC_OK;
    }

    // Reset flushed when receiving a valid frame.
    ctx->flushed = 0;

    // Initialize the decoder on the first frame.
    if (ctx->pbi == NULL) {
        const vpx_codec_err_t res = init_decoder(ctx);
        if (res != VPX_CODEC_OK) return res;
    }
    cm = &ctx->pbi->common;

    res = vp9_parse_superframe_index(data, data_sz, frame_sizes, &frame_count,
                                     ctx->decrypt_cb, ctx->decrypt_state);
    if (res != VPX_CODEC_OK) return res;

    if (ctx->svc_decoding && ctx->svc_spatial_layer < frame_count - 1)
        frame_count = ctx->svc_spatial_layer + 1;

#if DEBUG_LATENCY
    struct timespec start_time, finish_time;
    double diff;
#endif

    cm->current_super_frame = 0;
    if (frame_count > 0) {
        int i;
        int current_video_frame;

        for (i = 0; i < frame_count; ++i) {
            const uint8_t *data_start_copy = data_start;
            const uint32_t frame_size = frame_sizes[i];
            vpx_codec_err_t res;
            if (data_start < data || frame_size > (uint32_t) (data_end - data_start)) {
                set_error_detail(ctx, "Invalid frame size in index");
                return VPX_CODEC_CORRUPT_FRAME;
            }
#if DEBUG_LATENCY
            memset(&cm->latency, 0, sizeof(cm->latency));
            clock_gettime( CLOCK_MONOTONIC, &start_time);
#endif
            res = decode_one(ctx, &data_start_copy, frame_size, user_priv, deadline);
            if (res != VPX_CODEC_OK) return res;
#if DEBUG_LATENCY
            clock_gettime( CLOCK_MONOTONIC, &finish_time);
            diff = (finish_time.tv_sec - start_time.tv_sec) * 1000 + (finish_time.tv_nsec - start_time.tv_nsec) / BILLION * 1000.0;
            cm->latency.decode_frame += diff;
#endif

            data_start += frame_size;

            /*******************Hyunho************************/
            if (cm->show_frame == 0) current_video_frame = cm->current_video_frame;
            else current_video_frame = cm->current_video_frame - 1;

            if (cm->mobinas_cfg->save_intermediate_frame) save_intermediate_frame(cm, current_video_frame, cm->current_super_frame);
            if (cm->mobinas_cfg->save_latency_result) save_latency_result(ctx->pbi, current_video_frame, cm->current_super_frame);
            if (cm->mobinas_cfg->save_metadata_result) save_metadata_result(ctx->pbi, current_video_frame, cm->current_super_frame);

            cm->current_super_frame++;
            /*******************Hyunho************************/
        }
    } else {
        while (data_start < data_end) {
            const uint32_t frame_size = (uint32_t) (data_end - data_start);
#if DEBUG_LATENCY
            memset(&cm->latency, 0, sizeof(cm->latency));
            clock_gettime( CLOCK_MONOTONIC, &start_time);
#endif
            const vpx_codec_err_t res = decode_one(ctx, &data_start, frame_size, user_priv, deadline);
            if (res != VPX_CODEC_OK) return res;
#if DEBUG_LATENCY
            clock_gettime( CLOCK_MONOTONIC, &finish_time);
            diff = (finish_time.tv_sec - start_time.tv_sec) * 1000 + (finish_time.tv_nsec - start_time.tv_nsec) / BILLION * 1000.0;
            cm->latency.decode_frame += diff;
#endif

            // Account for suboptimal termination by the encoder.
            while (data_start < data_end) {
                const uint8_t marker =
                        read_marker(ctx->decrypt_cb, ctx->decrypt_state, data_start);
                if (marker) break;

                ++data_start;
            }
            /*******************Hyunho************************/
            if (cm->mobinas_cfg->save_intermediate_frame) save_intermediate_frame(cm, cm->current_video_frame - 1, cm->current_super_frame);
            if (cm->mobinas_cfg->save_latency_result) save_latency_result(ctx->pbi, cm->current_video_frame - 1, cm->current_super_frame);
            if (cm->mobinas_cfg->save_metadata_result) save_metadata_result(ctx->pbi, cm->current_video_frame - 1, cm->current_super_frame);
            /*******************Hyunho************************/
        }
    }
    /*******************Hyunho************************/
    if (cm->mobinas_cfg->save_final_frame) save_final_frame(cm, cm->current_video_frame - 1);
    if (cm->mobinas_cfg->save_quality_result) save_quality_result(cm, cm->current_video_frame - 1);
    /*******************Hyunho************************/
    return res;
}

static vpx_image_t *decoder_get_frame(vpx_codec_alg_priv_t *ctx,
                                      vpx_codec_iter_t *iter) {
    vpx_image_t *img = NULL;

//     Legacy parameter carried over from VP8. Has no effect for VP9 since we
//     always return only 1 frame per decode call.
    (void) iter;

    if (ctx->pbi != NULL) {
        YV12_BUFFER_CONFIG sd;
        vp9_ppflags_t flags = {0, 0, 0};
        if (ctx->base.init_flags & VPX_CODEC_USE_POSTPROC) set_ppflags(ctx, &flags);
        if (vp9_get_raw_frame(ctx->pbi, &sd, &flags) == 0) {
            VP9_COMMON *const cm = &ctx->pbi->common;
            RefCntBuffer *const frame_bufs = cm->buffer_pool->frame_bufs;
            ctx->last_show_frame = ctx->pbi->common.new_fb_idx;
            if (ctx->need_resync) return NULL;
            yuvconfig2image(&ctx->img, &sd, ctx->user_priv);
            ctx->img.fb_priv = frame_bufs[cm->new_fb_idx].raw_frame_buffer.priv;
            img = &ctx->img;
            return img;
        }
    }
    return NULL;
}

static vpx_codec_err_t decoder_set_fb_fn(
        vpx_codec_alg_priv_t *ctx, vpx_get_frame_buffer_cb_fn_t cb_get,
        vpx_release_frame_buffer_cb_fn_t cb_release, void *cb_priv) {
    if (cb_get == NULL || cb_release == NULL) {
        return VPX_CODEC_INVALID_PARAM;
    } else if (ctx->pbi == NULL) {
        // If the decoder has already been initialized, do not accept changes to
        // the frame buffer functions.
        ctx->get_ext_fb_cb = cb_get;
        ctx->release_ext_fb_cb = cb_release;
        ctx->ext_priv = cb_priv;
        return VPX_CODEC_OK;
    }

    return VPX_CODEC_ERROR;
}

static vpx_codec_err_t ctrl_set_reference(vpx_codec_alg_priv_t *ctx,
                                          va_list args) {
    vpx_ref_frame_t *const data = va_arg(args, vpx_ref_frame_t *);

    if (data) {
        vpx_ref_frame_t *const frame = (vpx_ref_frame_t *) data;
        YV12_BUFFER_CONFIG sd;
        image2yuvconfig(&frame->img, &sd);
        return vp9_set_reference_dec(
                &ctx->pbi->common, ref_frame_to_vp9_reframe(frame->frame_type), &sd);
    } else {
        return VPX_CODEC_INVALID_PARAM;
    }
}

static vpx_codec_err_t ctrl_copy_reference(vpx_codec_alg_priv_t *ctx,
                                           va_list args) {
    vpx_ref_frame_t *data = va_arg(args, vpx_ref_frame_t *);

    if (data) {
        vpx_ref_frame_t *frame = (vpx_ref_frame_t *) data;
        YV12_BUFFER_CONFIG sd;
        image2yuvconfig(&frame->img, &sd);
        return vp9_copy_reference_dec(ctx->pbi, (VP9_REFFRAME) frame->frame_type,
                                      &sd);
    } else {
        return VPX_CODEC_INVALID_PARAM;
    }
}

static vpx_codec_err_t ctrl_get_reference(vpx_codec_alg_priv_t *ctx,
                                          va_list args) {
    vp9_ref_frame_t *data = va_arg(args, vp9_ref_frame_t *);

    if (data) {
        YV12_BUFFER_CONFIG *fb;
        fb = get_ref_frame(&ctx->pbi->common, data->idx);
        if (fb == NULL) return VPX_CODEC_ERROR;
        yuvconfig2image(&data->img, fb, NULL);
        return VPX_CODEC_OK;
    } else {
        return VPX_CODEC_INVALID_PARAM;
    }
}

static vpx_codec_err_t ctrl_set_postproc(vpx_codec_alg_priv_t *ctx,
                                         va_list args) {
#if CONFIG_VP9_POSTPROC
    vp8_postproc_cfg_t *data = va_arg(args, vp8_postproc_cfg_t *);

    if (data) {
      ctx->postproc_cfg_set = 1;
      ctx->postproc_cfg = *((vp8_postproc_cfg_t *)data);
      return VPX_CODEC_OK;
    } else {
      return VPX_CODEC_INVALID_PARAM;
    }
#else
    (void) ctx;
    (void) args;
    return VPX_CODEC_INCAPABLE;
#endif
}

static vpx_codec_err_t ctrl_get_quantizer(vpx_codec_alg_priv_t *ctx,
                                          va_list args) {
    int *const arg = va_arg(args, int *);
    if (arg == NULL || ctx->pbi == NULL) return VPX_CODEC_INVALID_PARAM;
    *arg = ctx->pbi->common.base_qindex;
    return VPX_CODEC_OK;
}

static vpx_codec_err_t ctrl_get_last_ref_updates(vpx_codec_alg_priv_t *ctx,
                                                 va_list args) {
    int *const update_info = va_arg(args, int *);

    if (update_info) {
        if (ctx->pbi != NULL) {
            *update_info = ctx->pbi->refresh_frame_flags;
            return VPX_CODEC_OK;
        } else {
            return VPX_CODEC_ERROR;
        }
    }

    return VPX_CODEC_INVALID_PARAM;
}

static vpx_codec_err_t ctrl_get_frame_corrupted(vpx_codec_alg_priv_t *ctx,
                                                va_list args) {
    int *corrupted = va_arg(args, int *);

    if (corrupted) {
        if (ctx->pbi != NULL) {
            RefCntBuffer *const frame_bufs = ctx->pbi->common.buffer_pool->frame_bufs;
            if (ctx->pbi->common.frame_to_show == NULL) return VPX_CODEC_ERROR;
            if (ctx->last_show_frame >= 0)
                *corrupted = frame_bufs[ctx->last_show_frame].buf.corrupted;
            return VPX_CODEC_OK;
        } else {
            return VPX_CODEC_ERROR;
        }
    }

    return VPX_CODEC_INVALID_PARAM;
}

static vpx_codec_err_t ctrl_get_frame_size(vpx_codec_alg_priv_t *ctx,
                                           va_list args) {
    int *const frame_size = va_arg(args, int *);

    if (frame_size) {
        if (ctx->pbi != NULL) {
            const VP9_COMMON *const cm = &ctx->pbi->common;
            frame_size[0] = cm->width;
            frame_size[1] = cm->height;
            return VPX_CODEC_OK;
        } else {
            return VPX_CODEC_ERROR;
        }
    }

    return VPX_CODEC_INVALID_PARAM;
}

static vpx_codec_err_t ctrl_get_render_size(vpx_codec_alg_priv_t *ctx,
                                            va_list args) {
    int *const render_size = va_arg(args, int *);

    if (render_size) {
        if (ctx->pbi != NULL) {
            const VP9_COMMON *const cm = &ctx->pbi->common;
            render_size[0] = cm->render_width;
            render_size[1] = cm->render_height;
            return VPX_CODEC_OK;
        } else {
            return VPX_CODEC_ERROR;
        }
    }

    return VPX_CODEC_INVALID_PARAM;
}

static vpx_codec_err_t ctrl_get_bit_depth(vpx_codec_alg_priv_t *ctx,
                                          va_list args) {
    unsigned int *const bit_depth = va_arg(args, unsigned int *);

    if (bit_depth) {
        if (ctx->pbi != NULL) {
            const VP9_COMMON *const cm = &ctx->pbi->common;
            *bit_depth = cm->bit_depth;
            return VPX_CODEC_OK;
        } else {
            return VPX_CODEC_ERROR;
        }
    }

    return VPX_CODEC_INVALID_PARAM;
}

static vpx_codec_err_t ctrl_set_invert_tile_order(vpx_codec_alg_priv_t *ctx,
                                                  va_list args) {
    ctx->invert_tile_order = va_arg(args, int);
    return VPX_CODEC_OK;
}

static vpx_codec_err_t ctrl_set_decryptor(vpx_codec_alg_priv_t *ctx,
                                          va_list args) {
    vpx_decrypt_init *init = va_arg(args, vpx_decrypt_init *);
    ctx->decrypt_cb = init ? init->decrypt_cb : NULL;
    ctx->decrypt_state = init ? init->decrypt_state : NULL;
    return VPX_CODEC_OK;
}

static vpx_codec_err_t ctrl_set_byte_alignment(vpx_codec_alg_priv_t *ctx,
                                               va_list args) {
    const int legacy_byte_alignment = 0;
    const int min_byte_alignment = 32;
    const int max_byte_alignment = 1024;
    const int byte_alignment = va_arg(args, int);

    if (byte_alignment != legacy_byte_alignment &&
        (byte_alignment < min_byte_alignment ||
         byte_alignment > max_byte_alignment ||
         (byte_alignment & (byte_alignment - 1)) != 0))
        return VPX_CODEC_INVALID_PARAM;

    ctx->byte_alignment = byte_alignment;
    if (ctx->pbi != NULL) {
        ctx->pbi->common.byte_alignment = byte_alignment;
    }
    return VPX_CODEC_OK;
}

static vpx_codec_err_t ctrl_set_skip_loop_filter(vpx_codec_alg_priv_t *ctx,
                                                 va_list args) {
    ctx->skip_loop_filter = va_arg(args, int);

    if (ctx->pbi != NULL) {
        ctx->pbi->common.skip_loop_filter = ctx->skip_loop_filter;
    }

    return VPX_CODEC_OK;
}

static vpx_codec_err_t ctrl_set_spatial_layer_svc(vpx_codec_alg_priv_t *ctx,
                                                  va_list args) {
    ctx->svc_decoding = 1;
    ctx->svc_spatial_layer = va_arg(args, int);
    if (ctx->svc_spatial_layer < 0)
        return VPX_CODEC_INVALID_PARAM;
    else
        return VPX_CODEC_OK;
}

static vpx_codec_ctrl_fn_map_t decoder_ctrl_maps[] = {
        {VP8_COPY_REFERENCE,           ctrl_copy_reference},

        // Setters
        {VP8_SET_REFERENCE,            ctrl_set_reference},
        {VP8_SET_POSTPROC,             ctrl_set_postproc},
        {VP9_INVERT_TILE_DECODE_ORDER, ctrl_set_invert_tile_order},
        {VPXD_SET_DECRYPTOR,           ctrl_set_decryptor},
        {VP9_SET_BYTE_ALIGNMENT,       ctrl_set_byte_alignment},
        {VP9_SET_SKIP_LOOP_FILTER,     ctrl_set_skip_loop_filter},
        {VP9_DECODE_SVC_SPATIAL_LAYER, ctrl_set_spatial_layer_svc},

        // Getters
        {VPXD_GET_LAST_QUANTIZER,      ctrl_get_quantizer},
        {VP8D_GET_LAST_REF_UPDATES,    ctrl_get_last_ref_updates},
        {VP8D_GET_FRAME_CORRUPTED,     ctrl_get_frame_corrupted},
        {VP9_GET_REFERENCE,            ctrl_get_reference},
        {VP9D_GET_DISPLAY_SIZE,        ctrl_get_render_size},
        {VP9D_GET_BIT_DEPTH,           ctrl_get_bit_depth},
        {VP9D_GET_FRAME_SIZE,          ctrl_get_frame_size},

        {-1, NULL},
};

#ifndef VERSION_STRING
#define VERSION_STRING
#endif

CODEC_INTERFACE(vpx_codec_vp9_dx) = {
        "WebM Project VP9 Decoder" VERSION_STRING,
        VPX_CODEC_INTERNAL_ABI_VERSION,
#if CONFIG_VP9_HIGHBITDEPTH
        VPX_CODEC_CAP_HIGHBITDEPTH |
#endif
        VPX_CODEC_CAP_DECODER | VP9_CAP_POSTPROC |
        VPX_CODEC_CAP_EXTERNAL_FRAME_BUFFER,  // vpx_codec_caps_t
        decoder_init,                             // vpx_codec_init_fn_t
        decoder_destroy,                          // vpx_codec_destroy_fn_t
        decoder_ctrl_maps,                        // vpx_codec_ctrl_fn_map_t
        {
                // NOLINT
                decoder_peek_si,    // vpx_codec_peek_si_fn_t
                decoder_get_si,     // vpx_codec_get_si_fn_t
                decoder_decode,     // vpx_codec_decode_fn_t
                decoder_get_frame,  // vpx_codec_frame_get_fn_t
                decoder_set_fb_fn,  // vpx_codec_set_fb_fn_t
        },
        {
                // NOLINT
                0,
                NULL,  // vpx_codec_enc_cfg_map_t
                NULL,  // vpx_codec_encode_fn_t
                NULL,  // vpx_codec_get_cx_data_fn_t
                NULL,  // vpx_codec_enc_config_set_fn_t
                NULL,  // vpx_codec_get_global_headers_fn_t
                NULL,  // vpx_codec_get_preview_frame_fn_t
                NULL   // vpx_codec_enc_mr_get_mem_loc_fn_t
        },
        {
            load_mobinas_cfg
        }
};
