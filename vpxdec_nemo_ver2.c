/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

#include "./vpx_config.h"

#if CONFIG_LIBYUV
#include "third_party/libyuv/include/libyuv/scale.h"
#endif

#include "./args.h"
#include "./ivfdec.h"

#include "vpx/vpx_decoder.h"
#include "vpx_ports/mem_ops.h"
#include "vpx_ports/vpx_timer.h"

#if CONFIG_VP8_DECODER || CONFIG_VP9_DECODER
#include "vpx/vp8dx.h"
#endif

#include "./md5_utils.h"

#include "./tools_common.h"
#if CONFIG_WEBM_IO
#include "./webmdec.h"
#endif
#include "./y4menc.h"

static const char *exec_name;

struct VpxDecInputContext
{
    struct VpxInputContext *vpx_input_ctx;
    struct WebmInputContext *webm_ctx;
};

typedef enum{
    LOW,
    MEDIUM,
    HIGH,
} nemo_dnn_quality;


static const arg_def_t help =
        ARG_DEF(NULL, "help", 0, "Show usage options and exit");
static const arg_def_t looparg =
        ARG_DEF(NULL, "loops", 1, "Number of times to decode the file");
static const arg_def_t codecarg = ARG_DEF(NULL, "codec", 1, "Codec to use");
static const arg_def_t use_yv12 =
        ARG_DEF(NULL, "yv12", 0, "Output raw YV12 frames");
static const arg_def_t use_i420 =
        ARG_DEF(NULL, "i420", 0, "Output raw I420 frames");
static const arg_def_t flipuvarg =
        ARG_DEF(NULL, "flipuv", 0, "Flip the chroma planes in the output");
static const arg_def_t rawvideo =
        ARG_DEF(NULL, "rawvideo", 0, "Output raw YUV frames");
static const arg_def_t noblitarg =
        ARG_DEF(NULL, "noblit", 0, "Don't process the decoded frames");
static const arg_def_t progressarg =
        ARG_DEF(NULL, "progress", 0, "Show progress after each frame decodes");
static const arg_def_t limitarg =
        ARG_DEF(NULL, "limit", 1, "Stop decoding after n frames");
static const arg_def_t skiparg =
        ARG_DEF(NULL, "skip", 1, "Skip the first n input frames");
static const arg_def_t postprocarg =
        ARG_DEF(NULL, "postproc", 0, "Postprocess decoded frames");
static const arg_def_t summaryarg =
        ARG_DEF(NULL, "summary", 0, "Show timing summary");
static const arg_def_t outputfile =
        ARG_DEF("o", "output", 1, "Output file name pattern (see below)");
static const arg_def_t threadsarg =
        ARG_DEF("t", "threads", 1, "Max threads to use");
static const arg_def_t frameparallelarg =
        ARG_DEF(NULL, "frame-parallel", 0, "Frame parallel decode (ignored)");
static const arg_def_t verbosearg =
        ARG_DEF("v", "verbose", 0, "Show version string");
static const arg_def_t error_concealment =
        ARG_DEF(NULL, "error-concealment", 0, "Enable decoder error-concealment");
static const arg_def_t scalearg =
        ARG_DEF("S", "scale", 0, "Scale output frames uniformly");
static const arg_def_t continuearg =
        ARG_DEF("k", "keep-going", 0, "(debug) Continue decoding after error");
static const arg_def_t fb_arg =
        ARG_DEF(NULL, "frame-buffers", 1, "Number of frame buffers to use");
static const arg_def_t md5arg =
        ARG_DEF(NULL, "md5", 0, "Compute the MD5 sum of the decoded frame");
#if CONFIG_VP9_HIGHBITDEPTH
static const arg_def_t outbitdeptharg =
    ARG_DEF(NULL, "output-bit-depth", 1, "Output bit-depth for decoded frames");
#endif
static const arg_def_t svcdecodingarg =
        ARG_DEF(NULL, "svc-decode-layer", 1,
        "Decode SVC stream up to given spatial layer");
static const arg_def_t framestatsarg =
        ARG_DEF(NULL, "framestats", 1, "Output per-frame stats (.csv format)");
/* NEMO: New arguments */
static const arg_def_t datasetdirarg =
        ARG_DEF(NULL, "dataset-dir", 1, "Dataset directory");
static const arg_def_t inputvideonamearg =
        ARG_DEF(NULL, "input-video-name", 1, "Input video name");
static const arg_def_t referencevideonamearg =
        ARG_DEF(NULL, "reference-video-name", 1, "Reference video name (for quality measurement)");
static const arg_def_t outputwidtharg =
        ARG_DEF(NULL, "output-width", 1, "Output frame width");
static const arg_def_t outputheightarg =
        ARG_DEF(NULL, "output-height", 1, "Output frame height");
static const arg_def_t dnnscalearg =
        ARG_DEF(NULL, "dnn-scale", 1, "DNN scale (i.e., output size / input size");
static const arg_def_t dnnnamearg =
        ARG_DEF(NULL, "dnn-name", 1, "DNN name");
static const arg_def_t decodemodearg =
        ARG_DEF(NULL, "decode-mode", 1, "Decode mode");
static const arg_def_t dnnmodearg =
        ARG_DEF(NULL, "dnn-mode", 1, "DNN mode");
static const arg_def_t cachemodearg =
        ARG_DEF(NULL, "cache-mode", 1, "Cache mode");
static const arg_def_t cacheprofilenamearg =
        ARG_DEF(NULL, "cache-profile-name", 1, "Cache profile name");
static const arg_def_t savergbframedarg =
        ARG_DEF(NULL, "save-rgbframe", 0, "Save RGB frames");
static const arg_def_t saveyuvframearg=
        ARG_DEF(NULL, "save-yuvframe", 0, "Save YUV frame");
static const arg_def_t savequalityarg =
        ARG_DEF(NULL, "save-quality", 0, "Save a quality log");
static const arg_def_t savelatencyarg =
        ARG_DEF(NULL, "save-latency", 0, "Save a latency log");
static const arg_def_t savemetadataarg =
        ARG_DEF(NULL, "save-metadata", 0, "Save a metadata log");
static const arg_def_t postfixarg =
        ARG_DEF(NULL, "postfix", 1, "Postfix for a directory name");
static const arg_def_t dnnruntimearg =
        ARG_DEF(NULL, "dnn-runtime", 1, "DNN runtime");
static const arg_def_t *all_args[] =
{   &help, &codecarg, &use_yv12, &use_i420, &flipuvarg, &rawvideo, &noblitarg, &progressarg, &limitarg, &skiparg,
    &postprocarg, &summaryarg, &outputfile, &threadsarg, &frameparallelarg, &verbosearg, &scalearg, &fb_arg,
    &md5arg, &error_concealment, &continuearg,
#if CONFIG_VP9_HIGHBITDEPTH
    &outbitdeptharg,
#endif
    &svcdecodingarg, &framestatsarg,
    &datasetdirarg, &inputvideonamearg, &referencevideonamearg,
    &outputwidtharg, &outputheightarg, &dnnscalearg, &dnnnamearg, &decodemodearg, &dnnmodearg, &cachemodearg, &cacheprofilenamearg,
    &savergbframedarg, &savequalityarg, &savelatencyarg, &saveyuvframearg,
    &postfixarg, &dnnruntimearg, NULL};
