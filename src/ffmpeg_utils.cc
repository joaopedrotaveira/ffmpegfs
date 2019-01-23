/*
 * FFmpeg utilities for ffmpegfs
 *
 * Copyright (C) 2017-2019 Norbert Schlia (nschlia@oblivion-software.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "ffmpeg_utils.h"
#include "id3v1tag.h"
#include "ffmpegfs.h"

#include <libgen.h>
#include <unistd.h>
#include <algorithm>
#include <regex.h>
#include <wordexp.h>
#include <memory>
#include <regex>

// Disable annoying warnings outside our code
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#ifdef __GNUC__
#  include <features.h>
#  if __GNUC_PREREQ(5,0) || defined(__clang__)
// GCC >= 5.0
#     pragma GCC diagnostic ignored "-Wfloat-conversion"
#  elif __GNUC_PREREQ(4,8)
// GCC >= 4.8
#  else
#     error("GCC < 4.8 not supported");
#  endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
#include <libswscale/swscale.h>
#if LAVR_DEPRECATE
#include <libswresample/swresample.h>
#else
#include <libavresample/avresample.h>
#endif
#ifndef USING_LIBAV
// Does not exist in Libav
#include "libavutil/ffversion.h"
#else
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#endif
#ifdef __cplusplus
}
#endif
#pragma GCC diagnostic pop

static int is_device(const AVClass *avclass);

#ifndef AV_ERROR_MAX_STRING_SIZE
#define AV_ERROR_MAX_STRING_SIZE 128
#endif // AV_ERROR_MAX_STRING_SIZE

// Add / to the path if required
// Returns: Constant reference to path
const std::string & append_sep(std::string * path)
{
    if (path->back() != '/')
    {
        *path += '/';
    }

    return *path;
}

// Add filename to path, adding a / to the path if required
// Returns: Constant reference to path
const std::string & append_filename(std::string * path, const std::string & filename)
{
    append_sep(path);

    *path += filename;

    return *path;
}

// Remove filename from path. Handy dirname alternative.
const std::string & remove_filename(std::string * path)
{
    char *p = new_strdup(*path);
    *path = dirname(p);
    delete [] p;
    append_sep(path);
    return *path;
}

// Remove path from filename. Handy basename alternative.
const std::string & remove_path(std::string *path)
{
    char *p = new_strdup(*path);
    *path = basename(p);
    delete [] p;
    return *path;
}

// Find extension in filename, if any
// Returns: Constant true if extension was found, false if there was none
bool find_ext(std::string * ext, const std::string & filename)
{
    size_t found;

    found = filename.rfind('.');

    if (found == std::string::npos)
    {
        // No extension
        ext->clear();
        return false;
    }
    else
    {
        // Have extension
        *ext = filename.substr(found + 1);
        return true;
    }
}

// Replace extension in filename. Take into account that there might
// not be an extension already.
// Returns: Constant reference to filename
const std::string & replace_ext(std::string * filename, const std::string & ext)
{
    size_t found;

    found = filename->rfind('.');

    if (found == std::string::npos)
    {
        // No extension, just add
        *filename += '.';
    }
    else
    {
        // Have extension, so replace
        *filename = filename->substr(0, found + 1);
    }

    *filename += ext;

    return *filename;
}

char * new_strdup(const std::string & str)
{
    size_t n = str.size() + 1;
    char * p = new char[n];
    strncpy(p, str.c_str(), n);
    return p;
}

const std::string & get_destname(std::string *destname, const std::string & filename)
{
    *destname = filename;
    remove_path(destname);
    replace_ext(destname, params.current_format(filename)->m_format_name);
    *destname = params.m_mountpath + *destname;

    return *destname;
}

std::string ffmpeg_geterror(int errnum)
{
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, error, AV_ERROR_MAX_STRING_SIZE);
    return error;
}

double ffmpeg_cvttime(int64_t ts, const AVRational & time_base)
{
    if (ts != 0 && ts != static_cast<int64_t>(AV_NOPTS_VALUE))
    {
        return (static_cast<double>(ts) * ::av_q2d(time_base));
    }
    else
    {
        return 0;
    }
}

#if !HAVE_MEDIA_TYPE_STRING
const char *get_media_type_string(enum AVMediaType media_type)
{
    switch (media_type)
    {
    case AVMEDIA_TYPE_VIDEO:
        return "video";
    case AVMEDIA_TYPE_AUDIO:
        return "audio";
    case AVMEDIA_TYPE_DATA:
        return "data";
    case AVMEDIA_TYPE_SUBTITLE:
        return "subtitle";
    case AVMEDIA_TYPE_ATTACHMENT:
        return "attachment";
    default:
        return "unknown";
    }
}
#endif

static std::string ffmpeg_libinfo(
        bool bLibExists,
        unsigned int /*nVersion*/,
        const char * /*pszCfg*/,
        int nVersionMinor,
        int nVersionMajor,
        int nVersionMicro,
        const char * pszLibname)
{
    std::string info;

    if (bLibExists)
    {
        string_format(info,
                      "lib%-17s: %d.%d.%d\n",
                      pszLibname,
                      nVersionMinor,
                      nVersionMajor,
                      nVersionMicro);
    }

    return info;
}

