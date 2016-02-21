//
//    Copyright (C) 2016 Haruki Hasegawa
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//

#define LOG_TAG "AudioTrackStream"

#include "oslmp/impl/AudioTrackStream.hpp"

#include <new>
#include <cmath>
#include <cxxporthelper/compiler.hpp>
#include <jni_utils/jni_utils.hpp>
#include <loghelper/loghelper.h>

#include <oslmp/OpenSLMediaPlayerResultCodes.hpp>

#include "oslmp/impl/AudioTrack.hpp"
#include "oslmp/impl/AudioFormat.hpp"

namespace oslmp {
namespace impl {

class LocalByteBuffer {
public:
    LocalByteBuffer(JNIEnv *env, void *buffer, size_t size)
        : env_(env), buffer_(buffer), size_(size), bb_(0), m_rewind_(0)
    {
        bb_ = env_->NewDirectByteBuffer(buffer, static_cast<jlong>(size));

        {
            jclass cls = env_->FindClass("java/nio/Buffer");
            m_rewind_ = env_->GetMethodID(cls, "rewind", "()Ljava/nio/Buffer;");
            env_->DeleteLocalRef(cls);
        }
    }

    ~LocalByteBuffer() {
        if (env_ && bb_) {
            env_->DeleteLocalRef(bb_);
        }
    }

    bool is_valid() const noexcept {
        return (bb_ != 0);
    }

    size_t size() const noexcept {
        return size_;
    }

    void rewind() noexcept {
        jobject bb2 = env_->CallObjectMethod(bb_, m_rewind_);
        env_->DeleteLocalRef(bb2);
    }

