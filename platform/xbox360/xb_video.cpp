// LAIKA 64 - preview videos (see xb_video.h).

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_log.h"
#include "../../third_party/openh264/codec/api/wels/codec_api.h"
#include "xb_video.h"

#define VIDEO_MAX_FILE (64u * 1024u * 1024u)

// One track's samples: where each one is in the file.
struct Track
{
    u32 samples;
    u32 *offset, *size;
    u32 maxSize;
    u32 timescale, delta;     // ticks a second, ticks of one sample (0: unknown)
};

struct XbVideo
{
    u8 *file;
    u32 fileSize;
    Track video, audio;
    u32 width, height;        // as the video track's sample entry says
    const u8 *avcc;           // the decoder's configuration: NAL length size, SPS and PPS
    u32 avccSize, lenSize;
    int audioCodec;           // VIDEO_AUDIO_*
    const u8 *asc;            // an AAC track's AudioSpecificConfig
    u32 ascSize;
    u32 next;                 // the next video sample to decode
    u8 *au;                   // one sample as an Annex B stream (start codes in place of lengths)
    u32 auCap;
    u32 errors;
    ISVCDecoder *dec;
};

static u32 be16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// The first box of `type` in [p, end): its payload and that payload's size; NULL when there is none.
static const u8 *box_find(const u8 *p, const u8 *end, const char *type, u32 *size)
{
    while (end - p >= 8)
    {
        u32 n = be32(p), head = 8;
        if (n == 1)   // a 64-bit size follows the type
        {
            if (end - p < 16 || be32(p + 8)) return NULL;
            n = be32(p + 12);
            head = 16;
        }
        else if (n == 0) n = (u32)(end - p);   // to the end of the file
        if (n < head || n > (u32)(end - p)) return NULL;
        if (!memcmp(p + 4, type, 4))
        {
            *size = n - head;
            return p + head;
        }
        p += n;
    }
    return NULL;
}

// A track's sample tables ("stbl"): each sample's place and size, its timing.
static int samples_read(const XbVideo *v, Track *t, const u8 *stbl, u32 stblSize)
{
    const u8 *end = stbl + stblSize, *stsz, *stsc, *stco, *stts;
    u32 nStsz, nStsc, nStco, nStts, i, chunks, runs, run = 0, sample = 0, fixed, co64 = 0;

    stsz = box_find(stbl, end, "stsz", &nStsz);
    stsc = box_find(stbl, end, "stsc", &nStsc);
    stco = box_find(stbl, end, "stco", &nStco);
    if (!stco) { stco = box_find(stbl, end, "co64", &nStco); co64 = 1; }
    if (!stsz || !stsc || !stco || nStsz < 12 || nStsc < 8 || nStco < 8) return 0;
    fixed = be32(stsz + 4);
    t->samples = be32(stsz + 8);
    runs = be32(stsc + 4);
    chunks = be32(stco + 4);
    if (!t->samples || t->samples > 1000000 || (!fixed && (nStsz - 12) / 4 < t->samples)) return 0;
    if (!runs || (nStsc - 8) / 12 < runs || (nStco - 8) / (co64 ? 8 : 4) < chunks) return 0;

    t->offset = (u32 *)malloc(t->samples * sizeof(u32));
    t->size = (u32 *)malloc(t->samples * sizeof(u32));
    if (!t->offset || !t->size) return 0;
    for (i = 0; i < t->samples; i++)
    {
        t->size[i] = fixed ? fixed : be32(stsz + 12 + 4 * i);
        if (t->size[i] > t->maxSize) t->maxSize = t->size[i];
    }
    // Chunk by chunk: its samples lie one after the other from the chunk's offset.
    for (i = 0; i < chunks && sample < t->samples; i++)
    {
        u32 per, k, off;
        while (run + 1 < runs && be32(stsc + 8 + 12 * (run + 1)) <= i + 1) run++;
        per = be32(stsc + 8 + 12 * run + 4);
        if (co64)
        {
            if (be32(stco + 8 + 8 * i)) return 0;   // past 4 GB
            off = be32(stco + 8 + 8 * i + 4);
        }
        else off = be32(stco + 8 + 4 * i);
        for (k = 0; k < per && sample < t->samples; k++, sample++)
        {
            if (off > v->fileSize || t->size[sample] > v->fileSize - off) return 0;
            t->offset[sample] = off;
            off += t->size[sample];
        }
    }
    if (sample != t->samples) return 0;
    stts = box_find(stbl, end, "stts", &nStts);
    if (stts && nStts >= 16 && be32(stts + 4)) t->delta = be32(stts + 12);
    return 1;
}