std::string ffmpeg_libinfo()
{
#define PRINT_LIB_INFO(libname, LIBNAME) \
    ffmpeg_libinfo(true, libname##_version(), libname##_configuration(), \
    LIB##LIBNAME##_VERSION_MAJOR, LIB##LIBNAME##_VERSION_MINOR, LIB##LIBNAME##_VERSION_MICRO, #libname)

    std::string info;

#ifdef USING_LIBAV
    info = "Libav Version       :\n";
#else
    info = "FFmpeg Version      : " FFMPEG_VERSION "\n";
#endif

    info += PRINT_LIB_INFO(avutil,      AVUTIL);
    info += PRINT_LIB_INFO(avcodec,     AVCODEC);
    info += PRINT_LIB_INFO(avformat,    AVFORMAT);
    // info += PRINT_LIB_INFO(avdevice,    AVDEVICE);
    // info += PRINT_LIB_INFO(avfilter,    AVFILTER);
#if LAVR_DEPRECATE
    info += PRINT_LIB_INFO(swresample,  SWRESAMPLE);
#else
    info += PRINT_LIB_INFO(avresample,  AVRESAMPLE);
#endif
    info += PRINT_LIB_INFO(swscale,     SWSCALE);
    // info += PRINT_LIB_INFO(postproc,    POSTPROC);

    return info;
}

static int is_device(const AVClass *avclass)
{
    if (!avclass)
        return 0;

    return 0;
    //return AV_IS_INPUT_DEVICE(avclass->category) || AV_IS_OUTPUT_DEVICE(avclass->category);
}

int show_formats_devices(int device_only)
{
    const AVInputFormat *ifmt  = nullptr;
    const AVOutputFormat *ofmt = nullptr;
    const char *last_name;
    int is_dev;

    std::printf("%s\n"
                " D. = Demuxing supported\n"
                " .E = Muxing supported\n"
                " --\n", device_only ? "Devices:" : "File formats:");
    last_name = "000";
    for (;;)
    {
        int decode = 0;
        int encode = 0;
        const char *name      = nullptr;
        const char *long_name = nullptr;
        const char *extensions = nullptr;

#if LAVF_DEP_AV_REGISTER
        void *ofmt_opaque = nullptr;
        ofmt_opaque = nullptr;
        while ((ofmt = av_muxer_iterate(&ofmt_opaque)))
#else
        while ((ofmt = av_oformat_next(ofmt)))
#endif
        {
            is_dev = is_device(ofmt->priv_class);
            if (!is_dev && device_only)
            {
                continue;
            }

            if ((!name || strcmp(ofmt->name, name) < 0) &&
                    strcmp(ofmt->name, last_name) > 0)
            {
                name        = ofmt->name;
                long_name   = ofmt->long_name;
                encode      = 1;
            }
        }
#if LAVF_DEP_AV_REGISTER
        void *ifmt_opaque = nullptr;
        ifmt_opaque = nullptr;
        while ((ifmt = av_demuxer_iterate(&ifmt_opaque)))
#else
        while ((ifmt = av_iformat_next(ifmt)))
#endif
        {
            is_dev = is_device(ifmt->priv_class);
            if (!is_dev && device_only)
            {
                continue;
            }

            if ((!name || strcmp(ifmt->name, name) < 0) &&
                    strcmp(ifmt->name, last_name) > 0)
            {
                name        = ifmt->name;
                long_name   = ifmt->long_name;
                extensions  = ifmt->extensions;
                encode      = 0;
            }
            if (name && strcmp(ifmt->name, name) == 0)
            {
                decode      = 1;
            }
        }

        if (!name)
        {
            break;
        }

        last_name = name;
        if (!extensions)
        {
            continue;
        }

        std::printf(" %s%s %-15s %-15s %s\n",
                    decode ? "D" : " ",
                    encode ? "E" : " ",
                    extensions != nullptr ? extensions : "-",
                    name,
                    long_name ? long_name:" ");
    }
    return 0;
}

const char * get_codec_name(AVCodecID codec_id, bool long_name)
{
    const AVCodecDescriptor * pCodecDescriptor;
    const char * psz = "unknown";

    pCodecDescriptor = avcodec_descriptor_get(codec_id);

    if (pCodecDescriptor != nullptr)
    {
        if (pCodecDescriptor->long_name != nullptr && long_name)
        {
            psz = pCodecDescriptor->long_name;
        }

        else
        {
            psz = pCodecDescriptor->name;
        }
    }

    return psz;
}

int mktree(const std::string &filename, mode_t mode)
{
    char *path = new_strdup(filename);

    if (path == nullptr)
    {
        return ENOMEM;
    }

    char dir[PATH_MAX] = "\0";
    char *p = strtok (path, "/");
    int status = 0;

    while (p != nullptr)
    {
        int newstat;

        strcat(dir, "/");
        strcat(dir, p);

        errno = 0;

        newstat = mkdir(dir, mode);

        if (!status && newstat && errno != EEXIST)
        {
            status = -1;
            break;
        }

        status = newstat;

        p = strtok (nullptr, "/");
    }

    delete [] path;

    return status;
}

void tempdir(std::string & dir)
{
    const char *temp = getenv("TMPDIR");

    if (temp != nullptr)
    {
        dir = temp;
        return;
    }

    dir = P_tmpdir;

    if (!dir.empty())
    {
        return;
    }

    dir = "/tmp";
}

int supports_albumart(FILETYPE filetype)
{
    // Could also allow OGG but the format requires special handling for album arts
    return (filetype == FILETYPE_MP3 || filetype == FILETYPE_MP4);
}

FILETYPE get_filetype(const std::string & desttype)
{
    if (!strcasecmp(desttype, "mp3"))
    {
        return FILETYPE_MP3;
    }
    else if (!strcasecmp(desttype, "mp4"))
    {
        return FILETYPE_MP4;
    }
    else if (!strcasecmp(desttype, "wav"))
    {
        return FILETYPE_WAV;
    }
    else if (!strcasecmp(desttype, "ogg"))
    {
        return FILETYPE_OGG;
    }
    else if (!strcasecmp(desttype, "webm"))
    {
        return FILETYPE_WEBM;
    }
    else if (!strcasecmp(desttype, "mov"))
    {
        return FILETYPE_MOV;
    }
    else if (!strcasecmp(desttype, "aiff"))
    {
        return FILETYPE_AIFF;
    }
    else if (!strcasecmp(desttype, "opus"))
    {
        return FILETYPE_OPUS;
    }
    else if (!strcasecmp(desttype, "prores"))
    {
        return FILETYPE_PRORES;
    }
    else
    {
        return FILETYPE_UNKNOWN;
    }
}

FILETYPE get_filetype_from_list(const std::string & desttypelist)
{
    std::vector<std::string> desttype = split(desttypelist, ",");
    FILETYPE filetype = FILETYPE_UNKNOWN;

    // Find first matching entry
    for (size_t n = 0; n < desttype.size() && filetype != FILETYPE_UNKNOWN; n++)
    {
        filetype = get_filetype(desttype[n]);
    }
    return filetype;
}

int get_codecs(const std::string & desttype, ffmpegfs_format *format)
{
    int ret = -1;

    switch (get_filetype(desttype))
    {
    case FILETYPE_MP3:
    {
        format->m_desttype      = desttype;
        format->m_audio_codec_id = AV_CODEC_ID_MP3;
        format->m_video_codec_id = AV_CODEC_ID_NONE; //AV_CODEC_ID_MJPEG;
        format->m_filetype      = FILETYPE_MP3;
        format->m_format_name   = "mp3";
        break;
    }
    case FILETYPE_MP4:
    {
        format->m_desttype      = desttype;
        format->m_audio_codec_id = AV_CODEC_ID_AAC;
        format->m_video_codec_id = AV_CODEC_ID_H264;
        format->m_filetype      = FILETYPE_MP4;
        format->m_format_name   = "mp4";
        break;
    }
    case FILETYPE_WAV:
    {
        format->m_desttype      = desttype;
        format->m_audio_codec_id = AV_CODEC_ID_PCM_S16LE;
        format->m_video_codec_id = AV_CODEC_ID_NONE;
        format->m_filetype      =  FILETYPE_WAV;
        format->m_format_name   = "wav";
        break;
    }
    case FILETYPE_OGG:
    {
        format->m_desttype      = desttype;
        format->m_audio_codec_id = AV_CODEC_ID_VORBIS;
        format->m_video_codec_id = AV_CODEC_ID_THEORA;
        format->m_filetype      = FILETYPE_OGG;
        format->m_format_name   = "ogg";
        break;
    }
    case FILETYPE_WEBM:
    {
        format->m_desttype      = desttype;
        format->m_audio_codec_id = AV_CODEC_ID_OPUS;
        format->m_video_codec_id = AV_CODEC_ID_VP9;
        format->m_filetype      = FILETYPE_WEBM;
        format->m_format_name   = "webm";
        break;
    }
    case FILETYPE_MOV:
    {
        format->m_desttype      = desttype;
        format->m_audio_codec_id = AV_CODEC_ID_AAC;
        format->m_video_codec_id = AV_CODEC_ID_H264;
        format->m_filetype      = FILETYPE_MOV;
        format->m_format_name   = "mov";
        break;
    }
    case FILETYPE_AIFF:
    {
        format->m_desttype      = desttype;
        format->m_audio_codec_id = AV_CODEC_ID_PCM_S16BE;
        format->m_video_codec_id = AV_CODEC_ID_NONE;
        format->m_filetype      = FILETYPE_AIFF;
        format->m_format_name   = "aiff";
        break;
    }
    case FILETYPE_OPUS:
    {
        format->m_desttype      = desttype;
        format->m_audio_codec_id = AV_CODEC_ID_OPUS;
        format->m_video_codec_id = AV_CODEC_ID_NONE;
        format->m_filetype      = FILETYPE_OPUS;
        format->m_format_name   = "opus";
        break;
    }
    case FILETYPE_PRORES:
    {
        format->m_desttype      = desttype;
        format->m_audio_codec_id = AV_CODEC_ID_PCM_S16LE;
        format->m_video_codec_id = AV_CODEC_ID_PRORES;
        format->m_filetype      = FILETYPE_PRORES;
        format->m_format_name   = "mov";
        break;
    }
    case FILETYPE_UNKNOWN:
    {
        ret = 0;
        break;
    }
    }

    return ret;
}

#ifdef USING_LIBAV
int avformat_alloc_output_context2(AVFormatContext **avctx, AVOutputFormat *oformat, const char *format, const char *filename)
{
    AVFormatContext *s = avformat_alloc_context();
    int ret = 0;

    *avctx = nullptr;
    if (!s)
        goto nomem;

    if (!oformat)
    {
        if (format)
        {
            oformat = av_guess_format(format, nullptr, nullptr);
            if (!oformat)
            {
                av_log(s, AV_LOG_ERROR, "Requested output format '%s' is not a suitable output format\n", format);
                ret = AVERROR(EINVAL);
                goto error;
            }
        }
        else
        {
            oformat = av_guess_format(nullptr, filename, nullptr);
            if (!oformat)
            {
                ret = AVERROR(EINVAL);
                av_log(s, AV_LOG_ERROR, "%s * Unable to find a suitable output format\n", filename);
                goto error;
            }
        }
    }

    s->oformat = oformat;
    if (s->oformat->priv_data_size > 0)
    {
        s->priv_data = av_mallocz(s->oformat->priv_data_size);
        if (!s->priv_data)
            goto nomem;
        if (s->oformat->priv_class)
        {
            *(const AVClass**)s->priv_data= s->oformat->priv_class;
            av_opt_set_defaults(s->priv_data);
        }
    }
    else
        s->priv_data = nullptr;

    if (filename)
        av_strlcpy(s->filename, filename, sizeof(s->filename));
    *avctx = s;
    return 0;
nomem:
    av_log(s, AV_LOG_ERROR, "Out of memory\n");
    ret = AVERROR(ENOMEM);
error:
    avformat_free_context(s);
    return ret;
}

const char *avcodec_get_name(enum AVCodecID id)
{
    const AVCodecDescriptor *cd;
    AVCodec *codec;

    if (id == AV_CODEC_ID_NONE)
        return "none";
    cd = avcodec_descriptor_get(id);
    if (cd)
        return cd->name;
    av_log(nullptr, AV_LOG_WARNING, "Codec 0x%x is not in the full list.\n", id);
    codec = avcodec_find_decoder(id);
    if (codec)
        return codec->name;
    codec = avcodec_find_encoder(id);
    if (codec)
        return codec->name;
    return "unknown_codec";
}
#endif

void init_id3v1(ID3v1 *id3v1)
{
    // Initialise ID3v1.1 tag structure
    memset(id3v1, ' ', sizeof(ID3v1));
    memcpy(&id3v1->m_tag, "TAG", 3);
    id3v1->m_padding = '\0';
    id3v1->m_title_no = 0;
    id3v1->m_genre = 0;
}

std::string format_number(int64_t value)
{
    if (!value)
    {
        return "unlimited";
    }

    if (value == static_cast<int64_t>(AV_NOPTS_VALUE))
    {
        return "unset";
    }

    return string_format("%" PRId64, value);
}

std::string format_bitrate(BITRATE value)
{
    if (value == static_cast<BITRATE>(AV_NOPTS_VALUE))
    {
        return "unset";
    }

    if (value > 1000000)
    {
        return string_format("%.2f Mbps", static_cast<double>(value) / 1000000);
    }
    else if (value > 1000)
    {
        return string_format("%.1f kbps", static_cast<double>(value) / 1000);
    }
    else
    {
        return string_format("%" PRId64 " bps", value);
    }
}

std::string format_samplerate(unsigned int value)
{
    if (value == static_cast<unsigned int>(AV_NOPTS_VALUE))
    {
        return "unset";
    }

    if (value < 1000)
    {
        return string_format("%u Hz", value);
    }
    else
    {
        return string_format("%.3f kHz", static_cast<double>(value) / 1000);
    }
}

std::string format_duration(int64_t value, int fracs /*= 1*/)
{
    if (value == AV_NOPTS_VALUE)
    {
        return "unset";
    }

    std::string buffer;
    int hours   = static_cast<int>((value / AV_TIME_BASE) / (3600));
    int mins    = static_cast<int>(((value / AV_TIME_BASE) % 3600) / 60);
    int secs    = static_cast<int>((value / AV_TIME_BASE) % 60);

    if (hours)
    {
        buffer = string_format("%02i:", hours);
    }

    buffer += string_format("%02i:%02i", mins, secs);
    if (fracs)
    {
        int decimals    = static_cast<int>(value % AV_TIME_BASE);
        buffer += string_format(".%5i", decimals).substr(0, fracs + 1);
    }
    return buffer;
}

std::string format_time(time_t value)
{
    if (!value)
    {
        return "unlimited";
    }

    if (value == static_cast<time_t>(AV_NOPTS_VALUE))
    {
        return "unset";
    }

    std::string buffer;
    int weeks;
    int days;
    int hours;
    int mins;
    int secs;

    weeks = static_cast<int>(value / (60*60*24*7));
    value -= weeks * (60*60*24*7);
    days = static_cast<int>(value / (60*60*24));
    value -= days * (60*60*24);
    hours = static_cast<int>(value / (60*60));
    value -= hours * (60*60);
    mins = static_cast<int>(value / (60));
    value -= mins * (60);
    secs = static_cast<int>(value);

    if (weeks)
    {
        buffer = string_format("%iw ", weeks);
    }
    if (days)
    {
        buffer += string_format("%id ", days);
    }
    if (hours)
    {
        buffer += string_format("%ih ", hours);
    }
    if (mins)
    {
        buffer += string_format("%im ", mins);
    }
    if (secs)
    {
        buffer += string_format("%is ", secs);
    }
    return buffer;
}

std::string format_size(size_t value)
{
    if (!value)
    {
        return "unlimited";
    }

    if (value == static_cast<size_t>(AV_NOPTS_VALUE))
    {
        return "unset";
    }

    if (value > 1024*1024*1024*1024LL)
    {
        return string_format("%.3f TB", static_cast<double>(value) / (1024*1024*1024*1024LL));
    }
    else if (value > 1024*1024*1024)
    {
        return string_format("%.2f GB", static_cast<double>(value) / (1024*1024*1024));
    }
    else if (value > 1024*1024)
    {
        return string_format("%.1f MB", static_cast<double>(value) / (1024*1024));
    }
    else if (value > 1024)
    {
        return string_format("%.1f KB", static_cast<double>(value) / (1024));
    }
    else
    {
        return string_format("%zu bytes", value);
    }
}

std::string format_size_ex(size_t value)
{
    return format_size(value) + string_format(" (%zu bytes)", value);
}

static void print_fps(double d, const char *postfix)
{
    uint64_t v = lrint(d * 100);
    if (!v)
    {
        std::printf("%1.4f %s\n", d, postfix);
    }
    else if (v % 100)
    {
        std::printf("%3.2f %s\n", d, postfix);
    }
    else if (v % (100 * 1000))
    {
        std::printf("%1.0f %s\n", d, postfix);
    }
    else
    {
        std::printf("%1.0fk %s\n", d / 1000, postfix);
    }
}

int print_info(const AVStream* stream)
{
    int ret = 0;

#if LAVF_DEP_AVSTREAM_CODEC
    AVCodecContext *avctx = avcodec_alloc_context3(nullptr);
    if (!avctx)
    {
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(avctx, stream->codecpar);
    if (ret < 0)
    {
        avcodec_free_context(&avctx);
        return ret;
    }

    // Fields which are missing from AVCodecParameters need to be taken from the AVCodecContext
    //            avctx->properties   = output_stream->codec->properties;
    //            avctx->codec        = output_stream->codec->codec;
    //            avctx->qmin         = output_stream->codec->qmin;
    //            avctx->qmax         = output_stream->codec->qmax;
    //            avctx->coded_width  = output_stream->codec->coded_width;
    //            avctx->coded_height = output_stream->codec->coded_height;
#else
    AVCodecContext *avctx = stream->codec;
#endif
    int fps = stream->avg_frame_rate.den && stream->avg_frame_rate.num;
#ifndef USING_LIBAV
    int tbr = stream->r_frame_rate.den && stream->r_frame_rate.num;
#endif
    int tbn = stream->time_base.den && stream->time_base.num;
    int tbc = avctx->time_base.den && avctx->time_base.num; // Even the currently latest (lavf 58.10.100) refers to AVStream codec->time_base member... (See dump.c dump_stream_format)

    if (fps)
        print_fps(av_q2d(stream->avg_frame_rate), "avg fps");
#ifndef USING_LIBAV
    if (tbr)
        print_fps(av_q2d(stream->r_frame_rate), "Real base framerate (tbr)");
#endif
    if (tbn)
        print_fps(1 / av_q2d(stream->time_base), "stream timebase (tbn)");
    if (tbc)
        print_fps(1 / av_q2d(avctx->time_base), "codec timebase (tbc)");

#if LAVF_DEP_AVSTREAM_CODEC
    avcodec_free_context(&avctx);
#endif

    return ret;
}

void exepath(std::string * path)
{
    char result[PATH_MAX + 1] = "";
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    if (count != -1)
    {
        *path = dirname(result);
        append_sep(path);
    }
    else
    {
        path->clear();
    }
}

// trim from start
std::string &ltrim(std::string &s)
{
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
    return s;
}

// trim from end
std::string &rtrim(std::string &s)
{
    s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
    return s;
}

// trim from both ends
std::string &trim(std::string &s)
{
    return ltrim(rtrim(s));
}

std::string replace_all(std::string str, const std::string& from, const std::string& to)
{
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
    }
    return str;
}

int strcasecmp(const std::string & s1, const std::string & s2)
{
    return strcasecmp(s1.c_str(), s2.c_str());
}

template<typename ... Args>
std::string string_format(const std::string& format, Args ... args)
{
    size_t size = snprintf(nullptr, 0, format.c_str(), args ...) + 1; // Extra space for '\0'
    std::unique_ptr<char[]> buf(new char[size]);
    std::snprintf(buf.get(), size, format.c_str(), args ...);
    return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

// Compare value with pattern
// Returns:
// -1 if pattern is no valid regex
// 0 if pattern matches
// 1 if not
int compare(const std::string & value, const std::string & pattern)
{
    regex_t regex;
    int reti;

    reti = regcomp(&regex, pattern.c_str(), REG_EXTENDED | REG_ICASE);
    if (reti)
    {
        std::fprintf(stderr, "Could not compile regex\n");
        return -1;
    }

    reti = regexec(&regex, value.c_str(), 0, nullptr, 0);

    regfree(&regex);

    return reti;
}

const std::string & expand_path(std::string *tgt, const std::string & src)
{
    wordexp_t exp_result;
    if (!wordexp(replace_all(src, " ", "\\ ").c_str(), &exp_result, 0))
    {
        *tgt = exp_result.we_wordv[0];
        wordfree(&exp_result);
    }
    else
    {
        *tgt =  src;
    }

    return *tgt;
}

int is_mount(const std::string & filename)
{
    char* orig_name = nullptr;
    int ret = 0;

    try
    {
        struct stat file_stat;
        struct stat parent_stat;
        char * parent_name = nullptr;

        orig_name = new_strdup(filename);

        // get the parent directory of the file
        parent_name = dirname(orig_name);

        // get the file's stat info
        if (-1 == stat(filename.c_str(), &file_stat))
        {
            std::fprintf(stderr, "is_mount(): %s\n", strerror(errno));
            throw -1;
        }

        //determine whether the supplied file is a directory
        // if it isn't, then it can't be a mountpoint.
        if (!(file_stat.st_mode & S_IFDIR))
        {
            std::fprintf(stderr, "is_mount(): %s is not a directory.\n", filename.c_str());
            throw -1;
        }

        // get the parent's stat info
        if (-1 == stat(parent_name, &parent_stat))
        {
            std::fprintf(stderr, "is_mount(): %s\n", strerror(errno));
            throw -1;
        }

        // if file and parent have different device ids,
        // then the file is a mount point
        // or, if they refer to the same file,
        // then it's probably the root directory
        // and therefore a mountpoint
        if (file_stat.st_dev != parent_stat.st_dev ||
                (file_stat.st_dev == parent_stat.st_dev &&
                 file_stat.st_ino == parent_stat.st_ino))
        {
            // IS a mountpoint
            ret = 1;
        }
        else
        {
            // is NOT a mountpoint
            ret = 0;
        }
    }
    catch (int _ret)
    {
        ret = _ret;
    }

    delete [] orig_name;

    return ret;
}

std::vector<std::string> split(const std::string& input, const std::string & regex)
{
    // passing -1 as the submatch index parameter performs splitting
    std::regex re(regex);
    std::sregex_token_iterator
            first{input.begin(), input.end(), re, -1},
    last;
    return {first, last};
}

std::string sanitise_name(const std::string & filename)
{
    char resolved_name[PATH_MAX + 1];

    if (realpath(filename.c_str(), resolved_name) != nullptr)
    {
        return resolved_name;
    }
    return filename;
}

bool is_album_art(AVCodecID codec_id)
{
    return (codec_id == AV_CODEC_ID_MJPEG || codec_id == AV_CODEC_ID_PNG || codec_id == AV_CODEC_ID_BMP);
}

size_t get_disk_size(std::string & file)
{
    struct statvfs buf;

    if (statvfs(file.c_str(), &buf))
    {
        return 0;
    }

    return (buf.f_bfree * buf.f_bsize);
}