#if CONFIG_VP8_DECODER
static const arg_def_t addnoise_level =
ARG_DEF(NULL, "noise-level", 1, "Enable VP8 postproc add noise");
static const arg_def_t deblock =
ARG_DEF(NULL, "deblock", 0, "Enable VP8 deblocking");
static const arg_def_t demacroblock_level = ARG_DEF(
        NULL, "demacroblock-level", 1, "Enable VP8 demacroblocking, w/ level");
static const arg_def_t mfqe =
ARG_DEF(NULL, "mfqe", 0, "Enable multiframe quality enhancement");

static const arg_def_t *vp8_pp_args[] =
{   &addnoise_level, &deblock,
    &demacroblock_level, &mfqe, NULL};
#endif

#if CONFIG_LIBYUV
static INLINE int libyuv_scale(vpx_image_t *src, vpx_image_t *dst, FilterModeEnum mode)
{
#if CONFIG_VP9_HIGHBITDEPTH
  if (src->fmt == VPX_IMG_FMT_I42016) {
    assert(dst->fmt == VPX_IMG_FMT_I42016);
    return I420Scale_16(
        (uint16_t *)src->planes[VPX_PLANE_Y], src->stride[VPX_PLANE_Y] / 2,
        (uint16_t *)src->planes[VPX_PLANE_U], src->stride[VPX_PLANE_U] / 2,
        (uint16_t *)src->planes[VPX_PLANE_V], src->stride[VPX_PLANE_V] / 2,
        src->d_w, src->d_h, (uint16_t *)dst->planes[VPX_PLANE_Y],
        dst->stride[VPX_PLANE_Y] / 2, (uint16_t *)dst->planes[VPX_PLANE_U],
        dst->stride[VPX_PLANE_U] / 2, (uint16_t *)dst->planes[VPX_PLANE_V],
        dst->stride[VPX_PLANE_V] / 2, dst->d_w, dst->d_h, mode);
  }
#endif
    assert(src->fmt == VPX_IMG_FMT_I420);
    assert(dst->fmt == VPX_IMG_FMT_I420);
    return I420Scale(src->planes[VPX_PLANE_Y], src->stride[VPX_PLANE_Y], src->planes[VPX_PLANE_U],
                     src->stride[VPX_PLANE_U], src->planes[VPX_PLANE_V], src->stride[VPX_PLANE_V], src->d_w, src->d_h,
                     dst->planes[VPX_PLANE_Y], dst->stride[VPX_PLANE_Y], dst->planes[VPX_PLANE_U],
                     dst->stride[VPX_PLANE_U], dst->planes[VPX_PLANE_V], dst->stride[VPX_PLANE_V], dst->d_w, dst->d_h,
                     mode);
}
#endif
void show_help(FILE *fout, int shorthelp)
{
    int i;

    fprintf(fout, "Usage: %s <options> inputfile\n\n", exec_name);

    if (shorthelp)
    {
        fprintf(fout, "Use --help to see the full list of options.\n");
        return;
    }

    fprintf(fout, "Options:\n");
    arg_show_usage(fout, all_args);
#if CONFIG_VP8_DECODER
  fprintf(fout, "\nVP8 Postprocessing Options:\n");
  arg_show_usage(fout, vp8_pp_args);
#endif
    fprintf(fout, "\nOutput File Patterns:\n\n"
            "  The -o argument specifies the name of the file(s) to "
            "write to. If the\n  argument does not include any escape "
            "characters, the output will be\n  written to a single file. "
            "Otherwise, the filename will be calculated by\n  expanding "
            "the following escape characters:\n");
    fprintf(fout, "\n\t%%w   - Frame width"
            "\n\t%%h   - Frame height"
            "\n\t%%<n> - Frame number, zero padded to <n> places (1..9)"
            "\n\n  Pattern arguments are only supported in conjunction "
            "with the --yv12 and\n  --i420 options. If the -o option is "
            "not specified, the output will be\n  directed to stdout.\n");
    fprintf(fout, "\nIncluded decoders:\n\n");

    for (i = 0; i < get_vpx_decoder_count(); ++i)
    {
        const VpxInterface *const decoder = get_vpx_decoder_by_index(i);
        fprintf(fout, "    %-6s - %s\n", decoder->name, vpx_codec_iface_name(decoder->codec_interface()));
    }
}

void usage_exit(void)
{
    show_help(stderr, 1);
    exit(EXIT_FAILURE);
}

static int raw_read_frame(FILE *infile, uint8_t **buffer, size_t *bytes_read, size_t *buffer_size)
{
    char raw_hdr[RAW_FRAME_HDR_SZ];
    size_t frame_size = 0;

    if (fread(raw_hdr, RAW_FRAME_HDR_SZ, 1, infile) != 1)
    {
        if (!feof(infile))
            warn("Failed to read RAW frame size\n");
    }
    else
    {
        const size_t kCorruptFrameThreshold = 256 * 1024 * 1024;
        const size_t kFrameTooSmallThreshold = 256 * 1024;
        frame_size = mem_get_le32(raw_hdr);

        if (frame_size > kCorruptFrameThreshold)
        {
            warn("Read invalid frame size (%u)\n", (unsigned int) frame_size);
            frame_size = 0;
        }

        if (frame_size < kFrameTooSmallThreshold)
        {
            warn("Warning: Read invalid frame size (%u) - not a raw file?\n", (unsigned int) frame_size);
        }

        if (frame_size > *buffer_size)
        {
            uint8_t *new_buf = realloc(*buffer, 2 * frame_size);
            if (new_buf)
            {
                *buffer = new_buf;
                *buffer_size = 2 * frame_size;
            }
            else
            {
                warn("Failed to allocate compressed data buffer\n");
                frame_size = 0;
            }
        }
    }

    if (!feof(infile))
    {
        if (fread(*buffer, 1, frame_size, infile) != frame_size)
        {
            warn("Failed to read full frame\n");
            return 1;
        }
        *bytes_read = frame_size;
    }

    return 0;
}

static int read_frame(struct VpxDecInputContext *input, uint8_t **buf, size_t *bytes_in_buffer, size_t *buffer_size)
{
    switch (input->vpx_input_ctx->file_type)
    {
#if CONFIG_WEBM_IO
    case FILE_TYPE_WEBM:
        return webm_read_frame(input->webm_ctx, buf, bytes_in_buffer);
#endif
    case FILE_TYPE_RAW:
        return raw_read_frame(input->vpx_input_ctx->file, buf, bytes_in_buffer, buffer_size);
    case FILE_TYPE_IVF:
        return ivf_read_frame(input->vpx_input_ctx->file, buf, bytes_in_buffer, buffer_size);
    default:
        return 1;
    }
}

