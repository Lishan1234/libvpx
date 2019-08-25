//
// Created by hyunho on 7/24/19.
//

#include <stdio.h>
#include <math.h>
#include <android/log.h>
#include <assert.h>
#include "vpx_dsp/vpx_bilinear.h"
#include "vpx_dsp/vpx_dsp_common.h"
#include "vpx_dsp_common.h"
#include "../vp9/common/vp9_onyxc_int.h"

#define TAG "vpx_bilinear.c JNI"
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

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define FRACTION_BIT (5)
#define FRACTION_SCALE (1 << FRACTION_BIT)
static const int16_t delta = (1 << (FRACTION_BIT - 1));

static void vpx_bilinear_interp_horiz_uint8_c(const uint8_t *src, ptrdiff_t src_stride, int16_t *dst,  ptrdiff_t dst_stride, int width, int height, int scale, const bilinear_config_t *config){
    int x, y;

    for (y = 0; y < height; ++y) {
        for (x = 0; x < width * scale; x = x + 2) {
            const int left_x_index_0 = config->left_x_index[x];
            const int left_x_index_1 = config->left_x_index[x + 1];
            const int right_x_index_0 = config->right_x_index[x];
            const int right_x_index_1 = config->right_x_index[x + 1];

            const int16_t x_lerp_fixed_0 = config->x_lerp_fixed[x];
            const int16_t x_lerp_fixed_1 = config->x_lerp_fixed[x + 1];

            const int16_t left_0 = src[y * src_stride + left_x_index_0];
            const int16_t right_0 = src[y * src_stride + right_x_index_0];
            const int16_t left_1 = src[y * src_stride + left_x_index_1];
            const int16_t right_1 = src[y * src_stride + right_x_index_1];

            const int16_t result_0 = left_0 + (((right_0 - left_0) * x_lerp_fixed_0 + delta) >> FRACTION_BIT);
            const int16_t result_1 = left_1 + (((right_1 - left_1) * x_lerp_fixed_1 + delta) >> FRACTION_BIT);

            dst[y * dst_stride + x] = result_0;
            dst[y * dst_stride + (x + 1)] = result_1;
        }
    }
}

static void vpx_bilinear_interp_horiz_int16_c(const int16_t *src, ptrdiff_t src_stride, int16_t *dst,  ptrdiff_t dst_stride, int width, int height, int scale, const bilinear_config_t *config){
    int x, y;

    for (y = 0; y < height; ++y) {
        for (x = 0; x < width * scale; x = x + 2) {
            const int left_x_index_0 = config->left_x_index[x];
            const int left_x_index_1 = config->left_x_index[x + 1];
            const int right_x_index_0 = config->right_x_index[x];
            const int right_x_index_1 = config->right_x_index[x + 1];

            const int16_t x_lerp_fixed_0 = config->x_lerp_fixed[x];
            const int16_t x_lerp_fixed_1 = config->x_lerp_fixed[x + 1];

            const int16_t left_0 = src[y * src_stride + left_x_index_0];
            const int16_t right_0 = src[y * src_stride + right_x_index_0];
            const int16_t left_1 = src[y * src_stride + left_x_index_1];
            const int16_t right_1 = src[y * src_stride + right_x_index_1];

            const int16_t result_0 = left_0 + (((right_0 - left_0) * x_lerp_fixed_0 + delta) >> FRACTION_BIT);
            const int16_t result_1 = left_1 + (((right_1 - left_1) * x_lerp_fixed_1 + delta) >> FRACTION_BIT);

            dst[y * dst_stride + x] = result_0;
            dst[y * dst_stride + (x + 1)] = result_1;
        }
    }
}

