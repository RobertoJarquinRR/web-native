/*
 * webnative_media.h - public media provider ABI (v1).
 *
 * This header is the contract between the engine and an optional media
 * provider (a demuxer + decoder). It is versioned separately from
 * include/webnative.h because third parties compile it:
 *
 *   - plain C, no C++/STL/.NET types, no exceptions, no wchar_t;
 *   - every struct starts with `uint32_t struct_size` (append-only);
 *   - every kind enum reserves 0 for "unknown";
 *   - no GPU headers and no backend types: native handles are opaque.
 *
 * The engine never links a decoder and never interprets codec bytes. A build
 * with no provider is valid: `<video>` still lays out, paints its poster, a
 * neutral placeholder or the last uploaded frame, and reports
 * WN_MEDIA_UNSUPPORTED_CONTAINER when the host tries to open. `media_null` is
 * always present and never matches.
 *
 * E4 implements the provider ABI, the registry, media_null, the inline session
 * driver (state machine, seek/loop/autoplay, events) and an RGBA8 frame sink.
 * Real demuxers/decoders, audio, subtitles, NV12/P010 and NV12 shaders are
 * later phases; the ABI here does not change for them.
 */
#ifndef WEBNATIVE_MEDIA_H
#define WEBNATIVE_MEDIA_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define WN_MEDIA_ABI_VERSION 1u

/* ------------------------------------------------------------- status */

typedef enum wn_media_status {
    WN_MEDIA_OK = 0,
    WN_MEDIA_AGAIN = 1,                 /* retry later */
    WN_MEDIA_EOF = 2,
    WN_MEDIA_UNSUPPORTED_CONTAINER = 3,
    WN_MEDIA_UNSUPPORTED_CODEC = 4,
    WN_MEDIA_IO = 5,
    WN_MEDIA_CORRUPT = 6,
    WN_MEDIA_OOM = 7,
    WN_MEDIA_CANCELLED = 8,
    WN_MEDIA_GPU = 9,
    WN_MEDIA_INTERNAL = 10
} wn_media_status;

/* ---------------------------------------------------- container / codec */

typedef enum wn_container_kind {
    WN_CONTAINER_UNKNOWN = 0,
    WN_CONTAINER_MP4,
    WN_CONTAINER_MKV,
    WN_CONTAINER_WEBM,
    WN_CONTAINER_OGG,
    WN_CONTAINER_AVI
} wn_container_kind;

typedef enum wn_codec_kind {
    WN_CODEC_UNKNOWN = 0,
    WN_CODEC_H264,
    WN_CODEC_HEVC,
    WN_CODEC_VP8,
    WN_CODEC_VP9,
    WN_CODEC_AV1,
    WN_CODEC_AAC,
    WN_CODEC_OPUS,
    WN_CODEC_VORBIS,
    WN_CODEC_MP3,
    WN_CODEC_FLAC
} wn_codec_kind;

/* Bit mask helper: WN_MEDIA_BIT(WN_CONTAINER_MP4), WN_MEDIA_BIT(WN_CODEC_H264).
 * Keep the enum values below 32 when the result is stored in a uint32_t. */
#define WN_MEDIA_BIT(kind) (1u << (kind))

typedef enum wn_pixel_format {
    WN_PIXEL_NONE = 0,
    WN_PIXEL_NV12 = 1,      /* 8-bit 4:2:0, 2 planes (Y + interleaved UV) */
    WN_PIXEL_P010 = 2,      /* 10-bit 4:2:0 in 16 bits, 2 planes */
    WN_PIXEL_RGBA8 = 3,     /* 8-bit RGBA, 1 plane */
    WN_PIXEL_NATIVE = 16    /* opaque GPU surface; see native_kind */
} wn_pixel_format;

typedef enum wn_sample_format {
    WN_SAMPLE_NONE = 0,
    WN_SAMPLE_F32 = 1,
    WN_SAMPLE_S16 = 2
} wn_sample_format;

/* Native surface discriminator. The pointer itself is opaque to the engine
 * and is only consumed by the renderer's video sink. */
typedef enum wn_native_kind {
    WN_NATIVE_NONE = 0,
    WN_NATIVE_D3D11 = 1,
    WN_NATIVE_D3D12 = 2,
    WN_NATIVE_VULKAN = 3,
    WN_NATIVE_METAL = 4
} wn_native_kind;

/* Color mask packed into wn_video_frame.color. */
#define WN_MEDIA_COLOR(matrix, range, primaries, transfer) \
    ((uint32_t)(matrix) | ((uint32_t)(range) << 8) |        \
     ((uint32_t)(primaries) << 16) | ((uint32_t)(transfer) << 24))

typedef enum wn_color_matrix {
    WN_MATRIX_BT601 = 0,
    WN_MATRIX_BT709 = 1,
    WN_MATRIX_BT2020_NCL = 2
} wn_color_matrix;

