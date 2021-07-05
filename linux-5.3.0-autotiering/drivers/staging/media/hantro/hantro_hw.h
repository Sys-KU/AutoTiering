/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hantro VPU codec driver
 *
 * Copyright 2018 Google LLC.
 *	Tomasz Figa <tfiga@chromium.org>
 */

#ifndef HANTRO_HW_H_
#define HANTRO_HW_H_

#include <linux/interrupt.h>
#include <linux/v4l2-controls.h>
#include <media/mpeg2-ctrls.h>
#include <media/videobuf2-core.h>

struct hantro_dev;
struct hantro_ctx;
struct hantro_buf;
struct hantro_variant;

/**
 * struct hantro_aux_buf - auxiliary DMA buffer for hardware data
 * @cpu:	CPU pointer to the buffer.
 * @dma:	DMA address of the buffer.
 * @size:	Size of the buffer.
 */
struct hantro_aux_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

/**
 * struct hantro_jpeg_enc_hw_ctx
 * @bounce_buffer:	Bounce buffer
 */
struct hantro_jpeg_enc_hw_ctx {
	struct hantro_aux_buf bounce_buffer;
};

/**
 * struct hantro_mpeg2_dec_hw_ctx
 * @qtable:		Quantization table
 */
struct hantro_mpeg2_dec_hw_ctx {
	struct hantro_aux_buf qtable;
};

/**
 * struct hantro_codec_ops - codec mode specific operations
 *
 * @init:	If needed, can be used for initialization.
 *		Optional and called from process context.
 * @exit:	If needed, can be used to undo the .init phase.
 *		Optional and called from process context.
 * @run:	Start single {en,de)coding job. Called from atomic context
 *		to indicate that a pair of buffers is ready and the hardware
 *		should be programmed and started.
 * @done:	Read back processing results and additional data from hardware.
 * @reset:	Reset the hardware in case of a timeout.
 */
struct hantro_codec_ops {
	int (*init)(struct hantro_ctx *ctx);
	void (*exit)(struct hantro_ctx *ctx);
	void (*run)(struct hantro_ctx *ctx);
	void (*done)(struct hantro_ctx *ctx, enum vb2_buffer_state);
	void (*reset)(struct hantro_ctx *ctx);
};

/**
 * enum hantro_enc_fmt - source format ID for hardware registers.
 */
enum hantro_enc_fmt {
	RK3288_VPU_ENC_FMT_YUV420P = 0,
	RK3288_VPU_ENC_FMT_YUV420SP = 1,
	RK3288_VPU_ENC_FMT_YUYV422 = 2,
	RK3288_VPU_ENC_FMT_UYVY422 = 3,
};

extern const struct hantro_variant rk3399_vpu_variant;
extern const struct hantro_variant rk3328_vpu_variant;
extern const struct hantro_variant rk3288_vpu_variant;

void hantro_watchdog(struct work_struct *work);
void hantro_run(struct hantro_ctx *ctx);
void hantro_irq_done(struct hantro_dev *vpu, unsigned int bytesused,
		     enum vb2_buffer_state result);

void hantro_h1_jpeg_enc_run(struct hantro_ctx *ctx);
void rk3399_vpu_jpeg_enc_run(struct hantro_ctx *ctx);
int hantro_jpeg_enc_init(struct hantro_ctx *ctx);
void hantro_jpeg_enc_exit(struct hantro_ctx *ctx);

void hantro_g1_mpeg2_dec_run(struct hantro_ctx *ctx);
void rk3399_vpu_mpeg2_dec_run(struct hantro_ctx *ctx);
void hantro_mpeg2_dec_copy_qtable(u8 *qtable,
	const struct v4l2_ctrl_mpeg2_quantization *ctrl);
int hantro_mpeg2_dec_init(struct hantro_ctx *ctx);
void hantro_mpeg2_dec_exit(struct hantro_ctx *ctx);

#endif /* HANTRO_HW_H_ */