static void vpx_bilinear_interp_vert_uint8_c(const int16_t *src, ptrdiff_t src_stride, uint8_t *dst,  ptrdiff_t dst_stride, int width, int height, int scale, const bilinear_config_t *config){
    int x, y;

    for (y = 0; y < height * scale; ++y) {
        const int top_y_index = config->top_y_index[y];
        const int bottom_y_index = config->bottom_y_index[y];
        const int16_t y_lerp_fixed = config->y_lerp_fixed[y];

        for (x = 0; x < width * scale; x = x + 2) {
            const int16_t top_0 = src[top_y_index * src_stride + x];
            const int16_t bottom_0 = src[bottom_y_index * src_stride + x];
            const int16_t top_1 = src[top_y_index * src_stride + (x + 1)];
            const int16_t bottom_1 = src[bottom_y_index * src_stride + (x + 1)];

            const int16_t result_0 = top_0 + (((bottom_0 - top_0) * y_lerp_fixed + delta) >> FRACTION_BIT);
            const int16_t result_1 = top_1 + (((bottom_1 - top_1) * y_lerp_fixed + delta) >> FRACTION_BIT);

            dst[y * dst_stride + x] = result_0;
            dst[y * dst_stride + (x + 1)] = result_1;
        }
    }
}

static void vpx_bilinear_interp_vert_int16_c(const int16_t *src, ptrdiff_t src_stride, uint8_t *dst,  ptrdiff_t dst_stride, int width, int height, int scale, const bilinear_config_t *config){
    int x, y;

    for (y = 0; y < height * scale; ++y) {
        const int top_y_index = config->top_y_index[y];
        const int bottom_y_index = config->bottom_y_index[y];
        const int16_t y_lerp_fixed = config->y_lerp_fixed[y];

        for (x = 0; x < width * scale; x = x + 2) {
            const int16_t top_0 = src[top_y_index * src_stride + x];
            const int16_t bottom_0 = src[bottom_y_index * src_stride + x];
            const int16_t top_1 = src[top_y_index * src_stride + (x + 1)];
            const int16_t bottom_1 = src[bottom_y_index * src_stride + (x + 1)];

            const int16_t result_0 = top_0 + (((bottom_0 - top_0) * y_lerp_fixed + delta) >> FRACTION_BIT);
            const int16_t result_1 = top_1 + (((bottom_1 - top_1) * y_lerp_fixed + delta) >> FRACTION_BIT);

            dst[y * dst_stride + x] = clip_pixel(dst[y * dst_stride + x] + result_0);
            dst[y * dst_stride + (x + 1)] = clip_pixel(dst[y * dst_stride + (x + 1)] + result_1);
        }
    }
}

void vpx_bilinear_interp_uint8_c(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst,  ptrdiff_t dst_stride, int x_offset, int y_offset, int width, int height, int scale, const bilinear_config_t *config){
    int16_t temp[128 * 128];

    assert(width <= 32);
    assert(height <= 32);
    assert(scale <= 4 && scale >= 2);

    src = src + (y_offset * src_stride + x_offset);
    dst = dst + (y_offset * dst_stride + x_offset) * scale;

    vpx_bilinear_interp_horiz_uint8_c(src, src_stride, temp, 128, width, height, scale, config);
    vpx_bilinear_interp_vert_uint8_c(temp, 128, dst, dst_stride, width, height, scale, config);
}

void vpx_bilinear_interp_int16_c(const int16_t *src, ptrdiff_t src_stride, uint8_t *dst,  ptrdiff_t dst_stride, int x_offset, int y_offset, int width, int height, int scale, const bilinear_config_t *config){
    int16_t temp[128 * 128];

    assert(width <= 32);
    assert(height <= 32);
    assert(scale <= 4 && scale >= 2);

    src = src + (y_offset * src_stride + x_offset);
    dst = dst + (y_offset * dst_stride + x_offset) * scale;

    vpx_bilinear_interp_horiz_int16_c(src, src_stride, temp, 128, width, height, scale, config);
    vpx_bilinear_interp_vert_int16_c(temp, 128, dst, dst_stride, width, height, scale, config);
}