// SPDX-License-Identifier: MIT
// AAC-LC through the Media Foundation AAC encoder that ships with Windows.
// Output is wrapped in LATM (muxConfigPresent = 1) as A2DP requires.
// Media Foundation is absent on "N" editions without the Media Feature Pack;
// creation then fails and the service falls back to the next codec.
#ifdef OA2DP_WITH_MFAAC

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

#include "oa2dp/encoder.h"
#include "oa2dp/media.h"

namespace {

// CLSID_AACMFTEncoder {93AF0C51-2275-45d2-A35B-F2BA21CAED00}
const GUID kClsidAacEncoder = {0x93af0c51, 0x2275, 0x45d2,
                               {0xa3, 0x5b, 0xf2, 0xba, 0x21, 0xca, 0xed, 0x00}};

constexpr UINT32 kSamplesPerFrame = 1024;

template <class T> void release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct AacState {
    IMFTransform *mft = nullptr;
    bool mf_started = false;
    bool com_initialized = false;
    UINT32 out_buffer_size = 0;
    bool mft_allocates = false;
    LONGLONG next_time = 0;  // 100 ns units; the encoder rejects untimed samples
    std::deque<std::vector<uint8_t>> pending;  // raw AUs waiting to be sent
    std::vector<int16_t> pcm16;
};

// Standard-layout wrapper: the C side only sees `base`.
struct AacEncoder {
    oa2dp_encoder base;
    AacState *st;
};

HRESULT set_types(IMFTransform *mft, UINT32 rate, UINT32 channels, UINT32 bitrate)
{
    // The encoder accepts 12000/16000/20000/24000 bytes per second.
    static const UINT32 kRates[] = {24000, 20000, 16000, 12000};
    UINT32 bytes_per_sec = 12000;
    for (UINT32 r : kRates)
        if (r * 8 <= bitrate) {
            bytes_per_sec = r;
            break;
        }

    IMFMediaType *out = nullptr, *in = nullptr;
    HRESULT hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes_per_sec);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);  // raw AUs
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    // The AAC encoder requires the output type before the input type.
    if (SUCCEEDED(hr)) hr = mft->SetOutputType(0, out, 0);

    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * channels);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 2 * channels * rate);
    if (SUCCEEDED(hr)) hr = mft->SetInputType(0, in, 0);

    release(out);
    release(in);
    return hr;
}

// Pull every available output sample into `pending`.
HRESULT drain(AacState *x)
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER db = {};
        IMFSample *sample = nullptr;
        IMFMediaBuffer *buf = nullptr;
        if (!x->mft_allocates) {
            HRESULT hr = MFCreateSample(&sample);
            if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(x->out_buffer_size, &buf);
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
            if (FAILED(hr)) {
                release(buf);
                release(sample);
                return hr;
            }
            db.pSample = sample;
        }
        DWORD status = 0;
        HRESULT hr = x->mft->ProcessOutput(0, 1, &db, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            release(buf);
            release(sample);
            return S_OK;
        }
        if (SUCCEEDED(hr) && db.pSample) {
            IMFMediaBuffer *contig = nullptr;
            hr = db.pSample->ConvertToContiguousBuffer(&contig);
            if (SUCCEEDED(hr)) {
                BYTE *data = nullptr;
                DWORD len = 0;
                hr = contig->Lock(&data, nullptr, &len);
                if (SUCCEEDED(hr)) {
                    x->pending.emplace_back(data, data + len);
                    contig->Unlock();
                }
                release(contig);
            }
        }
        if (db.pEvents)
            db.pEvents->Release();
        if (x->mft_allocates && db.pSample)
            db.pSample->Release();
        release(buf);
        release(sample);
        if (FAILED(hr))
            return hr;
    }
}