typedef enum wn_color_range {
    WN_RANGE_LIMITED = 0,
    WN_RANGE_FULL = 1
} wn_color_range;

typedef enum wn_color_primaries {
    WN_PRIMARIES_BT709 = 0,
    WN_PRIMARIES_BT2020 = 1,
    WN_PRIMARIES_DISPLAY_P3 = 2
} wn_color_primaries;

typedef enum wn_transfer_fn {
    WN_TRANSFER_SDR = 0,
    WN_TRANSFER_PQ = 1,
    WN_TRANSFER_HLG = 2
} wn_transfer_fn;

/* ------------------------------------------------------------- session */

/* Opaque session handle returned by provider `open`; owned by the provider
 * until `close`. */
typedef struct wn_media_session wn_media_session;

typedef enum wn_media_state {
    WN_MEDIA_STATE_IDLE = 0,       /* no session; the poster is painted */
    WN_MEDIA_STATE_OPENING = 1,
    WN_MEDIA_STATE_PAUSED = 2,     /* ready, position fixed */
    WN_MEDIA_STATE_PLAYING = 3,
    WN_MEDIA_STATE_BUFFERING = 4,  /* missing data; last frame frozen */
    WN_MEDIA_STATE_SEEKING = 5,
    WN_MEDIA_STATE_ENDED = 6,
    WN_MEDIA_STATE_ERROR = 7       /* terminal until open/set_source */
} wn_media_state;

/* -------------------------------------------------------- capabilities */

typedef struct wn_media_caps {
    uint32_t struct_size;
    uint32_t flags;            /* WN_MEDIA_CAP_* */
    uint32_t containers;       /* WN_MEDIA_BIT(wn_container_kind) */
    uint32_t video_codecs;     /* WN_MEDIA_BIT(wn_codec_kind) */
    uint32_t audio_codecs;
    uint32_t pixel_formats;    /* WN_MEDIA_BIT(wn_pixel_format) */
    uint32_t max_video_width, max_video_height;
    uint32_t max_audio_channels;
} wn_media_caps;

#define WN_MEDIA_CAP_ASYNC        0x0001u /* decodes off the UI thread */
#define WN_MEDIA_CAP_HW_DECODE    0x0002u /* produces WN_PIXEL_NATIVE */
#define WN_MEDIA_CAP_SUBTITLES    0x0004u /* exposes TEXT/BITMAP streams */
#define WN_MEDIA_CAP_AUDIO        0x0008u
#define WN_MEDIA_CAP_SEEK         0x0010u
#define WN_MEDIA_CAP_HDR          0x0020u

/* --------------------------------------------------------- stream info */

typedef enum wn_stream_type {
    WN_STREAM_UNKNOWN = 0,
    WN_STREAM_VIDEO,
    WN_STREAM_AUDIO,
    WN_STREAM_TEXT,
    WN_STREAM_BITMAP
} wn_stream_type;

typedef struct wn_media_stream_info {
    uint32_t struct_size;
    uint32_t type;                 /* wn_stream_type */
    uint32_t codec;                /* wn_codec_kind */
    int32_t  index;                /* provider's stream index */
    int64_t  duration_us;
    int32_t  width, height;        /* video: coded */
    int32_t  display_width, display_height; /* after rotation and PAR */
    uint32_t rotation_deg;         /* 0/90/180/270 */
    int32_t  sample_rate, channels;/* audio */
    uint32_t color;                /* WN_MEDIA_COLOR(...) */
    char     language[4];          /* ISO-639-2 + NUL; "" when absent */
} wn_media_stream_info;

typedef struct wn_media_info {
    uint32_t struct_size;
    uint32_t flags;                /* WN_MEDIA_INFO_* */
    uint32_t container;            /* wn_container_kind */
    uint32_t stream_count;
    int64_t  duration_us;
    wn_media_stream_info video;    /* first video stream, or type=0 */
    wn_media_stream_info audio;    /* first audio stream, or type=0 */
} wn_media_info;

#define WN_MEDIA_INFO_SEEKABLE 0x01u
#define WN_MEDIA_INFO_VFR      0x02u
#define WN_MEDIA_INFO_HDR      0x04u

/* --------------------------------------------------- frames and packets */

/* The runtime calls release() exactly once, from any thread, after the sink
 * is done with the frame. It must be thread-safe and non-blocking. */
typedef struct wn_frame_lease {
    void  *object;
    void  (*release)(void *context, void *object);
    void  *context;
} wn_frame_lease;

typedef struct wn_media_plane {
    const uint8_t *data;   /* NULL in a native frame */
    int32_t stride;        /* bytes per row */
    int32_t width, height; /* plane samples */
} wn_media_plane;