    jobject get() const noexcept {
        return bb_;
    }

private:
    JNIEnv *env_;
    void *buffer_;
    size_t size_;
    jobject bb_;
    jmethodID m_rewind_;
};

AudioTrackStream::AudioTrackStream()
    : vm_(nullptr), env_(nullptr), track_(nullptr), pt_handle_(0), 
    buffer_size_in_frames_(0), buffer_block_count_(0),
    callback_func_(nullptr), callback_args_(nullptr), stop_request_(false)
{
}

AudioTrackStream::~AudioTrackStream()
{
    if (pt_handle_) {
        stop_request_ = true;
        (void) ::pthread_join(pt_handle_, nullptr);
    }

    if (env_ && track_) {
        track_->release(env_);
        track_.reset();
    }

    vm_ = nullptr;
}

int AudioTrackStream::init(
    JNIEnv *env, int stream_type, sample_format_type format, 
    uint32_t sample_rate_in_hz, uint32_t num_channels, 
    uint32_t buffer_size_in_frames, uint32_t buffer_block_count) noexcept
{
    JavaVM *vm = nullptr;
    int32_t encoding;

    switch (format) {
    case kAudioSampleFormatType_S16:
        encoding = AudioFormat::ENCODING_PCM_16BIT;
        break;
    case kAudioSampleFormatType_F32:
        encoding = AudioFormat::ENCODING_PCM_FLOAT;
        break;
    default:
        return OSLMP_RESULT_ILLEGAL_ARGUMENT;
    }

    if (num_channels != 2) {
        return OSLMP_RESULT_ILLEGAL_ARGUMENT;
    }

    if (env->GetJavaVM(&vm) != JNI_OK) {
        return OSLMP_RESULT_INTERNAL_ERROR;
    }

    std::unique_ptr<AudioTrack> track(new(std::nothrow) AudioTrack());

    if (!(track && track->create(
        env, stream_type, sample_rate_in_hz, num_channels, encoding, 
        (buffer_size_in_frames * buffer_block_count * 1), AudioTrack::MODE_STREAM, 0))) {
        return OSLMP_RESULT_INTERNAL_ERROR;
    }

    vm_ = vm;
    env_ = env;
    track_ = std::move(track);
    buffer_size_in_frames_ = buffer_size_in_frames;
    buffer_block_count_ = buffer_block_count;

    return OSLMP_RESULT_SUCCESS;
}

int AudioTrackStream::start(render_callback_func_t callback, void *args) noexcept
{
    LOGD("AudioTrackStream::start");

    if (!callback) {
        return OSLMP_RESULT_ILLEGAL_ARGUMENT;
    }
    if (!track_) {
        return OSLMP_RESULT_ILLEGAL_STATE;
    }

    pthread_t pt_handle;
    int pt_create_result;

    callback_func_ = callback;
    callback_args_ = args;
    pt_create_result = ::pthread_create(&pt_handle, nullptr, &AudioTrackStream::sinkWriterThreadEntryFunc, this);

    if (pt_create_result != 0) {
        callback_func_ = nullptr;
        callback_args_ = nullptr;
        return OSLMP_RESULT_INTERNAL_ERROR;
    }

    pt_handle_ = pt_handle;

    return OSLMP_RESULT_SUCCESS;
}

int AudioTrackStream::stop() noexcept
{
    LOGD("AudioTrackStream::stop");
    if (!pt_handle_) {
        return OSLMP_RESULT_SUCCESS;
    }

    stop_request_ = true;
    ::pthread_join(pt_handle_, nullptr);
    pt_handle_ = 0;
    callback_func_ = nullptr;
    callback_args_ = nullptr;
    stop_request_ = false;

    return OSLMP_RESULT_SUCCESS;
}

void* AudioTrackStream::sinkWriterThreadEntryFunc(void *args) noexcept
{
    LOGD("AudioTrackStream::sinkWriterThreadEntryFunc");

    AudioTrackStream *thiz = static_cast<AudioTrackStream*>(args);
    JNIEnv *env = nullptr;

    if (thiz->vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        thiz->sinkWriterThreadProcess(env);

        thiz->vm_->DetachCurrentThread();
        env = nullptr;
    }

    return nullptr;
}

void AudioTrackStream::sinkWriterThreadProcess(JNIEnv *env) noexcept
{
    LOGD("AudioTrackStream::sinkWriterThreadProcess");

    const int32_t format = track_->getAudioFormat();

    const int32_t play_result = track_->play(env);
    const bool supports_bb = track_->supportsByteBufferMethods();

    if (play_result == AudioTrack::SUCCESS) {
        switch (format) {
            case AudioFormat::ENCODING_PCM_16BIT:
                if (supports_bb) {
                    sinkWriterThreadLoopS16ByteBuffer(env);
                } else {
                    sinkWriterThreadLoopS16(env);
                }
                break;
            case AudioFormat::ENCODING_PCM_FLOAT:
                if (supports_bb) {
                    sinkWriterThreadLoopFloatByteBuffer(env);
                } else {
                    sinkWriterThreadLoopFloat(env);
                }
                break;
            default:
                break;
        }

        track_->pause(env);
        track_->stop(env);
    }
}

void AudioTrackStream::sinkWriterThreadLoopS16(JNIEnv *env) noexcept
{
    const sample_format_type format = kAudioSampleFormatType_S16;
    const int32_t num_channels = track_->getChannelCount();
    const int32_t buffer_size_in_frames = buffer_size_in_frames_;
    const int32_t num_data_write = num_channels * buffer_size_in_frames;

    jshortArray buffer = env->NewShortArray(num_channels * buffer_size_in_frames);

    if (!buffer) {
        return;
    }

    jlocal_ref_wrapper<jshortArray> buffer_ref;
    buffer_ref.assign(env, buffer, jref_type::local_reference);

    while (CXXPH_UNLIKELY(!stop_request_)) {
        {
            jshort_array buffer_array(env, buffer);
            (*callback_func_)(buffer_array.data(), format, num_channels, buffer_size_in_frames, callback_args_);
        }

        const int32_t write_result = track_->write(env, buffer, 0, num_data_write);

        if (write_result != num_data_write) {
            break;
        }
    }
}

void AudioTrackStream::sinkWriterThreadLoopFloat(JNIEnv *env) noexcept
{
    const sample_format_type format = kAudioSampleFormatType_F32;
    const int32_t num_channels = track_->getChannelCount();
    const int32_t buffer_size_in_frames = buffer_size_in_frames_;
    const int32_t num_data_write = num_channels * buffer_size_in_frames;

    jfloatArray buffer = env->NewFloatArray(num_channels * buffer_size_in_frames);

    if (!buffer) {
        return;
    }

    jlocal_ref_wrapper<jfloatArray> buffer_ref;
    buffer_ref.assign(env, buffer, jref_type::local_reference);

    {
        jfloat_array buffer_array(env, buffer);

        double delta = 2 * M_PI * 16 / buffer_size_in_frames;

        float *p = buffer_array.data();
        for (int i = 0; i < buffer_size_in_frames_; ++i)
        {
            p[2 * i + 0] = p[2 * i + 1] = static_cast<float>(std::sin(delta * i));
        }
    }

    while (CXXPH_UNLIKELY(!stop_request_)) {
        // {
        //     jfloat_array data_array(env, buffer);
        //     (void) (*callback_func_)(buffer_array.data(), format, num_channels, buffer_size_in_frames, callback_args_);
        // }

        const int32_t write_result = track_->write(env, buffer, 0, num_data_write, AudioTrack::WRITE_BLOCKING);

        if (write_result != num_data_write) {
            break;
        }
    }
}

void AudioTrackStream::sinkWriterThreadLoopS16ByteBuffer(JNIEnv *env) noexcept
{
    const sample_format_type format = kAudioSampleFormatType_S16;
    const int32_t num_channels = track_->getChannelCount();
    const int32_t buffer_size_in_frames = buffer_size_in_frames_;
    const int32_t num_data_write = num_channels * buffer_size_in_frames;
    const size_t num_data_write_in_bytes = num_data_write * sizeof(int16_t);
    std::unique_ptr<int16_t[]> buffer(new int16_t[num_data_write]);

    LocalByteBuffer bb(env, buffer.get(), num_data_write_in_bytes);

    if (!bb.is_valid()) {
        return;
    }

    while (CXXPH_UNLIKELY(!stop_request_)) {
        (*callback_func_)(buffer.get(), format, num_channels, buffer_size_in_frames, callback_args_);

        const int32_t write_result = track_->write(env, bb.get(), bb.size(), AudioTrack::WRITE_BLOCKING);

        if (write_result != bb.size()) {
            break;
        }

        bb.rewind();
    }
}

void AudioTrackStream::sinkWriterThreadLoopFloatByteBuffer(JNIEnv *env) noexcept
{
    const sample_format_type format = kAudioSampleFormatType_F32;
    const int32_t num_channels = track_->getChannelCount();
    const int32_t buffer_size_in_frames = buffer_size_in_frames_;
    const int32_t num_data_write = num_channels * buffer_size_in_frames;
    const size_t num_data_write_in_bytes = num_data_write * sizeof(float);
    std::unique_ptr<float[]> buffer(new float[num_data_write]);

    LocalByteBuffer bb(env, buffer.get(), num_data_write_in_bytes);

    if (!bb.is_valid()) {
        return;
    }

    while (CXXPH_UNLIKELY(!stop_request_)) {
        (*callback_func_)(buffer.get(), format, num_channels, buffer_size_in_frames, callback_args_);

        const int32_t write_result = track_->write(env, bb.get(), bb.size(), AudioTrack::WRITE_BLOCKING);

        if (write_result != bb.size()) {
            break;
        }

        bb.rewind();
    }
}

}
}
