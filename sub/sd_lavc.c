/*
 * This file is part of mplayer2.
 *
 * mplayer2 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mplayer2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mplayer2.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>

#include <libavcodec/avcodec.h>

#include "mp_msg.h"
#include "libmpdemux/stheader.h"
#include "sd.h"
#include "spudec.h"
// Current code still pushes subs directly to global spudec
#include "sub.h"

struct sd_lavc_priv {
    AVCodecContext *avctx;
    int count;
    struct sub_bitmap *inbitmaps;
    struct sub_bitmap *outbitmaps;
    bool bitmaps_changed;
    double endpts;
};

static int init(struct sh_sub *sh, struct osd_state *osd)
{
    if (sh->initialized)
        return 0;
    struct sd_lavc_priv *priv = talloc_zero(NULL, struct sd_lavc_priv);
    enum AVCodecID cid = AV_CODEC_ID_NONE;
    switch (sh->type) {
    case 'b':
        cid = AV_CODEC_ID_DVB_SUBTITLE; break;
    case 'p':
        cid = AV_CODEC_ID_HDMV_PGS_SUBTITLE; break;
    case 'x':
        cid = AV_CODEC_ID_XSUB; break;
    case 'v':
        cid = AV_CODEC_ID_DVD_SUBTITLE; break;
    }
    AVCodecContext *ctx = NULL;
    AVCodec *sub_codec = avcodec_find_decoder(cid);
    if (!sub_codec)
        goto error;
    ctx = avcodec_alloc_context3(sub_codec);
    if (!ctx)
        goto error;
    ctx->extradata_size = sh->extradata_len;
    ctx->extradata = sh->extradata;
    if (avcodec_open2(ctx, sub_codec, NULL) < 0)
        goto error;
    priv->avctx = ctx;
    sh->context = priv;
    return 0;

 error:
    mp_msg(MSGT_SUBREADER, MSGL_ERR,
           "Could not open libavcodec subtitle decoder\n");
    av_free(ctx);
    talloc_free(priv);
    return -1;
}

static void clear(struct sd_lavc_priv *priv)
{
    priv->count = 0;
    talloc_free(priv->inbitmaps);
    talloc_free(priv->outbitmaps);
    priv->inbitmaps = priv->outbitmaps = NULL;
    priv->bitmaps_changed = true;
    priv->endpts = MP_NOPTS_VALUE;
}

static void old_spudec_render(AVCodecContext *ctx, AVSubtitle *sub,
                              double pts, double endpts)
{
    if (!vo_spudec)
        vo_spudec = spudec_new_scaled(NULL, ctx->width, ctx->height, NULL, 0);
    spudec_set_paletted(vo_spudec,
                        sub->rects[0]->pict.data[0],
                        sub->rects[0]->pict.linesize[0],
                        sub->rects[0]->pict.data[1],
                        sub->rects[0]->x, sub->rects[0]->y,
                        sub->rects[0]->w, sub->rects[0]->h,
                        pts, endpts);
    vo_osd_changed(OSDTYPE_SPU);
}

static void decode(struct sh_sub *sh, struct osd_state *osd, void *data,
                   int data_len, double pts, double duration)
{
    struct sd_lavc_priv *priv = sh->context;
    AVCodecContext *ctx = priv->avctx;
    AVSubtitle sub;
    AVPacket pkt;

    clear(priv);
    av_init_packet(&pkt);
    pkt.data = data;
    pkt.size = data_len;
    pkt.pts = pts * 1000;
    if (duration >= 0)
        pkt.convergence_duration = duration * 1000;
    int got_sub;
    int res = avcodec_decode_subtitle2(ctx, &sub, &got_sub, &pkt);
    if (res < 0 || !got_sub)
        return;
    if (pts != MP_NOPTS_VALUE) {
        if (sub.end_display_time > sub.start_display_time)
            duration = (sub.end_display_time - sub.start_display_time) / 1000.0;
        pts += sub.start_display_time / 1000.0;
    }
    double endpts = MP_NOPTS_VALUE;
    if (pts != MP_NOPTS_VALUE && duration >= 0)
        endpts = pts + duration;
    if (vo_spudec && sub.num_rects == 0)
        spudec_set_paletted(vo_spudec, NULL, 0, NULL, 0, 0, 0, 0, pts, endpts);
    if (sub.num_rects > 0) {
        switch (sub.rects[0]->type) {
        case SUBTITLE_BITMAP:
            // Assume resolution heuristics only work for PGS and DVB
            if (!osd->support_rgba || sh->type != 'p' && sh->type != 'b') {
                old_spudec_render(ctx, &sub, pts, endpts);
                break;
            }
            priv->inbitmaps = talloc_array(priv, struct sub_bitmap,
                                           sub.num_rects);
            for (int i = 0; i < sub.num_rects; i++) {
                struct AVSubtitleRect *r = sub.rects[i];
                struct sub_bitmap *b = &priv->inbitmaps[i];
                uint32_t *outbmp = talloc_size(priv->inbitmaps,
                                               r->w * r->h * 4);
                b->bitmap = outbmp;
                b->w = r->w;
                b->h = r->h;
                b->x = r->x;
                b->y = r->y;
                uint8_t *inbmp = r->pict.data[0];
                uint32_t *palette = (uint32_t *) r->pict.data[1];
                for (int y = 0; y < r->h; y++) {
                    for (int x = 0; x < r->w; x++)
                        *outbmp++ = palette[*inbmp++];
                    inbmp += r->pict.linesize[0] - r->w;
                };
            }
            priv->count = sub.num_rects;
            priv->endpts = endpts;
            break;
        default:
            mp_msg(MSGT_SUBREADER, MSGL_ERR, "sd_lavc: unsupported subtitle "
                   "type from libavcodec\n");
            break;
        }
    }
    avsubtitle_free(&sub);
}

static void get_bitmaps(struct sh_sub *sh, struct osd_state *osd,
                        struct sub_bitmaps *res)
{
    struct sd_lavc_priv *priv = sh->context;

    if (priv->endpts != MP_NOPTS_VALUE && (osd->sub_pts >= priv->endpts ||
                                           osd->sub_pts < priv->endpts - 300))
        clear(priv);
    if (!osd->support_rgba)
        return;
    if (priv->bitmaps_changed && priv->count > 0)
        priv->outbitmaps = talloc_memdup(priv, priv->inbitmaps,
                                         talloc_get_size(priv->inbitmaps));
    bool pos_changed = false;
    // Hope that PGS subs set these and 720/576 works for dvb subs
    int inw = priv->avctx->width;
    if (!inw)
        inw = 720;
    int inh = priv->avctx->height;
    if (!inh)
        inh = 576;
    struct mp_eosd_res *d = &osd->dim;
    double xscale = (double) (d->w - d->ml - d->mr) / inw;
    double yscale = (double) (d->h - d->mt - d->mb) / inh;
    for (int i = 0; i < priv->count; i++) {
        struct sub_bitmap *bi = &priv->inbitmaps[i];
        struct sub_bitmap *bo = &priv->outbitmaps[i];
#define SET(var, val) pos_changed |= var != (int)(val); var = (val)
        SET(bo->x, bi->x * xscale + d->ml);
        SET(bo->y, bi->y * yscale + d->mt);
        SET(bo->dw, bi->w * xscale);
        SET(bo->dh, bi->h * yscale);
    }
    res->parts = priv->outbitmaps;
    res->part_count = priv->count;
    if (priv->bitmaps_changed)
        res->bitmap_id = ++res->bitmap_pos_id;
    else if (pos_changed)
        res->bitmap_pos_id++;
    priv->bitmaps_changed = false;
    res->type = SUBBITMAP_RGBA;
    res->scaled = xscale != 1 || yscale != 1;
}

static void reset(struct sh_sub *sh, struct osd_state *osd)
{
    struct sd_lavc_priv *priv = sh->context;

    clear(priv);
    // lavc might not do this right for all codecs; may need close+reopen
    avcodec_flush_buffers(priv->avctx);
}

static void uninit(struct sh_sub *sh)
{
    struct sd_lavc_priv *priv = sh->context;

    avcodec_close(priv->avctx);
    av_free(priv->avctx);
    talloc_free(priv);
}

const struct sd_functions sd_lavc = {
    .init = init,
    .decode = decode,
    .get_bitmaps = get_bitmaps,
    .reset = reset,
    .switch_off = reset,
    .uninit = uninit,
};
