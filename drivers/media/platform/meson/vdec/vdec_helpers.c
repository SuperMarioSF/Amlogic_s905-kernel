// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 BayLibre, SAS
 * Author: Maxime Jourdan <mjourdan@baylibre.com>
 */

#include <linux/gcd.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-dma-contig.h>

#include "vdec_helpers.h"

#define NUM_CANVAS_NV12 2
#define NUM_CANVAS_YUV420 3

u32 amvdec_read_dos(struct amvdec_core *core, u32 reg)
{
	return readl_relaxed(core->dos_base + reg);
}
EXPORT_SYMBOL_GPL(amvdec_read_dos);

void amvdec_write_dos(struct amvdec_core *core, u32 reg, u32 val)
{
	writel_relaxed(val, core->dos_base + reg);
}
EXPORT_SYMBOL_GPL(amvdec_write_dos);

void amvdec_write_dos_bits(struct amvdec_core *core, u32 reg, u32 val)
{
	amvdec_write_dos(core, reg, amvdec_read_dos(core, reg) | val);
}
EXPORT_SYMBOL_GPL(amvdec_write_dos_bits);

void amvdec_clear_dos_bits(struct amvdec_core *core, u32 reg, u32 val)
{
	amvdec_write_dos(core, reg, amvdec_read_dos(core, reg) & ~val);
}
EXPORT_SYMBOL_GPL(amvdec_clear_dos_bits);

u32 amvdec_read_parser(struct amvdec_core *core, u32 reg)
{
	return readl_relaxed(core->esparser_base + reg);
}
EXPORT_SYMBOL_GPL(amvdec_read_parser);

void amvdec_write_parser(struct amvdec_core *core, u32 reg, u32 val)
{
	writel_relaxed(val, core->esparser_base + reg);
}
EXPORT_SYMBOL_GPL(amvdec_write_parser);

/* 4 KiB per 64x32 block */
u32 amvdec_am21c_body_size(u32 width, u32 height)
{
	u32 width_64 = ALIGN(width, 64) / 64;
	u32 height_32 = ALIGN(height, 32) / 32;

	return SZ_4K * width_64 * height_32;
}
EXPORT_SYMBOL_GPL(amvdec_am21c_body_size);

/* 32 bytes per 128x64 block */
u32 amvdec_am21c_head_size(u32 width, u32 height)
{
	u32 width_128 = ALIGN(width, 128) / 128;
	u32 height_64 = ALIGN(height, 64) / 64;

	return 32 * width_128 * height_64;
}
EXPORT_SYMBOL_GPL(amvdec_am21c_head_size);

u32 amvdec_am21c_size(u32 width, u32 height)
{
	return ALIGN(amvdec_am21c_body_size(width, height) +
		     amvdec_am21c_head_size(width, height), SZ_64K);
}
EXPORT_SYMBOL_GPL(amvdec_am21c_size);

static int canvas_alloc(struct amvdec_session *sess, u8 *canvas_id)
{
	int ret;

	if (sess->canvas_num >= MAX_CANVAS) {
		dev_err(sess->core->dev, "Reached max number of canvas\n");
		return -ENOMEM;
	}

	ret = meson_canvas_alloc(sess->core->canvas, canvas_id);
	if (ret)
		return ret;

	sess->canvas_alloc[sess->canvas_num++] = *canvas_id;
	return 0;
}

static int set_canvas_yuv420m(struct amvdec_session *sess,
			      struct vb2_buffer *vb, u32 width,
			      u32 height, u32 reg)
{
	struct amvdec_core *core = sess->core;
	u8 canvas_id[NUM_CANVAS_YUV420]; /* Y U V */
	dma_addr_t buf_paddr[NUM_CANVAS_YUV420]; /* Y U V */
	int ret, i;

	for (i = 0; i < NUM_CANVAS_YUV420; ++i) {
		ret = canvas_alloc(sess, &canvas_id[i]);
		if (ret)
			return ret;

		buf_paddr[i] =
		    vb2_dma_contig_plane_dma_addr(vb, i);
	}

	/* Y plane */
	meson_canvas_config(core->canvas, canvas_id[0], buf_paddr[0],
			    width, height, MESON_CANVAS_WRAP_NONE,
			    MESON_CANVAS_BLKMODE_LINEAR,
			    MESON_CANVAS_ENDIAN_SWAP64);

	/* U plane */
	meson_canvas_config(core->canvas, canvas_id[1], buf_paddr[1],
			    width / 2, height / 2, MESON_CANVAS_WRAP_NONE,
			    MESON_CANVAS_BLKMODE_LINEAR,
			    MESON_CANVAS_ENDIAN_SWAP64);

	/* V plane */
	meson_canvas_config(core->canvas, canvas_id[2], buf_paddr[2],
			    width / 2, height / 2, MESON_CANVAS_WRAP_NONE,
			    MESON_CANVAS_BLKMODE_LINEAR,
			    MESON_CANVAS_ENDIAN_SWAP64);

	amvdec_write_dos(core, reg,
			 ((canvas_id[2]) << 16) |
			 ((canvas_id[1]) << 8)  |
			 (canvas_id[0]));

	return 0;
}