int aac_encode(oa2dp_encoder *e, const int32_t *pcm, uint8_t *out, size_t cap,
               size_t *out_len, uint8_t *frames)
{
    AacState *x = reinterpret_cast<AacEncoder *>(e)->st;
    const UINT32 ch = e->info.channels;
    const DWORD bytes = kSamplesPerFrame * ch * 2;
    *out_len = 0;
    *frames = 0;

    oa2dp_pcm_to_s16(pcm, x->pcm16.data(), kSamplesPerFrame * ch);

    IMFSample *sample = nullptr;
    IMFMediaBuffer *buf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
    if (SUCCEEDED(hr)) {
        BYTE *dst = nullptr;
        hr = buf->Lock(&dst, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            std::memcpy(dst, x->pcm16.data(), bytes);
            buf->Unlock();
            hr = buf->SetCurrentLength(bytes);
        }
    }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
    const LONGLONG duration = 10000000LL * kSamplesPerFrame / e->info.sample_rate;
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(x->next_time);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    x->next_time += duration;
    if (SUCCEEDED(hr)) {
        hr = x->mft->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {  // drain, then retry once
            hr = drain(x);
            if (SUCCEEDED(hr)) hr = x->mft->ProcessInput(0, sample, 0);
        }
    }
    if (SUCCEEDED(hr)) hr = drain(x);
    release(buf);
    release(sample);
    if (FAILED(hr))
        return OA2DP_ERR_IO;

    if (x->pending.empty())
        return OA2DP_OK;  // encoder priming
    const std::vector<uint8_t> &au = x->pending.front();
    int n = oa2dp_latm_write(out, cap, au.data(), au.size(), e->info.sample_rate,
                             e->info.channels);
    x->pending.pop_front();
    if (n < 0)
        return n;
    *out_len = static_cast<size_t>(n);
    *frames = 1;
    return OA2DP_OK;
}

void aac_destroy(oa2dp_encoder *e)
{
    auto *enc = reinterpret_cast<AacEncoder *>(e);
    AacState *x = enc->st;
    if (x->mft) {
        x->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        x->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    release(x->mft);
    if (x->mf_started)
        MFShutdown();
    if (x->com_initialized)
        CoUninitialize();
    delete x;
    delete enc;
}

const oa2dp_encoder_ops kAacOps = {aac_encode, nullptr, nullptr, aac_destroy};

}  // namespace

extern "C" int oa2dp_aac_mf_encoder_create(const oa2dp_codec_caps *c, const oa2dp_settings *s,
                                           oa2dp_encoder **out)
{
    (void)s;
    if (c->codec != OA2DP_CODEC_AAC)
        return OA2DP_ERR_INVAL;
    const UINT32 rate = oa2dp_codec_sample_rate(c);
    const UINT32 ch = oa2dp_codec_channels(c);
    if (rate != 44100 && rate != 48000)
        return OA2DP_ERR_UNSUPPORTED;

    auto *enc = new (std::nothrow) AacEncoder();
    AacState *x = new (std::nothrow) AacState();
    if (!enc || !x) {
        delete enc;
        delete x;
        return OA2DP_ERR_NOSPC;
    }
    enc->base.ops = &kAacOps;
    enc->st = x;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    x->com_initialized = SUCCEEDED(hr);  // S_FALSE also needs CoUninitialize
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    x->mf_started = SUCCEEDED(hr);
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(kClsidAacEncoder, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&x->mft));
    if (SUCCEEDED(hr))
        hr = set_types(x->mft, rate, ch, c->u.aac.bitrate ? c->u.aac.bitrate : 192000);
    MFT_OUTPUT_STREAM_INFO info = {};
    if (SUCCEEDED(hr)) hr = x->mft->GetOutputStreamInfo(0, &info);
    if (SUCCEEDED(hr)) {
        x->mft_allocates = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                            MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        x->out_buffer_size = info.cbSize ? info.cbSize : 8192;
        hr = x->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    }
    if (SUCCEEDED(hr)) hr = x->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        aac_destroy(&enc->base);
        return OA2DP_ERR_UNSUPPORTED;
    }

    x->pcm16.resize(kSamplesPerFrame * ch);
    enc->base.info.codec = OA2DP_CODEC_AAC;
    enc->base.info.sample_rate = rate;
    enc->base.info.channels = static_cast<uint8_t>(ch);
    enc->base.info.samples_per_call = kSamplesPerFrame;
    enc->base.info.max_output = 8192;
    enc->base.info.levels = 1;
    enc->base.info.delay_us = static_cast<uint32_t>(2ull * kSamplesPerFrame * 1000000 / rate);
    *out = &enc->base;
    return OA2DP_OK;
}

#endif  // OA2DP_WITH_MFAAC