// An MPEG-4 descriptor's length (1 to 4 bytes of 7 bits); NULL past the end.
static const u8 *desc_len(const u8 *p, const u8 *end, u32 *len)
{
    int k;
    *len = 0;
    for (k = 0; k < 4 && p < end; k++)
    {
        u8 b = *p++;
        *len = (*len << 7) | (b & 0x7Fu);
        if (!(b & 0x80)) return *len <= (u32)(end - p) ? p : NULL;
    }
    return NULL;
}

// The codec in an "esds" box (ES descriptor > decoder config), and for AAC its AudioSpecificConfig
// (> decoder specific info).
static int esds_read(XbVideo *v, const u8 *p, u32 size)
{
    const u8 *end = p + size;
    u32 len;
    u8 flags;
    if (size < 4) return 0;
    p += 4;   // version, flags
    if (p >= end || *p++ != 0x03 || !(p = desc_len(p, end, &len)) || len < 3) return 0;
    end = p + len;
    p += 2;   // ES_ID
    flags = *p++;
    if (flags & 0x80) p += 2;               // depends on another stream
    if (flags & 0x40) { if (p >= end) return 0; p += 1 + *p; }   // a URL
    if (flags & 0x20) p += 2;               // OCR stream
    if (p >= end || *p++ != 0x04 || !(p = desc_len(p, end, &len)) || len < 13) return 0;
    if (p[0] == 0x6B || p[0] == 0x69)   // MPEG-1 / MPEG-2 audio: MP3 frames, nothing to set up
    {
        v->audioCodec = VIDEO_AUDIO_MP3;
        return 1;
    }
    if (p[0] != 0x40 && p[0] != 0x66 && p[0] != 0x67 && p[0] != 0x68) return 0;   // MPEG-4 AAC, MPEG-2 AAC Main/LC/SSR
    end = p + len;
    p += 13;
    if (p >= end || *p++ != 0x05 || !(p = desc_len(p, end, &len)) || len < 2) return 0;
    v->asc = p;
    v->ascSize = len;
    v->audioCodec = VIDEO_AUDIO_AAC;
    return 1;
}

// The video track's decoder configuration (sample entry "avc1"/"avc3", its "avcC").
static int video_entry(XbVideo *v, const u8 *stsd, u32 n)
{
    const u8 *entry = stsd + 8;
    if (n < 8 + 86 || be32(entry) < 86 || be32(entry) > n - 8 || (memcmp(entry + 4, "avc1", 4) && memcmp(entry + 4, "avc3", 4))) return 0;
    v->width = be16(entry + 32);
    v->height = be16(entry + 34);
    v->avcc = box_find(entry + 86, entry + be32(entry), "avcC", &v->avccSize);
    if (!v->avcc || v->avccSize < 7 || v->avcc[0] != 1) return 0;
    v->lenSize = (v->avcc[4] & 3u) + 1;
    return 1;
}

// The sound track's sample entry "mp4a" and its "esds".
static int audio_entry(XbVideo *v, const u8 *stsd, u32 n)
{
    const u8 *entry = stsd + 8, *esds;
    u32 nEsds, version;
    if (n < 8 + 36 || be32(entry) < 36 || be32(entry) > n - 8 || memcmp(entry + 4, "mp4a", 4)) return 0;
    version = be16(entry + 16);   // QuickTime sound descriptions add 16 or 36 bytes
    esds = box_find(entry + 36 + (version == 1 ? 16 : version == 2 ? 36 : 0), entry + be32(entry), "esds", &nEsds);
    return esds && esds_read(v, esds, nEsds);
}