static int set_canvas_nv12m(struct amvdec_session *sess,
			    struct vb2_buffer *vb, u32 width,
			    u32 height, u32 reg)
{
	struct amvdec_core *core = sess->core;
	u8 canvas_id[NUM_CANVAS_NV12]; /* Y U/V */
	dma_addr_t buf_paddr[NUM_CANVAS_NV12]; /* Y U/V */
	int ret, i;

	for (i = 0; i < NUM_CANVAS_NV12; ++i) {
		ret = canvas_alloc(sess, &canvas_id[i]);
		if (ret)
			return ret;

		buf_paddr[i] =
		    vb2_dma_contig_plane_dma_addr(vb, i);
	}

	/* Y plane */
	meson_canvas_config(core->canvas, canvas_id[0], buf_paddr[0],
			    width, height, MESON_CANVAS_WRAP_NONE,
			    MESON_CANVAS_BLKMODE_LINEAR,
			    MESON_CANVAS_ENDIAN_SWAP64);

	/* U/V plane */
	meson_canvas_config(core->canvas, canvas_id[1], buf_paddr[1],
			    width, height / 2, MESON_CANVAS_WRAP_NONE,
			    MESON_CANVAS_BLKMODE_LINEAR,
			    MESON_CANVAS_ENDIAN_SWAP64);

	amvdec_write_dos(core, reg,
			 ((canvas_id[1]) << 16) |
			 ((canvas_id[1]) << 8)  |
			 (canvas_id[0]));

	return 0;
}