static void update_image_md5(const vpx_image_t *img, const int planes[3], MD5Context *md5)
{
    int i, y;

    for (i = 0; i < 3; ++i)
    {
        const int plane = planes[i];
        const unsigned char *buf = img->planes[plane];
        const int stride = img->stride[plane];
        const int w = vpx_img_plane_width(img, plane) * ((img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) ? 2 : 1);
        const int h = vpx_img_plane_height(img, plane);

        for (y = 0; y < h; ++y)
        {
            MD5Update(md5, buf, w);
            buf += stride;
        }
    }
}

static void write_image_file(const vpx_image_t *img, const int planes[3], FILE *file)
{
    int i, y;
#if CONFIG_VP9_HIGHBITDEPTH
  const int bytes_per_sample = ((img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) ? 2 : 1);
#else
    const int bytes_per_sample = 1;
#endif

    for (i = 0; i < 3; ++i)
    {
        const int plane = planes[i];
        const unsigned char *buf = img->planes[plane];
        const int stride = img->stride[plane];
        const int w = vpx_img_plane_width(img, plane);
        const int h = vpx_img_plane_height(img, plane);

        for (y = 0; y < h; ++y)
        {
            fwrite(buf, bytes_per_sample, w, file);
            buf += stride;
        }
    }
}

static int file_is_raw(struct VpxInputContext *input)
{
    uint8_t buf[32];
    int is_raw = 0;
    vpx_codec_stream_info_t si;

    si.sz = sizeof(si);

    if (fread(buf, 1, 32, input->file) == 32)
    {
        int i;

        if (mem_get_le32(buf) < 256 * 1024 * 1024)
        {
            for (i = 0; i < get_vpx_decoder_count(); ++i)
            {
                const VpxInterface *const decoder = get_vpx_decoder_by_index(i);
                if (!vpx_codec_peek_stream_info(decoder->codec_interface(), buf + 4, 32 - 4, &si))
                {
                    is_raw = 1;
                    input->fourcc = decoder->fourcc;
                    input->width = si.w;
                    input->height = si.h;
                    input->framerate.numerator = 30;
                    input->framerate.denominator = 1;
                    break;
                }
            }
        }
    }

    rewind(input->file);
    return is_raw;
}

static void show_progress(int frame_in, int frame_out, uint64_t dx_time)
{
    fprintf(stderr, "%d decoded frames/%d showed frames in %" PRId64 " us (%.2f fps)\r", frame_in, frame_out, dx_time,
            (double) frame_out * 1000000.0 / (double) dx_time);
}

struct ExternalFrameBuffer
{
    uint8_t *data;
    size_t size;
    int in_use;
};

struct ExternalFrameBufferList
{
    int num_external_frame_buffers;
    struct ExternalFrameBuffer *ext_fb;
};

// Callback used by libvpx to request an external frame buffer. |cb_priv|
// Application private data passed into the set function. |min_size| is the
// minimum size in bytes needed to decode the next frame. |fb| pointer to the
// frame buffer.
static int get_vp9_frame_buffer(void *cb_priv, size_t min_size, vpx_codec_frame_buffer_t *fb)
{
    int i;
    struct ExternalFrameBufferList *const ext_fb_list = (struct ExternalFrameBufferList*) cb_priv;
    if (ext_fb_list == NULL)
        return -1;

    // Find a free frame buffer.
    for (i = 0; i < ext_fb_list->num_external_frame_buffers; ++i)
    {
        if (!ext_fb_list->ext_fb[i].in_use)
            break;
    }

    if (i == ext_fb_list->num_external_frame_buffers)
        return -1;

    if (ext_fb_list->ext_fb[i].size < min_size)
    {
        free(ext_fb_list->ext_fb[i].data);
        ext_fb_list->ext_fb[i].data = (uint8_t*) calloc(min_size, sizeof(uint8_t));
        if (!ext_fb_list->ext_fb[i].data)
            return -1;

        ext_fb_list->ext_fb[i].size = min_size;
    }

    fb->data = ext_fb_list->ext_fb[i].data;
    fb->size = ext_fb_list->ext_fb[i].size;
    ext_fb_list->ext_fb[i].in_use = 1;

    // Set the frame buffer's private data to point at the external frame buffer.
    fb->priv = &ext_fb_list->ext_fb[i];
    return 0;
}

// Callback used by libvpx when there are no references to the frame buffer.
// |cb_priv| user private data passed into the set function. |fb| pointer
// to the frame buffer.
static int release_vp9_frame_buffer(void *cb_priv, vpx_codec_frame_buffer_t *fb)
{
    struct ExternalFrameBuffer *const ext_fb = (struct ExternalFrameBuffer*) fb->priv;
    (void) cb_priv;
    ext_fb->in_use = 0;
    return 0;
}

static void generate_filename(const char *pattern, char *out, size_t q_len, unsigned int d_w, unsigned int d_h,
                              unsigned int frame_in)
{
    const char *p = pattern;
    char *q = out;

    do
    {
        char *next_pat = strchr(p, '%');

        if (p == next_pat)
        {
            size_t pat_len;

            /* parse the pattern */
            q[q_len - 1] = '\0';
            switch (p[1])
            {
            case 'w':
                snprintf(q, q_len - 1, "%d", d_w);
                break;
            case 'h':
                snprintf(q, q_len - 1, "%d", d_h);
                break;
            case '1':
                snprintf(q, q_len - 1, "%d", frame_in);
                break;
            case '2':
                snprintf(q, q_len - 1, "%02d", frame_in);
                break;
            case '3':
                snprintf(q, q_len - 1, "%03d", frame_in);
                break;
            case '4':
                snprintf(q, q_len - 1, "%04d", frame_in);
                break;
            case '5':
                snprintf(q, q_len - 1, "%05d", frame_in);
                break;
            case '6':
                snprintf(q, q_len - 1, "%06d", frame_in);
                break;
            case '7':
                snprintf(q, q_len - 1, "%07d", frame_in);
                break;
            case '8':
                snprintf(q, q_len - 1, "%08d", frame_in);
                break;
            case '9':
                snprintf(q, q_len - 1, "%09d", frame_in);
                break;
            default:
                die("Unrecognized pattern %%%c\n", p[1]);
                break;
            }

            pat_len = strlen(q);
            if (pat_len >= q_len - 1)
                die("Output filename too long.\n");
            q += pat_len;
            p += 2;
            q_len -= pat_len;
        }
        else
        {
            size_t copy_len;

            /* copy the next segment */
            if (!next_pat)
                copy_len = strlen(p);
            else
                copy_len = next_pat - p;

            if (copy_len >= q_len - 1)
                die("Output filename too long.\n");

            memcpy(q, p, copy_len);
            q[copy_len] = '\0';
            q += copy_len;
            p += copy_len;
            q_len -= copy_len;
        }
    }
    while (*p);
}

static int is_single_file(const char *outfile_pattern)
{
    const char *p = outfile_pattern;

    do
    {
        p = strchr(p, '%');
        if (p && p[1] >= '1' && p[1] <= '9')
            return 0;  // pattern contains sequence number, so it's not unique
        if (p)
            p++;
    }
    while (p);

    return 1;
}

