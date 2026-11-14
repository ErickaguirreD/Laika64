// LAIKA 64 - preview videos: an MP4 file's H.264 track decoded picture by picture (OpenH264,
// third_party/openh264), and its AAC track's packets for the sound (decoded by xb_preview.cpp).
#ifndef XB_VIDEO_H
#define XB_VIDEO_H

#include <stddef.h>
#include "../../core/common/h64_types.h"

struct XbVideo;

// One decoded picture (I420), valid until the next VideoFrame/VideoRewind/VideoClose.
struct VideoPicture
{
    const u8 *y, *u, *v;
    int width, height;       // luma; the chroma planes have half of each
    int strideY, strideUV;
};

// Opens an MP4 with an H.264 video track (read whole into memory); NULL when it is not one.
XbVideo *VideoOpen(const char *path);
// The next picture in display order: 1, 0 after the last one, -1 on a broken file. With `drop`, a
// picture nothing refers to (most B pictures) may be left undecoded instead: 2 (it took its time).
int VideoFrame(XbVideo *v, VideoPicture *pic, int drop);
// Back to the first picture (the preview plays round and round).
void VideoRewind(XbVideo *v);
// Pictures per second as 1000 * rate (29970 = 29.97), 0 when the file does not say.
u32 VideoRate1000(const XbVideo *v);
void VideoClose(XbVideo *v);

// ---- The sound track (AAC or MP3) ----
enum { VIDEO_AUDIO_NONE = 0, VIDEO_AUDIO_AAC, VIDEO_AUDIO_MP3 };
int VideoAudioCodec(const XbVideo *v);
// An AAC track's AudioSpecificConfig (the decoder's set-up): 1, 0 when there is none.
int VideoAudioConfig(const XbVideo *v, const u8 **config, u32 *size);
// Its packets: how many, and the i-th one (one AAC frame, or MP3 frames).
u32 VideoAudioCount(const XbVideo *v);
int VideoAudioPacket(const XbVideo *v, u32 i, const u8 **data, u32 *size);

#endif