int amvdec_set_canvases(struct amvdec_session *sess,
			u32 reg_base[], u32 reg_num[])
{
	struct v4l2_m2m_buffer *buf;
	u32 pixfmt = sess->pixfmt_cap;
	u32 width = ALIGN(sess->width, 64);
	u32 height = ALIGN(sess->height, 64);
	u32 reg_cur = reg_base[0];
	u32 reg_num_cur = 0;
	u32 reg_base_cur = 0;
	int ret;

	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		if (!reg_base[reg_base_cur])
			return -EINVAL;

		reg_cur = reg_base[reg_base_cur] + reg_num_cur * 4;

		switch (pixfmt) {
		case V4L2_PIX_FMT_NV12M:
			ret = set_canvas_nv12m(sess, &buf->vb.vb2_buf, width,
					       height, reg_cur);
			if (ret)
				return ret;
			break;
		case V4L2_PIX_FMT_YUV420M:
			ret = set_canvas_yuv420m(sess, &buf->vb.vb2_buf, width,
						 height, reg_cur);
			if (ret)
				return ret;
			break;
		default:
			dev_err(sess->core->dev, "Unsupported pixfmt %08X\n",
				pixfmt);
			return -EINVAL;
		};

		reg_num_cur++;
		if (reg_num_cur >= reg_num[reg_base_cur]) {
			reg_base_cur++;
			reg_num_cur = 0;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(amvdec_set_canvases);

void amvdec_add_ts_reorder(struct amvdec_session *sess, u64 ts, s32 offset)
{
	struct amvdec_timestamp *new_ts, *tmp;
	unsigned long flags;

	new_ts = kmalloc(sizeof(*new_ts), GFP_KERNEL);
	new_ts->ts = ts;
	new_ts->offset = offset;

	spin_lock_irqsave(&sess->ts_spinlock, flags);

	if (list_empty(&sess->timestamps))
		goto add_tail;

	list_for_each_entry(tmp, &sess->timestamps, list) {
		if (ts <= tmp->ts) {
			list_add_tail(&new_ts->list, &tmp->list);
			goto unlock;
		}
	}

add_tail:
	list_add_tail(&new_ts->list, &sess->timestamps);
unlock:
	spin_unlock_irqrestore(&sess->ts_spinlock, flags);
}
EXPORT_SYMBOL_GPL(amvdec_add_ts_reorder);

static void amvdec_rm_first_ts(struct amvdec_session *sess)
{
	unsigned long flags;
	struct amvdec_buffer *tmp;
	struct device *dev = sess->core->dev_dec;

	spin_lock_irqsave(&sess->ts_spinlock, flags);
	if (list_empty(&sess->timestamps)) {
		dev_err(dev, "Can't rm first timestamp: list empty\n");
		goto unlock;
	}

	tmp = list_first_entry(&sess->timestamps, struct amvdec_buffer, list);
	list_del(&tmp->list);
	kfree(tmp);
	atomic_dec(&sess->esparser_queued_bufs);

unlock:
	spin_unlock_irqrestore(&sess->ts_spinlock, flags);
}

void amvdec_remove_ts(struct amvdec_session *sess, u64 ts)
{
	struct amvdec_timestamp *tmp;
	unsigned long flags;

	spin_lock_irqsave(&sess->ts_spinlock, flags);
	list_for_each_entry(tmp, &sess->timestamps, list) {
		if (tmp->ts == ts) {
			list_del(&tmp->list);
			kfree(tmp);
			goto unlock;
		}
	}
	dev_warn(sess->core->dev_dec,
		 "Couldn't remove buffer with timestamp %llu from list\n", ts);

unlock:
	spin_unlock_irqrestore(&sess->ts_spinlock, flags);
}
EXPORT_SYMBOL_GPL(amvdec_remove_ts);

static void dst_buf_done(struct amvdec_session *sess,
			 struct vb2_v4l2_buffer *vbuf,
			 u32 field,
			 u64 timestamp)
{
	struct device *dev = sess->core->dev_dec;
	u32 output_size = amvdec_get_output_size(sess);

	switch (sess->pixfmt_cap) {
	case V4L2_PIX_FMT_NV12M:
		vbuf->vb2_buf.planes[0].bytesused = output_size;
		vbuf->vb2_buf.planes[1].bytesused = output_size / 2;
		break;
	case V4L2_PIX_FMT_YUV420M:
		vbuf->vb2_buf.planes[0].bytesused = output_size;
		vbuf->vb2_buf.planes[1].bytesused = output_size / 4;
		vbuf->vb2_buf.planes[2].bytesused = output_size / 4;
		break;
	case V4L2_PIX_FMT_AM21C:
		vbuf->vb2_buf.planes[0].bytesused =
			amvdec_am21c_size(sess->width, sess->height);
		break;
	}

	vbuf->vb2_buf.timestamp = timestamp;
	vbuf->sequence = sess->sequence_cap++;

	atomic_dec(&sess->esparser_queued_bufs);

	if (sess->should_stop && list_empty(&sess->timestamps)) {
		const struct v4l2_event ev = { .type = V4L2_EVENT_EOS };

		dev_dbg(dev, "Signaling EOS\n");
		v4l2_event_queue_fh(&sess->fh, &ev);
		vbuf->flags |= V4L2_BUF_FLAG_LAST;
	} else if (sess->should_stop)
		dev_dbg(dev, "should_stop, %u bufs remain\n",
			atomic_read(&sess->esparser_queued_bufs));

	dev_dbg(dev, "Buffer %u done\n", vbuf->vb2_buf.index);
	vbuf->field = field;
	v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_DONE);

	/* Buffer done probably means the vififo got freed */
	schedule_work(&sess->esparser_queue_work);
}

void amvdec_dst_buf_done(struct amvdec_session *sess,
			 struct vb2_v4l2_buffer *vbuf, u32 field)
{
	struct device *dev = sess->core->dev_dec;
	struct amvdec_timestamp *tmp;
	struct list_head *timestamps = &sess->timestamps;
	u64 timestamp;
	unsigned long flags;

	spin_lock_irqsave(&sess->ts_spinlock, flags);
	if (list_empty(timestamps)) {
		dev_err(dev, "Buffer %u done but list is empty\n",
			vbuf->vb2_buf.index);

		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
		spin_unlock_irqrestore(&sess->ts_spinlock, flags);
		return;
	}

	tmp = list_first_entry(timestamps, struct amvdec_timestamp, list);
	timestamp = tmp->ts;
	list_del(&tmp->list);
	kfree(tmp);
	spin_unlock_irqrestore(&sess->ts_spinlock, flags);

	dst_buf_done(sess, vbuf, field, timestamp);
}
EXPORT_SYMBOL_GPL(amvdec_dst_buf_done);

static void amvdec_dst_buf_done_offset(struct amvdec_session *sess,
				       struct vb2_v4l2_buffer *vbuf,
				       s32 offset,
				       u32 field)
{
	struct device *dev = sess->core->dev_dec;
	struct amvdec_timestamp *match = NULL;
	struct amvdec_timestamp *tmp, *n;
	u64 timestamp;
	unsigned long flags;

	/* codec offsets do not wrap around the vififo size */
	offset %= sess->vififo_size;

	spin_lock_irqsave(&sess->ts_spinlock, flags);

	/* Look for our vififo offset to get the corresponding timestamp. */
	list_for_each_entry_safe(tmp, n, &sess->timestamps, list) {
		s32 delta = offset - tmp->offset;

		/* Offsets reported by codecs usually differ slightly,
		 * so we need some wiggle room.
		 * 4KiB being the minimum packet size, there is no risk here.
		 */
		if (delta > (-1 * (s32)SZ_4K) && delta < SZ_4K) {
			match = tmp;
			break;
		}

		/* Delete any timestamp entry that appears before our target
		 * (not all src packets/timestamps lead to a frame)
		 * Also handle the special case where the vififo wraps around,
		 * leading to a big negative value
		 */
		if (delta > 0 || delta < -1 * ((s32)sess->vififo_size / 2)) {
			atomic_dec(&sess->esparser_queued_bufs);
			list_del(&tmp->list);
			kfree(tmp);
		}
	}

	if (!match) {
		dev_err(dev, "Buffer %u done but can't match offset (%08X)\n",
			vbuf->vb2_buf.index, offset);

		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
		spin_unlock_irqrestore(&sess->ts_spinlock, flags);
		return;
	}

	timestamp = match->ts;
	list_del(&match->list);
	kfree(match);
	spin_unlock_irqrestore(&sess->ts_spinlock, flags);

	dst_buf_done(sess, vbuf, field, timestamp);
}

void amvdec_dst_buf_done_idx(struct amvdec_session *sess,
			     u32 buf_idx, s32 offset, u32 field)
{
	struct vb2_v4l2_buffer *vbuf;
	struct device *dev = sess->core->dev_dec;

	vbuf = v4l2_m2m_dst_buf_remove_by_idx(sess->m2m_ctx, buf_idx);
	if (!vbuf) {
		dev_err(dev,
			"Buffer %u done but it doesn't exist in m2m_ctx\n",
			buf_idx);
		amvdec_rm_first_ts(sess);
		return;
	}

	if (offset >= 0)
		amvdec_dst_buf_done_offset(sess, vbuf, offset, field);
	else
		amvdec_dst_buf_done(sess, vbuf, field);
}
EXPORT_SYMBOL_GPL(amvdec_dst_buf_done_idx);

void amvdec_set_par_from_dar(struct amvdec_session *sess,
			     u32 dar_num, u32 dar_den)
{
	u32 div;

	sess->pixelaspect.numerator = sess->height * dar_num;
	sess->pixelaspect.denominator = sess->width * dar_den;
	div = gcd(sess->pixelaspect.numerator, sess->pixelaspect.denominator);
	sess->pixelaspect.numerator /= div;
	sess->pixelaspect.denominator /= div;
}
EXPORT_SYMBOL_GPL(amvdec_set_par_from_dar);

void amvdec_abort(struct amvdec_session *sess)
{
	dev_info(sess->core->dev, "Aborting decoding session!\n");
	vb2_queue_error(&sess->m2m_ctx->cap_q_ctx.q);
	vb2_queue_error(&sess->m2m_ctx->out_q_ctx.q);
}
EXPORT_SYMBOL_GPL(amvdec_abort);