// The file's tracks: the video one (needed) and an AAC one (if there is one).
static int mp4_read(XbVideo *v)
{
    const u8 *end = v->file + v->fileSize, *moov, *trak, *p;
    u32 nMoov, nTrak;
    int haveVideo = 0, haveAudio = 0;
    moov = box_find(v->file, end, "moov", &nMoov);
    if (!moov) return 0;
    for (p = moov; (trak = box_find(p, moov + nMoov, "trak", &nTrak)) != NULL; p = trak + nTrak)
    {
        const u8 *mdia, *hdlr, *mdhd, *minf, *stbl, *stsd;
        u32 nMdia, nHdlr, nMdhd, nMinf, nStbl, nStsd, timescale = 0;
        int isVideo, isAudio;
        Track *t;
        mdia = box_find(trak, trak + nTrak, "mdia", &nMdia);
        if (!mdia) continue;
        hdlr = box_find(mdia, mdia + nMdia, "hdlr", &nHdlr);
        if (!hdlr || nHdlr < 12) continue;
        isVideo = !memcmp(hdlr + 8, "vide", 4) && !haveVideo;
        isAudio = !memcmp(hdlr + 8, "soun", 4) && !haveAudio;
        if (!isVideo && !isAudio) continue;
        mdhd = box_find(mdia, mdia + nMdia, "mdhd", &nMdhd);
        if (mdhd && mdhd[0] == 0 && nMdhd >= 20) timescale = be32(mdhd + 12);
        else if (mdhd && mdhd[0] == 1 && nMdhd >= 32) timescale = be32(mdhd + 20);
        minf = box_find(mdia, mdia + nMdia, "minf", &nMinf);
        stbl = minf ? box_find(minf, minf + nMinf, "stbl", &nStbl) : NULL;
        stsd = stbl ? box_find(stbl, stbl + nStbl, "stsd", &nStsd) : NULL;
        if (!stsd) continue;
        t = isVideo ? &v->video : &v->audio;
        if (isVideo ? !video_entry(v, stsd, nStsd) : !audio_entry(v, stsd, nStsd)) continue;
        if (!samples_read(v, t, stbl, nStbl))
        {
            if (isAudio) { v->asc = NULL; v->audioCodec = VIDEO_AUDIO_NONE; continue; }   // the picture alone, then
            return 0;
        }
        t->timescale = timescale;
        if (isVideo) haveVideo = 1;
        else haveAudio = 1;
    }
    if (!haveVideo) return 0;
    // Lengths shorter than a start code make a sample grow (1 byte lengths: up to 2.5 times).
    v->auCap = v->video.maxSize * 3 + v->avccSize * 4 + 64;
    v->au = (u8 *)malloc(v->auCap);
    return v->au != NULL;
}

static u32 annexb_put(u8 *out, const u8 *nal, u32 size)
{
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    memcpy(out + 4, nal, size);
    return size + 4;
}

// The SPS and PPS of the "avcC" box as an Annex B stream: bytes written, 0 when it is broken.
static u32 avcc_params(const XbVideo *v, u8 *out)
{
    const u8 *p = v->avcc + 5, *end = v->avcc + v->avccSize;
    u32 n = 0, set, count, size;
    for (set = 0; set < 2; set++)   // the SPS list, then the PPS list
    {
        if (p >= end) return 0;
        count = set ? *p : (*p & 0x1Fu);
        p++;
        for (; count; count--)
        {
            if (end - p < 2) return 0;
            size = be16(p);
            p += 2;
            if (size > (u32)(end - p)) return 0;
            n += annexb_put(out + n, p, size);
            p += size;
        }
    }
    return n;
}

// Sample `i` as an Annex B stream in v->au (the first one after the SPS and PPS): its size, 0 when broken.
static u32 sample_annexb(XbVideo *v, u32 i)
{
    const u8 *p = v->file + v->video.offset[i], *end = p + v->video.size[i];
    u32 n = 0, k, size;
    if (i == 0 && !(n = avcc_params(v, v->au))) return 0;
    while ((u32)(end - p) >= v->lenSize)
    {
        for (size = 0, k = 0; k < v->lenSize; k++) size = (size << 8) | *p++;
        if (size > (u32)(end - p) || n + size + 4 > v->auCap) return 0;
        n += annexb_put(v->au + n, p, size);
        p += size;
    }
    return n;
}

// The decoder's own messages (errors only) into the log.
static void dec_trace(void *ctx, int level, const char *text)
{
    (void)ctx;
    (void)level;
    H64_WARN("[video] decoder: %s", text);
}

static int decoder_open(XbVideo *v)
{
    SDecodingParam param;
    int level = WELS_LOG_ERROR;
    WelsTraceCallback trace = dec_trace;
    if (WelsCreateDecoder(&v->dec) || !v->dec) { v->dec = NULL; return 0; }
    v->dec->SetOption(DECODER_OPTION_TRACE_LEVEL, &level);
    v->dec->SetOption(DECODER_OPTION_TRACE_CALLBACK, &trace);
    memset(&param, 0, sizeof(param));
    param.uiTargetDqLayer = (unsigned char)-1;
    param.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    param.sVideoProperty.size = sizeof(param.sVideoProperty);
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    return !v->dec->Initialize(&param);
}

static void decoder_close(XbVideo *v)
{
    if (!v->dec) return;
    v->dec->Uninitialize();
    WelsDestroyDecoder(v->dec);
    v->dec = NULL;
}