static void print_md5(unsigned char digest[16], const char *filename)
{
    int i;

    for (i = 0; i < 16; ++i)
        printf("%02x", digest[i]);
    printf("  %s\n", filename);
}

static FILE* open_outfile(const char *name)
{
    if (strcmp("-", name) == 0)
    {
        set_binary_mode(stdout);
        return stdout;
    }
    else
    {
        FILE *file = fopen(name, "wb");
        if (!file)
            fatal("Failed to open output file '%s'", name);
        return file;
    }
}

#if CONFIG_VP9_HIGHBITDEPTH
static int img_shifted_realloc_required(const vpx_image_t *img,
                                        const vpx_image_t *shifted,
                                        vpx_img_fmt_t required_fmt) {
  return img->d_w != shifted->d_w || img->d_h != shifted->d_h ||
         required_fmt != shifted->fmt;
}
#endif


static void append_postfix_to_string(char *str, const char *postfix) {
    if (postfix != NULL) {
        sprintf(str, "%s/%s", str, postfix);
    }
}

static void setup_directory(nemo_cfg_t *nemo_cfg, const char *dataset_dir, const char *input_video_name, const char *reference_video_name,
                                const char *dnn_name, const char *cache_profile_name, const char *postfix) {
    switch(nemo_cfg->decode_mode) {
        case DECODE:
            sprintf(nemo_cfg->log_dir, "%s/log/%s", dataset_dir, input_video_name);
            sprintf(nemo_cfg->input_frame_dir, "%s/image/%s", dataset_dir, input_video_name);
            append_postfix_to_string(nemo_cfg->log_dir, postfix);
            append_postfix_to_string(nemo_cfg->input_frame_dir, postfix);

            if (nemo_cfg->save_quality) {
                sprintf(nemo_cfg->input_reference_frame_dir, "%s/image/%s", dataset_dir, reference_video_name);
                append_postfix_to_string(nemo_cfg->input_reference_frame_dir, postfix);
            }
            break;
        case DECODE_SR:
            sprintf(nemo_cfg->log_dir, "%s/log/%s/%s", dataset_dir, input_video_name, dnn_name);
            sprintf(nemo_cfg->input_frame_dir, "%s/image/%s", dataset_dir, input_video_name);
            sprintf(nemo_cfg->sr_frame_dir, "%s/image/%s/%s", dataset_dir, input_video_name, dnn_name);
            sprintf(nemo_cfg->sr_offline_frame_dir, "%s/image/%s/%s", dataset_dir, input_video_name, dnn_name);
            append_postfix_to_string(nemo_cfg->log_dir, postfix);
            append_postfix_to_string(nemo_cfg->input_frame_dir, postfix);
            append_postfix_to_string(nemo_cfg->sr_frame_dir, postfix);
            append_postfix_to_string(nemo_cfg->sr_offline_frame_dir, postfix);

            if (nemo_cfg->save_quality) {
                sprintf(nemo_cfg->input_reference_frame_dir, "%s/image/%s", dataset_dir, input_video_name);
                sprintf(nemo_cfg->sr_reference_frame_dir, "%s/image/%s", dataset_dir, reference_video_name);
                append_postfix_to_string(nemo_cfg->input_reference_frame_dir, postfix);
                append_postfix_to_string(nemo_cfg->sr_reference_frame_dir, postfix);
            }
            break;
        case DECODE_CACHE:
            sprintf(nemo_cfg->log_dir, "%s/log/%s/%s", dataset_dir, input_video_name, dnn_name);
            sprintf(nemo_cfg->input_frame_dir, "%s/image/%s", dataset_dir, input_video_name);
            sprintf(nemo_cfg->sr_frame_dir, "%s/image/%s/%s", dataset_dir, input_video_name, dnn_name);
            sprintf(nemo_cfg->sr_offline_frame_dir, "%s/image/%s/%s", dataset_dir, input_video_name, dnn_name);
            append_postfix_to_string(nemo_cfg->log_dir, postfix);
            append_postfix_to_string(nemo_cfg->input_frame_dir, postfix);
            append_postfix_to_string(nemo_cfg->sr_frame_dir, postfix);
            append_postfix_to_string(nemo_cfg->sr_offline_frame_dir, postfix);

            if (cache_profile_name != NULL) {
                append_postfix_to_string(nemo_cfg->log_dir, cache_profile_name);
                append_postfix_to_string(nemo_cfg->sr_frame_dir, cache_profile_name);
            }
            else {
                append_postfix_to_string(nemo_cfg->log_dir, "FAST");
                append_postfix_to_string(nemo_cfg->sr_frame_dir, "FAST");
            }

            if (nemo_cfg->save_quality) {
                sprintf(nemo_cfg->input_reference_frame_dir, "%s/image/%s", dataset_dir, input_video_name);
                sprintf(nemo_cfg->sr_reference_frame_dir, "%s/image/%s", dataset_dir, reference_video_name);
                append_postfix_to_string(nemo_cfg->input_reference_frame_dir, postfix);
                append_postfix_to_string(nemo_cfg->sr_reference_frame_dir, postfix);
            }
            break;
    }
}


static void set_decode_mode(nemo_decode_mode *decode_mode, const char *decode_mode_str)
{
    if (strcmp(decode_mode_str, "decode") == 0) {
        *decode_mode = DECODE;
    }
    else if (strcmp(decode_mode_str, "decode_sr") == 0) {
        *decode_mode = DECODE_SR;
    }
    else if (strcmp(decode_mode_str, "decode_cache") == 0) {
        *decode_mode = DECODE_CACHE;
    }
    else {
        die("Unsupported decode mode: %s", decode_mode_str);
    }
}

static void set_dnn_mode(nemo_dnn_mode *dnn_mode, const char *dnn_mode_str)
{
    if (strcmp(dnn_mode_str, "no_dnn") == 0) {
        *dnn_mode = NO_DNN;
    }
    else if (strcmp(dnn_mode_str, "online_dnn") == 0) {
        *dnn_mode = ONLINE_DNN;
    }
    else if (strcmp(dnn_mode_str, "offline_dnn") == 0) {
        *dnn_mode = OFFLINE_DNN;
    }
    else {
        die("Unsupported dnn mode: %s", dnn_mode_str);
    }
}

static void set_cache_mode(nemo_cache_mode *cache_mode, const char *cache_mode_str)
{
    if (strcmp(cache_mode_str, "no_cache") == 0) {
        *cache_mode = NO_CACHE;
    }
    else if (strcmp(cache_mode_str, "key_frame_cache") == 0) {
        *cache_mode = KEY_FRAME_CACHE;
    }
    else if (strcmp(cache_mode_str, "profile_cache") == 0) {
        *cache_mode = PROFILE_CACHE;
    }
    else {
        die("Unsupported cache mode: %s", cache_mode_str);
    }
}