typedef struct wn_video_frame {
    uint32_t struct_size;
    uint32_t format;                /* wn_pixel_format */
    int32_t  coded_width, coded_height;
    int32_t  display_width, display_height; /* rotation + PAR applied */
    uint32_t rotation_deg;
    uint32_t planes;                /* 1..3 */
    wn_media_plane plane[3];
    int64_t  pts_us;
    int64_t  duration_us;
    uint32_t flags;                 /* WN_FRAME_* */
    uint32_t color;                 /* WN_MEDIA_COLOR(...) */
    void    *native_texture;        /* WN_PIXEL_NATIVE: opaque, never deref */
    uint32_t native_kind;           /* wn_native_kind */
    uint32_t native_slice;          /* array slice (D3D11VA DPB) */
    wn_frame_lease lease;
} wn_video_frame;

#define WN_FRAME_KEY      0x01u
#define WN_FRAME_EOS      0x02u
#define WN_FRAME_CORRUPT  0x04u
#define WN_FRAME_OVERLAY  0x08u  /* bitmap subtitle (PGS/VobSub) */

typedef struct wn_audio_frame {
    uint32_t struct_size;
    uint32_t sample_format;         /* wn_sample_format */
    int32_t  sample_rate;
    int32_t  channels;
    uint32_t channel_mask;          /* WAVE mask; 0 = default */
    int32_t  frame_count;           /* samples per channel */
    const void *data;               /* interleaved: frame_count * channels */
    int64_t  pts_us;
    wn_frame_lease lease;
} wn_audio_frame;

typedef struct wn_media_packet {
    uint32_t struct_size;
    uint32_t stream;                /* wn_media_stream_info.index */
    uint32_t type;                  /* wn_stream_type */
    uint32_t flags;                 /* WN_PACKET_* */
    int64_t  pts_us;
    int64_t  duration_us;
    void    *token;                 /* opaque; valid until decode()/flush() */
} wn_media_packet;

#define WN_PACKET_KEY 0x01u
#define WN_PACKET_EOS 0x02u

/* ----------------------------------------------------------- open desc */

typedef struct wn_media_open_desc {
    uint32_t struct_size;
    const char *path_utf8;          /* valid during open(); provider copies */
    uint32_t flags;                 /* WN_MEDIA_OPEN_* */
    uint32_t allowed_formats;       /* formats the sink accepts; 0 = any CPU */
    uint32_t native_kind;           /* WN_NATIVE_* of the sink; 0 = no HW */
    void    *native_device;         /* opaque; only for HW decode */
    int32_t  max_width, max_height; /* resolution cap */
    int32_t  audio_sample_rate;     /* 0 = device rate */
    int32_t  audio_channels;
} wn_media_open_desc;

#define WN_MEDIA_OPEN_VIDEO_OFF 0x01u
#define WN_MEDIA_OPEN_AUDIO_OFF 0x02u
#define WN_MEDIA_OPEN_LOOP      0x04u

/* ------------------------------------------------ provider operations */

typedef struct wn_media_provider_ops {
    uint32_t struct_size;           /* sizeof(wn_media_provider_ops) */
    uint32_t abi_version;           /* WN_MEDIA_ABI_VERSION */
    const char *name;               /* "null", "ffmpeg-lgpl", "mf", ... */

    void (*caps)(void *context, wn_media_caps *out);
    wn_media_status (*probe)(void *context, const char *path_utf8,
                             wn_media_info *out);
    wn_media_status (*open)(void *context, const wn_media_open_desc *desc,
                            wn_media_session **out_session);
    wn_media_status (*streams)(void *context, wn_media_session *session,
                               wn_media_stream_info *out, uint32_t capacity,
                               uint32_t *out_count);
    wn_media_status (*read)(void *context, wn_media_session *session,
                            wn_media_packet *out);
    wn_media_status (*seek)(void *context, wn_media_session *session,
                            int64_t pts_us, uint32_t flags);
    wn_media_status (*decode)(void *context, wn_media_session *session,
                              const wn_media_packet *packet);
    wn_media_status (*receive_video)(void *context, wn_media_session *session,
                                     wn_video_frame *out);
    wn_media_status (*receive_audio)(void *context, wn_media_session *session,
                                     wn_audio_frame *out);
    wn_media_status (*flush)(void *context, wn_media_session *session);
    wn_media_status (*close)(void *context, wn_media_session *session);
    void (*destroy)(void *context);  /* frees the provider context */
} wn_media_provider_ops;

/* Single exported symbol of a dynamic provider. The engine validates the
 * returned vtable before copying it. */
#define WN_MEDIA_PROVIDER_QUERY "wn_media_provider_query"
typedef int (*wn_media_provider_query_fn)(uint32_t abi_version,
                                          const wn_media_provider_ops **out_ops,
                                          void **out_context);

#if defined(__cplusplus)
}
#endif

#endif /* WEBNATIVE_MEDIA_H */