XbVideo *VideoOpen(const char *path)
{
    XbVideo *v = (XbVideo *)calloc(1, sizeof(XbVideo));
    FILE *f = fopen(path, "rb");
    long size = 0;

    if (!v || !f) goto fail;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) <= 0 || (u32)size > VIDEO_MAX_FILE || fseek(f, 0, SEEK_SET)) goto fail;
    v->fileSize = (u32)size;
    v->file = (u8 *)malloc(v->fileSize);
    if (!v->file || fread(v->file, 1, v->fileSize, f) != v->fileSize) goto fail;
    fclose(f);
    f = NULL;
    if (!mp4_read(v))
    {
        H64_WARN("[video] %s: no H.264 video track this reads", path);
        goto fail;
    }
    if (!decoder_open(v)) goto fail;
    return v;

fail:
    if (f) fclose(f);
    VideoClose(v);
    return NULL;
}

// Whether video sample i is a picture no other one refers to: its first slice's nal_ref_idc is 0.
static int sample_unreferenced(const XbVideo *v, u32 i)
{
    const u8 *p = v->file + v->video.offset[i], *end = p + v->video.size[i];
    u32 k, size;
    while ((u32)(end - p) > v->lenSize)
    {
        for (size = 0, k = 0; k < v->lenSize; k++) size = (size << 8) | *p++;
        if (!size || size > (u32)(end - p)) return 0;
        if ((p[0] & 0x1F) == 1 || (p[0] & 0x1F) == 5) return (p[0] & 0x60) == 0 && (p[0] & 0x1F) == 1;   // a slice
        p += size;
    }
    return 0;
}

int VideoFrame(XbVideo *v, VideoPicture *pic, int drop)
{
    for (;;)
    {
        SBufferInfo info;
        unsigned char *dst[3] = { NULL, NULL, NULL };
        memset(&info, 0, sizeof(info));
        if (drop && v->next > 0 && v->next < v->video.samples && sample_unreferenced(v, v->next))
        {
            v->next++;
            return 2;
        }
        if (v->next < v->video.samples)
        {
            u32 len = sample_annexb(v, v->next);
            DECODING_STATE st;
            if (!len) return -1;
            st = v->dec->DecodeFrameNoDelay(v->au, (int)len, dst, &info);
            if (st != dsErrorFree && v->errors++ < 8) H64_WARN("[video] sample %u: decoder state 0x%X", v->next, (unsigned)st);
            v->next++;
        }
        else
        {
            // The end: the pictures the decoder still holds for their turn (B pictures reorder them).
            int left = 0;
            v->dec->GetOption(DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, &left);
            if (left <= 0) return 0;
            v->dec->FlushFrame(dst, &info);
            if (info.iBufferStatus != 1) return 0;
        }
        if (info.iBufferStatus == 1 && dst[0] && dst[1] && dst[2])
        {
            pic->y = dst[0];
            pic->u = dst[1];
            pic->v = dst[2];
            pic->width = info.UsrData.sSystemBuffer.iWidth;
            pic->height = info.UsrData.sSystemBuffer.iHeight;
            pic->strideY = info.UsrData.sSystemBuffer.iStride[0];
            pic->strideUV = info.UsrData.sSystemBuffer.iStride[1];
            return 1;
        }
    }
}

void VideoRewind(XbVideo *v)
{
    // A fresh decoder: nothing of the last pictures held back for their turn.
    decoder_close(v);
    decoder_open(v);
    v->next = 0;
}

u32 VideoRate1000(const XbVideo *v)
{
    return v->video.delta ? (u32)((unsigned __int64)v->video.timescale * 1000u / v->video.delta) : 0;
}

void VideoClose(XbVideo *v)
{
    if (!v) return;
    decoder_close(v);
    free(v->au);
    free(v->video.offset);
    free(v->video.size);
    free(v->audio.offset);
    free(v->audio.size);
    free(v->file);
    free(v);
}

int VideoAudioCodec(const XbVideo *v) { return v->audio.samples ? v->audioCodec : VIDEO_AUDIO_NONE; }

int VideoAudioConfig(const XbVideo *v, const u8 **config, u32 *size)
{
    if (!v->asc || !v->audio.samples) return 0;
    *config = v->asc;
    *size = v->ascSize;
    return 1;
}

u32 VideoAudioCount(const XbVideo *v) { return v->audioCodec ? v->audio.samples : 0; }

int VideoAudioPacket(const XbVideo *v, u32 i, const u8 **data, u32 *size)
{
    if (!v->audioCodec || i >= v->audio.samples) return 0;
    *data = v->file + v->audio.offset[i];
    *size = v->audio.size[i];
    return 1;
}