static void set_runtime(nemo_dnn_runtime *dnn_runtime, const char *dnn_runtime_str)
{
    if (strcmp(dnn_runtime_str, "cpu_float32") == 0) {
        *dnn_runtime = CPU_FLOAT32;
    }
    else if (strcmp(dnn_runtime_str, "gpu_float32") == 0) {
        *dnn_runtime = GPU_FLOAT32_16_HYBRID;
    }
    else if (strcmp(dnn_runtime_str, "gpu_float16") == 0) {
        *dnn_runtime = GPU_FLOAT16;
    }
    else {
        die("Unsupported dnn runtime: %s", dnn_runtime_str);
    }
}

static int main_loop(int argc, const char **argv_)
{
    vpx_codec_ctx_t decoder;
    char fn[PATH_MAX] = {0};
    int i;
    int ret = EXIT_FAILURE;
    uint8_t *buf = NULL;
    size_t bytes_in_buffer = 0, buffer_size = 0;
    FILE *infile;
    int frame_in = 0, frame_out = 0, flipuv = 0, noblit = 0;
    int do_md5 = 0, progress = 0;
    int stop_after = 0, postproc = 0, summary = 0, quiet = 1;
    int arg_skip = 0;
    int ec_enabled = 0;
    int keep_going = 0;
    const VpxInterface *interface = NULL;
    const VpxInterface *fourcc_interface = NULL;
    uint64_t dx_time = 0;
    struct arg arg;
    char **argv, **argi, **argj;

    int single_file;
    int use_y4m = 1;
    int opt_yv12 = 0;
    int opt_i420 = 0;
    vpx_codec_dec_cfg_t cfg =
    { 0, 0, 0 };
#if CONFIG_VP9_HIGHBITDEPTH
  unsigned int output_bit_depth = 0;
#endif
    int svc_decoding = 0;
    int svc_spatial_layer = 0;
#if CONFIG_VP8_DECODER
  vp8_postproc_cfg_t vp8_pp_cfg = { 0, 0, 0 };
#endif
    int frames_corrupted = 0;
    int dec_flags = 0;
    int do_scale = 0;
    vpx_image_t *scaled_img = NULL;
#if CONFIG_VP9_HIGHBITDEPTH
  vpx_image_t *img_shifted = NULL;
#endif
    int frame_avail, got_data, flush_decoder = 0;
    int num_external_frame_buffers = 0;
    struct ExternalFrameBufferList ext_fb_list =
    { 0, NULL };

    const char *outfile_pattern = NULL;
    char outfile_name[PATH_MAX] =
    { 0 };
    FILE *outfile = NULL;

    FILE *framestats_file = NULL;

    MD5Context md5_ctx;
    unsigned char md5_digest[16];

    struct VpxDecInputContext input =
    { NULL, NULL };
    struct VpxInputContext vpx_input_ctx;
#if CONFIG_WEBM_IO
    struct WebmInputContext webm_ctx;
    memset(&(webm_ctx), 0, sizeof(webm_ctx));
    input.webm_ctx = &webm_ctx;
#endif
    input.vpx_input_ctx = &vpx_input_ctx;

    /* NEMO variables */
    nemo_cfg_t *nemo_cfg = init_nemo_cfg();
    const char *dataset_dir = NULL;
    const char *input_video_name = NULL;
    const char *reference_video_name = NULL;
    const char *dnn_name = NULL;
    const char *cache_profile_name = NULL;
    char video_path[PATH_MAX];
    char dnn_path[PATH_MAX];
    char cache_profile_path[PATH_MAX];
    int dnn_scale = 0;
    const char *postfix = NULL;

    /* Parse command line */
    exec_name = argv_[0];
    argv = argv_dup(argc - 1, argv_ + 1);

    for (argi = argj = argv; (*argj = *argi); argi += arg.argv_step)
    {
        memset(&arg, 0, sizeof(arg));
        arg.argv_step = 1;

        if (arg_match(&arg, &help, argi))
        {
            show_help(stdout, 0);
            exit(EXIT_SUCCESS);
        }
        else if (arg_match(&arg, &codecarg, argi))
        {
            interface = get_vpx_decoder_by_name(arg.val);
            if (!interface)
                die("Error: Unrecognized argument (%s) to --codec\n", arg.val);
        }
        else if (arg_match(&arg, &looparg, argi))
        {
            // no-op
        }
        else if (arg_match(&arg, &outputfile, argi))
            outfile_pattern = arg.val;
        else if (arg_match(&arg, &use_yv12, argi))
        {
            use_y4m = 0;
            flipuv = 1;
            opt_yv12 = 1;
        }
        else if (arg_match(&arg, &use_i420, argi))
        {
            use_y4m = 0;
            flipuv = 0;
            opt_i420 = 1;
        }
        else if (arg_match(&arg, &rawvideo, argi))
        {
            use_y4m = 0;
        }
        else if (arg_match(&arg, &flipuvarg, argi))
            flipuv = 1;
        else if (arg_match(&arg, &noblitarg, argi))
            noblit = 1;
        else if (arg_match(&arg, &progressarg, argi))
            progress = 1;
        else if (arg_match(&arg, &limitarg, argi))
            stop_after = arg_parse_uint(&arg);
        else if (arg_match(&arg, &skiparg, argi))
            arg_skip = arg_parse_uint(&arg);
        else if (arg_match(&arg, &postprocarg, argi))
            postproc = 1;
        else if (arg_match(&arg, &md5arg, argi))
            do_md5 = 1;
        else if (arg_match(&arg, &summaryarg, argi))
            summary = 1;
        else if (arg_match(&arg, &threadsarg, argi))
            cfg.threads = arg_parse_uint(&arg);
#if CONFIG_VP9_DECODER
        else if (arg_match(&arg, &frameparallelarg, argi))
        {
            /* ignored for compatibility */
        }
#endif
        else if (arg_match(&arg, &verbosearg, argi))
            quiet = 0;
        else if (arg_match(&arg, &scalearg, argi))
            do_scale = 1;
        else if (arg_match(&arg, &fb_arg, argi))
            num_external_frame_buffers = arg_parse_uint(&arg);
        else if (arg_match(&arg, &continuearg, argi))
            keep_going = 1;
#if CONFIG_VP9_HIGHBITDEPTH
    else if (arg_match(&arg, &outbitdeptharg, argi)) {
      output_bit_depth = arg_parse_uint(&arg);
    }
#endif
        else if (arg_match(&arg, &svcdecodingarg, argi))
        {
            svc_decoding = 1;
            svc_spatial_layer = arg_parse_uint(&arg);
        }
        else if (arg_match(&arg, &framestatsarg, argi))
        {
            framestats_file = fopen(arg.val, "w");
            if (!framestats_file)
            {
                die("Error: Could not open --framestats file (%s) for writing.\n", arg.val);
            }
        }
#if CONFIG_VP8_DECODER
    else if (arg_match(&arg, &addnoise_level, argi)) {
      postproc = 1;
      vp8_pp_cfg.post_proc_flag |= VP8_ADDNOISE;
      vp8_pp_cfg.noise_level = arg_parse_uint(&arg);
    } else if (arg_match(&arg, &demacroblock_level, argi)) {
      postproc = 1;
      vp8_pp_cfg.post_proc_flag |= VP8_DEMACROBLOCK;
      vp8_pp_cfg.deblocking_level = arg_parse_uint(&arg);
    } else if (arg_match(&arg, &deblock, argi)) {
      postproc = 1;
      vp8_pp_cfg.post_proc_flag |= VP8_DEBLOCK;
    } else if (arg_match(&arg, &mfqe, argi)) {
      postproc = 1;
      vp8_pp_cfg.post_proc_flag |= VP8_MFQE;
    } else if (arg_match(&arg, &error_concealment, argi)) {
      ec_enabled = 1;
    }
#endif  // CONFIG_VP8_DECODER
        /* NEMO: new arguments */
        else if (arg_match(&arg, &datasetdirarg, argi))
            dataset_dir = arg.val;
        else if (arg_match(&arg, &inputvideonamearg, argi))
            input_video_name = arg.val;
        else if (arg_match(&arg, &referencevideonamearg, argi))
            reference_video_name = arg.val;
        else if (arg_match(&arg, &outputwidtharg, argi))
            nemo_cfg->target_width = atoi(arg.val);
        else if (arg_match(&arg, &outputheightarg, argi))
            nemo_cfg->target_height = atoi(arg.val);
        else if (arg_match(&arg, &dnnscalearg, argi))
            dnn_scale = atoi(arg.val);
        else if (arg_match(&arg, &dnnnamearg, argi))
            dnn_name = arg.val;
        else if (arg_match(&arg, &decodemodearg, argi))
            set_decode_mode(&nemo_cfg->decode_mode, arg.val);
        else if (arg_match(&arg, &dnnmodearg, argi))
            set_dnn_mode(&nemo_cfg->dnn_mode, arg.val);
        else if (arg_match(&arg, &cachemodearg, argi))
            set_cache_mode(&nemo_cfg->cache_mode, arg.val);
        else if (arg_match(&arg, &cacheprofilenamearg, argi))
            cache_profile_name = arg.val;
        else if (arg_match(&arg, &savergbframedarg, argi))
            nemo_cfg->save_rgbframe = 1;
        else if (arg_match(&arg, &savequalityarg, argi))
            nemo_cfg->save_quality = 1;
        else if (arg_match(&arg, &savelatencyarg, argi))
            nemo_cfg->save_latency = 1;
        else if (arg_match(&arg, &savemetadataarg, argi))
            nemo_cfg->save_metadata = 1;
        else if (arg_match(&arg, &saveyuvframearg, argi))
            nemo_cfg->save_yuvframe = 1;
        else if (arg_match(&arg, &postfixarg, argi))
            postfix = arg.val;
        else if (arg_match(&arg, &dnnruntimearg, argi))
            set_runtime(&nemo_cfg->dnn_runtime, arg.val);
        else
            argj++;
    }

    /* Check for unrecognized options */
    for (argi = argv; *argi; argi++)
        if (argi[0][0] == '-' && strlen(argi[0]) > 1)
            die("Error: Unrecognized option %s\n", *argi);

    /* NEMO: setup */
    setup_directory(nemo_cfg, dataset_dir, input_video_name, reference_video_name, dnn_name, cache_profile_name, postfix);
    sprintf(video_path, "%s/video/%s", dataset_dir, input_video_name);
    if (nemo_cfg->decode_mode == DECODE_SR || nemo_cfg->decode_mode == DECODE_CACHE) {
        sprintf(dnn_path, "%s/checkpoint/%s/%s.dlc", dataset_dir, input_video_name, dnn_name);
    }
    if (nemo_cfg->decode_mode == DECODE_CACHE && nemo_cfg->cache_mode == PROFILE_CACHE) {
        sprintf(cache_profile_path, "%s/profile/%s/%s/%s/%s.profile", dataset_dir, input_video_name, dnn_name, postfix, cache_profile_name);
    }

    /* Open a video file */
    sprintf(fn, "%s", video_path);
    infile = fopen(fn, "rb");
    if (!infile)
    {
        fatal("Failed to open input file '%s'", fn);
    }

#if CONFIG_OS_SUPPORT
    /* Make sure we don't dump to the terminal, unless forced to with -o - */
    if (!outfile_pattern && isatty(fileno(stdout)) && !do_md5 && !noblit)
    {
        fprintf(stderr, "Not dumping raw video to your terminal. Use '-o -' to "
                "override.\n");
        return EXIT_FAILURE;
    }
#endif
    input.vpx_input_ctx->file = infile;
    if (file_is_ivf(input.vpx_input_ctx))
        input.vpx_input_ctx->file_type = FILE_TYPE_IVF;
#if CONFIG_WEBM_IO
    else if (file_is_webm(input.webm_ctx, input.vpx_input_ctx))
        input.vpx_input_ctx->file_type = FILE_TYPE_WEBM;
#endif
    else if (file_is_raw(input.vpx_input_ctx))
        input.vpx_input_ctx->file_type = FILE_TYPE_RAW;
    else
    {
        fprintf(stderr, "Unrecognized input file type.\n");
#if !CONFIG_WEBM_IO
    fprintf(stderr, "vpxdec was built without WebM container support.\n");
#endif
        return EXIT_FAILURE;
    }

    outfile_pattern = outfile_pattern ? outfile_pattern : "-";
    single_file = is_single_file(outfile_pattern);

    if (!noblit && single_file)
    {
        generate_filename(outfile_pattern, outfile_name, PATH_MAX, vpx_input_ctx.width, vpx_input_ctx.height, 0);
        if (do_md5)
            MD5Init(&md5_ctx);
        else
            outfile = open_outfile(outfile_name);
    }

    if (use_y4m && !noblit)
    {
        if (!single_file)
        {
            fprintf(stderr, "YUV4MPEG2 not supported with output patterns,"
                    " try --i420 or --yv12 or --rawvideo.\n");
            return EXIT_FAILURE;
        }

#if CONFIG_WEBM_IO
        if (vpx_input_ctx.file_type == FILE_TYPE_WEBM)
        {
            if (webm_guess_framerate(input.webm_ctx, input.vpx_input_ctx))
            {
                fprintf(stderr, "Failed to guess framerate -- error parsing "
                        "webm file?\n");
                return EXIT_FAILURE;
            }
        }
#endif
    }

    fourcc_interface = get_vpx_decoder_by_fourcc(vpx_input_ctx.fourcc);
    if (interface && fourcc_interface && interface != fourcc_interface)
        warn("Header indicates codec: %s\n", fourcc_interface->name);
    else
        interface = fourcc_interface;

    if (!interface)
        interface = get_vpx_decoder_by_index(0);

    dec_flags = (postproc ? VPX_CODEC_USE_POSTPROC : 0) | (ec_enabled ? VPX_CODEC_USE_ERROR_CONCEALMENT : 0);
    if (vpx_codec_dec_init(&decoder, interface->codec_interface(), &cfg, dec_flags))
    {
        fprintf(stderr, "Failed to initialize decoder: %s\n", vpx_codec_error(&decoder));
        goto fail2;
    }
    if (svc_decoding)
    {
        if (vpx_codec_control(&decoder, VP9_DECODE_SVC_SPATIAL_LAYER, svc_spatial_layer))
        {
            fprintf(stderr, "Failed to set spatial layer for svc decode: %s\n", vpx_codec_error(&decoder));
            goto fail;
        }
    }
    if (!quiet)
        fprintf(stderr, "%s\n", decoder.name);

#if CONFIG_VP8_DECODER
  if (vp8_pp_cfg.post_proc_flag &&
      vpx_codec_control(&decoder, VP8_SET_POSTPROC, &vp8_pp_cfg)) {
    fprintf(stderr, "Failed to configure postproc: %s\n",
            vpx_codec_error(&decoder));
    goto fail;
  }
#endif

    if (arg_skip)
        fprintf(stdout, "Skipping first %d frames.\n", arg_skip);
    while (arg_skip)
    {
        if (read_frame(&input, &buf, &bytes_in_buffer, &buffer_size))
            break;
        arg_skip--;
    }

    if (num_external_frame_buffers > 0)
    {
        ext_fb_list.num_external_frame_buffers = num_external_frame_buffers;
        ext_fb_list.ext_fb = (struct ExternalFrameBuffer*) calloc(num_external_frame_buffers,
                                                                  sizeof(*ext_fb_list.ext_fb));
        if (vpx_codec_set_frame_buffer_functions(&decoder, get_vp9_frame_buffer, release_vp9_frame_buffer,
                                                 &ext_fb_list))
        {
            fprintf(stderr, "Failed to configure external frame buffers: %s\n", vpx_codec_error(&decoder));
            goto fail;
        }
    }

    frame_avail = 1;
    got_data = 0;

    if (framestats_file)
        fprintf(framestats_file, "bytes,qp\n");

    /* NEMO: Load NEMO configuration */
    if (vpx_load_nemo_cfg(&decoder, nemo_cfg)){
         warn("Failed to load configuration: %s\n", vpx_codec_error(&decoder));
         goto fail;
     };

    /* NEMO: Load a DNN */
    if (nemo_cfg->dnn_mode == ONLINE_DNN && (nemo_cfg->dnn_mode == DECODE_SR || nemo_cfg->dnn_mode == DECODE_CACHE)) {
        if(vpx_load_nemo_dnn(&decoder, dnn_scale, dnn_path)){
            warn("Failed to load a DNN: %s\n", vpx_codec_error(&decoder));
            goto fail;
        }
    }
    if (nemo_cfg->dnn_mode == OFFLINE_DNN && (nemo_cfg->dnn_mode == DECODE_SR || nemo_cfg->dnn_mode == DECODE_CACHE)) {
        if(vpx_load_nemo_dnn(&decoder, dnn_scale, NULL)){
            warn("Failed to load a DNN: %s\n", vpx_codec_error(&decoder));
            goto fail;
        }
    }

    /* NEMO: Load a cache profile */
    if (nemo_cfg->cache_mode == PROFILE_CACHE){
        if(vpx_load_nemo_cache_profile(&decoder, dnn_scale, cache_profile_path)){
            warn("Failed to load a cache profile: %s\n", vpx_codec_error(&decoder));
            goto fail;
        }
    }
    
    /* Decode file */
    while (frame_avail || got_data)
    {
        vpx_codec_iter_t iter = NULL;
        vpx_image_t *img;
        struct vpx_usec_timer timer;
        int corrupted = 0;

        frame_avail = 0;
        if (!stop_after || frame_in < stop_after)
        {
            if (!read_frame(&input, &buf, &bytes_in_buffer, &buffer_size))
            {
                frame_avail = 1;
                frame_in++;

                vpx_usec_timer_start(&timer);

                if (vpx_codec_decode(&decoder, buf, (unsigned int) bytes_in_buffer, NULL, 0))
                {
                    const char *detail = vpx_codec_error_detail(&decoder);
                    warn("Failed to decode frame %d: %s", frame_in, vpx_codec_error(&decoder));
                    if (detail)
                        warn("Additional information: %s", detail);
                    corrupted = 1;
                    if (!keep_going)
                        goto fail;
                }

                if (framestats_file)
                {
                    int qp;
                    if (vpx_codec_control(&decoder, VPXD_GET_LAST_QUANTIZER, &qp))
                    {
                        warn("Failed VPXD_GET_LAST_QUANTIZER: %s", vpx_codec_error(&decoder));
                        if (!keep_going)
                            goto fail;
                    }
                    fprintf(framestats_file, "%d,%d\n", (int) bytes_in_buffer, qp);
                }

                vpx_usec_timer_mark(&timer);
                dx_time += vpx_usec_timer_elapsed(&timer);
            }
            else
            {
                flush_decoder = 1;
            }
        }
        else
        {
            flush_decoder = 1;
        }

        vpx_usec_timer_start(&timer);

        if (flush_decoder)
        {
            // Flush the decoder in frame parallel decode.
            if (vpx_codec_decode(&decoder, NULL, 0, NULL, 0))
            {
                warn("Failed to flush decoder: %s", vpx_codec_error(&decoder));
                corrupted = 1;
                if (!keep_going)
                    goto fail;
            }
        }

        got_data = 0;
        if ((img = vpx_codec_get_frame(&decoder, &iter)))
        {
            ++frame_out;
            got_data = 1;
        }

        vpx_usec_timer_mark(&timer);
        dx_time += (unsigned int) vpx_usec_timer_elapsed(&timer);

        if (!corrupted && vpx_codec_control(&decoder, VP8D_GET_FRAME_CORRUPTED, &corrupted))
        {
            warn("Failed VP8_GET_FRAME_CORRUPTED: %s", vpx_codec_error(&decoder));
            if (!keep_going)
                goto fail;
        }
        frames_corrupted += corrupted;

        if (progress)
            show_progress(frame_in, frame_out, dx_time);

        if (!noblit && img)
        {
            const int PLANES_YUV[] =
            { VPX_PLANE_Y, VPX_PLANE_U, VPX_PLANE_V };
            const int PLANES_YVU[] =
            { VPX_PLANE_Y, VPX_PLANE_V, VPX_PLANE_U };
            const int *planes = flipuv ? PLANES_YVU : PLANES_YUV;

            if (do_scale)
            {
                if (frame_out == 1)
                {
                    // If the output frames are to be scaled to a fixed display size then
                    // use the width and height specified in the container. If either of
                    // these is set to 0, use the display size set in the first frame
                    // header. If that is unavailable, use the raw decoded size of the
                    // first decoded frame.
                    int render_width = vpx_input_ctx.width;
                    int render_height = vpx_input_ctx.height;
                    if (!render_width || !render_height)
                    {
                        int render_size[2];
                        if (vpx_codec_control(&decoder, VP9D_GET_DISPLAY_SIZE, render_size))
                        {
                            // As last resort use size of first frame as display size.
                            render_width = img->d_w;
                            render_height = img->d_h;
                        }
                        else
                        {
                            render_width = render_size[0];
                            render_height = render_size[1];
                        }
                    }
                    scaled_img = vpx_img_alloc(NULL, img->fmt, render_width, render_height, 16);
                    scaled_img->bit_depth = img->bit_depth;
                }

                if (img->d_w != scaled_img->d_w || img->d_h != scaled_img->d_h)
                {
#if CONFIG_LIBYUV
                    libyuv_scale(img, scaled_img, kFilterBox);
                    img = scaled_img;
#else
          fprintf(stderr,
                  "Failed  to scale output frame: %s.\n"
                  "Scaling is disabled in this configuration. "
                  "To enable scaling, configure with --enable-libyuv\n",
                  vpx_codec_error(&decoder));
          goto fail;
#endif
                }
            }
#if CONFIG_VP9_HIGHBITDEPTH
      // Default to codec bit depth if output bit depth not set
      if (!output_bit_depth && single_file && !do_md5) {
        output_bit_depth = img->bit_depth;
      }
      // Shift up or down if necessary
      if (output_bit_depth != 0 && output_bit_depth != img->bit_depth) {
        const vpx_img_fmt_t shifted_fmt =
            output_bit_depth == 8
                ? img->fmt ^ (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH)
                : img->fmt | VPX_IMG_FMT_HIGHBITDEPTH;
        if (img_shifted &&
            img_shifted_realloc_required(img, img_shifted, shifted_fmt)) {
          vpx_img_free(img_shifted);
          img_shifted = NULL;
        }
        if (!img_shifted) {
          img_shifted =
              vpx_img_alloc(NULL, shifted_fmt, img->d_w, img->d_h, 16);
          img_shifted->bit_depth = output_bit_depth;
        }
        if (output_bit_depth > img->bit_depth) {
          vpx_img_upshift(img_shifted, img, output_bit_depth - img->bit_depth);
        } else {
          vpx_img_downshift(img_shifted, img,
                            img->bit_depth - output_bit_depth);
        }
        img = img_shifted;
      }
#endif

            if (single_file)
            {
                if (use_y4m)
                {
                    char buf[Y4M_BUFFER_SIZE] =
                    { 0 };
                    size_t len = 0;
                    if (img->fmt == VPX_IMG_FMT_I440 || img->fmt == VPX_IMG_FMT_I44016)
                    {
                        fprintf(stderr, "Cannot produce y4m output for 440 sampling.\n");
                        goto fail;
                    }
                    if (frame_out == 1)
                    {
                        // Y4M file header
                        len = y4m_write_file_header(buf, sizeof(buf), vpx_input_ctx.width, vpx_input_ctx.height,
                                                    &vpx_input_ctx.framerate, img->fmt, img->bit_depth);
                        if (do_md5)
                        {
                            MD5Update(&md5_ctx, (md5byte*) buf, (unsigned int) len);
                        }
                        else
                        {
                            fputs(buf, outfile);
                        }
                    }

                    // Y4M frame header
                    len = y4m_write_frame_header(buf, sizeof(buf));
                    if (do_md5)
                    {
                        MD5Update(&md5_ctx, (md5byte*) buf, (unsigned int) len);
                    }
                    else
                    {
                        fputs(buf, outfile);
                    }
                }
                else
                {
                    if (frame_out == 1)
                    {
                        // Check if --yv12 or --i420 options are consistent with the
                        // bit-stream decoded
                        if (opt_i420)
                        {
                            if (img->fmt != VPX_IMG_FMT_I420 && img->fmt != VPX_IMG_FMT_I42016)
                            {
                                fprintf(stderr, "Cannot produce i420 output for bit-stream.\n");
                                goto fail;
                            }
                        }
                        if (opt_yv12)
                        {
                            if ((img->fmt != VPX_IMG_FMT_I420 && img->fmt != VPX_IMG_FMT_YV12) || img->bit_depth != 8)
                            {
                                fprintf(stderr, "Cannot produce yv12 output for bit-stream.\n");
                                goto fail;
                            }
                        }
                    }
                }

                if (do_md5)
                {
                    update_image_md5(img, planes, &md5_ctx);
                }
                else
                {
                    if (!corrupted)
                        write_image_file(img, planes, outfile);
                }
            }
            else
            {
                generate_filename(outfile_pattern, outfile_name, PATH_MAX, img->d_w, img->d_h, frame_in);
                if (do_md5)
                {
                    MD5Init(&md5_ctx);
                    update_image_md5(img, planes, &md5_ctx);
                    MD5Final(md5_digest, &md5_ctx);
                    print_md5(md5_digest, outfile_name);
                }
                else
                {
                    outfile = open_outfile(outfile_name);
                    write_image_file(img, planes, outfile);
                    fclose(outfile);
                }
            }
        }
    }

    if (summary || progress)
    {
        show_progress(frame_in, frame_out, dx_time);
        fprintf(stderr, "\n");
    }

    if (frames_corrupted)
    {
        fprintf(stderr, "WARNING: %d frames corrupted.\n", frames_corrupted);
    }
    else
    {
        ret = EXIT_SUCCESS;
    }

    fail:

    if (vpx_codec_destroy(&decoder))
    {
        fprintf(stderr, "Failed to destroy decoder: %s\n", vpx_codec_error(&decoder));
    }

    fail2:

    if (!noblit && single_file)
    {
        if (do_md5)
        {
            MD5Final(md5_digest, &md5_ctx);
            print_md5(md5_digest, outfile_name);
        }
        else
        {
            fclose(outfile);
        }
    }

    remove_nemo_cfg(nemo_cfg);

#if CONFIG_WEBM_IO
    if (input.vpx_input_ctx->file_type == FILE_TYPE_WEBM)
        webm_free(input.webm_ctx);
#endif

    if (input.vpx_input_ctx->file_type != FILE_TYPE_WEBM)
        free(buf);

    if (scaled_img)
        vpx_img_free(scaled_img);
#if CONFIG_VP9_HIGHBITDEPTH
  if (img_shifted) vpx_img_free(img_shifted);
#endif

    for (i = 0; i < ext_fb_list.num_external_frame_buffers; ++i)
    {
        free(ext_fb_list.ext_fb[i].data);
    }
    free(ext_fb_list.ext_fb);

    fclose(infile);
    if (framestats_file)
        fclose(framestats_file);

    free(argv);

    return ret;
}

int main(int argc, const char **argv_)
{
    unsigned int loops = 1, i;
    char **argv, **argi, **argj;
    struct arg arg;
    int error = 0;

    argv = argv_dup(argc - 1, argv_ + 1);
    for (argi = argj = argv; (*argj = *argi); argi += arg.argv_step)
    {
        memset(&arg, 0, sizeof(arg));
        arg.argv_step = 1;

        if (arg_match(&arg, &looparg, argi))
        {
            loops = arg_parse_uint(&arg);
            break;
        }
    }
    free(argv);
    for (i = 0; !error && i < loops; i++)
        error = main_loop(argc, argv_);
    return error;
}
