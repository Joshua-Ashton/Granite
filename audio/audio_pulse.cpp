/* Copyright (c) 2017-2022 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "audio_pulse.hpp"
#include <pulse/pulseaudio.h>
#include "dsp/dsp.hpp"
#include "logging.hpp"
#include <string.h>

static constexpr size_t MAX_NUM_SAMPLES = 256;

namespace Granite
{
namespace Audio
{
struct Pulse final : Backend
{
	explicit Pulse(BackendCallback *callback_)
		: Backend(callback_)
	{
	}

	~Pulse() override;

	bool init(float sample_rate_, unsigned channels_);
	bool start() override;
	bool stop() override;

	bool get_buffer_status(size_t &write_avail, size_t &write_avail_frames, uint32_t &latency_usec) override;
	size_t write_frames_interleaved(const float *data, size_t frames, bool blocking) override;

	const char *get_backend_name() override
	{
		return "pulse";
	}

	float get_sample_rate() override
	{
		return sample_rate;
	}

	unsigned get_num_channels() override
	{
		return channels;
	}

	float sample_rate = 0.0f;
	unsigned channels = 0;

	pa_threaded_mainloop *mainloop = nullptr;
	pa_context *context = nullptr;
	pa_stream *stream = nullptr;
	size_t buffer_frames = 0;
	int success = -1;
	bool has_success = false;
	bool is_active = false;

	void update_buffer_attr(const pa_buffer_attr &attr) noexcept;
	size_t to_frames(size_t size) const noexcept;
};

Pulse::~Pulse()
{
	if (is_active)
		stop();

	if (mainloop)
		pa_threaded_mainloop_stop(mainloop);

	if (stream)
	{
		pa_stream_disconnect(stream);
		pa_stream_unref(stream);
	}

	if (context)
	{
		pa_context_disconnect(context);
		pa_context_unref(context);
	}

	if (mainloop)
		pa_threaded_mainloop_free(mainloop);
}

static void stream_success_cb(pa_stream *, int success, void *data)
{
	auto *pa = static_cast<Pulse *>(data);
	pa->success = success;
	pa->has_success = true;
	pa_threaded_mainloop_signal(pa->mainloop, 0);
}

static void context_state_cb(pa_context *, void *data)
{
	auto *pa = static_cast<Pulse *>(data);
	pa_threaded_mainloop_signal(pa->mainloop, 0);
}

static void stream_state_cb(pa_stream *, void *data)
{
	auto *pa = static_cast<Pulse *>(data);
	pa_threaded_mainloop_signal(pa->mainloop, 0);
}

static void stream_buffer_attr_cb(pa_stream *s, void *data)
{
	auto *pa = static_cast<Pulse *>(data);
	auto *server_attr = pa_stream_get_buffer_attr(s);
	if (server_attr)
		pa->update_buffer_attr(*server_attr);
}

size_t Pulse::write_frames_interleaved(const float *data, size_t frames, bool blocking)
{
	if (callback)
		return 0;

	size_t written_frames = 0;
	pa_threaded_mainloop_lock(mainloop);

	while (frames)
	{
		size_t to_write = std::min(frames, to_frames(pa_stream_writable_size(stream)));
		if (to_write)
		{
			if (pa_stream_write(stream, data, to_write * channels * sizeof(float), nullptr, 0, PA_SEEK_RELATIVE))
			{
				LOGE("Failed to write to pulse stream.\n");
				break;
			}

			data += to_write * channels;
			written_frames += to_write;
			frames -= to_write;
		}
		else if (blocking)
			pa_threaded_mainloop_wait(mainloop);
		else
			break;
	}

	pa_threaded_mainloop_unlock(mainloop);
	return written_frames;
}

static void stream_request_cb(pa_stream *s, size_t length, void *data)
{
	auto *pa = static_cast<Pulse *>(data);
	auto *cb = pa->get_callback();

	// If we're not doing pull-based audio, just wake up main thread.
	if (!cb)
	{
		pa_threaded_mainloop_signal(pa->mainloop, 0);
		return;
	}

	// For callback based audio, render out audio immediately as requested.

	float mix_channels[Backend::MaxAudioChannels][MAX_NUM_SAMPLES];
	float *mix_channel_ptr[Backend::MaxAudioChannels];
	for (unsigned i = 0; i < pa->channels; i++)
		mix_channel_ptr[i] = mix_channels[i];

	void *out_data;
	if (pa_stream_begin_write(s, &out_data, &length) < 0)
	{
		LOGE("pa_stream_begin_write() failed.\n");
		return;
	}

	auto *out_interleaved = static_cast<float *>(out_data);
	size_t out_frames = pa->to_frames(length);
	unsigned channels = pa->channels;

	if (pa->is_active)
	{
		while (out_frames != 0)
		{
			size_t to_write = std::min<size_t>(out_frames, MAX_NUM_SAMPLES);
			cb->mix_samples(mix_channel_ptr, to_write);
			out_frames -= to_write;

			if (channels == 2)
			{
				DSP::interleave_stereo_f32(out_interleaved, mix_channels[0], mix_channels[1], to_write);
				out_interleaved += to_write * channels;
			}
			else
			{
				for (size_t f = 0; f < to_write; f++)
					for (unsigned c = 0; c < channels; c++)
						*out_interleaved++ = mix_channels[c][f];
			}
		}
	}
	else
		memset(out_interleaved, 0, sizeof(float) * channels * out_frames);

	if (pa_stream_write(s, out_data, length, nullptr, 0, PA_SEEK_RELATIVE) < 0)
	{
		LOGE("pa_stream_write() failed.\n");
		return;
	}

	// Update latency information.
	pa_usec_t latency_usec;
	int negative = 0;
	if (pa_stream_get_latency(s, &latency_usec, &negative) != 0)
		latency_usec = 0;
	if (negative)
		latency_usec = 0;

	cb->set_latency_usec(uint32_t(latency_usec));
}

size_t Pulse::to_frames(size_t size) const noexcept
{
	return size / (channels * sizeof(float));
}

void Pulse::update_buffer_attr(const pa_buffer_attr &attr) noexcept
{
	buffer_frames = to_frames(attr.tlength);
}

bool Pulse::init(float sample_rate_, unsigned channels_)
{
	sample_rate = sample_rate_;
	channels = channels_;

	if (channels_ > MaxAudioChannels)
		return false;

	mainloop = pa_threaded_mainloop_new();
	if (!mainloop)
		return false;

	context = pa_context_new(pa_threaded_mainloop_get_api(mainloop), "Granite");
	if (!context)
		return false;

	pa_context_set_state_callback(context, context_state_cb, this);

	if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
		return false;

	pa_threaded_mainloop_lock(mainloop);
	if (pa_threaded_mainloop_start(mainloop) < 0)
		return false;

	while (pa_context_get_state(context) < PA_CONTEXT_READY)
		pa_threaded_mainloop_wait(mainloop);

	if (pa_context_get_state(context) != PA_CONTEXT_READY)
	{
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}

	pa_sample_spec spec = {};
	spec.format = PA_SAMPLE_FLOAT32NE;
	spec.channels = uint8_t(channels_);
	spec.rate = uint32_t(sample_rate_);

	stream = pa_stream_new(context, "audio", &spec, nullptr);
	if (!stream)
	{
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}

	pa_stream_set_state_callback(stream, stream_state_cb, this);
	pa_stream_set_write_callback(stream, stream_request_cb, this);
	pa_stream_set_buffer_attr_callback(stream, stream_buffer_attr_cb, this);

	pa_buffer_attr buffer_attr = {};
	buffer_attr.maxlength = -1u;
	buffer_attr.tlength = pa_usec_to_bytes(50000, &spec);
	buffer_attr.prebuf = -1u;
	buffer_attr.minreq = -1u;
	buffer_attr.fragsize = -1u;
	update_buffer_attr(buffer_attr);

	if (pa_stream_connect_playback(stream, nullptr, &buffer_attr,
	                               static_cast<pa_stream_flags_t>(PA_STREAM_AUTO_TIMING_UPDATE |
	                                                              PA_STREAM_ADJUST_LATENCY |
	                                                              PA_STREAM_INTERPOLATE_TIMING |
	                                                              PA_STREAM_FIX_RATE |
	                                                              PA_STREAM_START_CORKED),
	                               nullptr, nullptr) < 0)
	{
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}

	while (pa_stream_get_state(stream) < PA_STREAM_READY)
		pa_threaded_mainloop_wait(mainloop);

	if (pa_stream_get_state(stream) != PA_STREAM_READY)
	{
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}

	auto *stream_spec = pa_stream_get_sample_spec(stream);
	this->sample_rate = float(stream_spec->rate);
	if (callback)
		callback->set_backend_parameters(this->sample_rate, channels_, MAX_NUM_SAMPLES);

	if (const auto *attr = pa_stream_get_buffer_attr(stream))
		update_buffer_attr(*attr);

	pa_threaded_mainloop_unlock(mainloop);
	return true;
}

bool Pulse::start()
{
	if (is_active)
		return false;

	has_success = false;
	pa_threaded_mainloop_lock(mainloop);
	if (callback)
		callback->on_backend_start();
	pa_stream_cork(stream, 0, stream_success_cb, this);

	while (!has_success)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);

	is_active = true;
	has_success = false;
	if (success < 0)
		LOGE("Pulse::start() failed.\n");
	return success >= 0;
}

bool Pulse::stop()
{
	if (!is_active)
		return false;

	has_success = false;
	pa_threaded_mainloop_lock(mainloop);
	pa_stream_cork(stream, 1, stream_success_cb, this);

	while (!has_success)
		pa_threaded_mainloop_wait(mainloop);

	if (callback)
		callback->on_backend_stop();
	pa_threaded_mainloop_unlock(mainloop);

	is_active = false;
	has_success = false;
	if (success < 0)
		LOGE("Pulse::stop() failed.\n");
	return success >= 0;
}

bool Pulse::get_buffer_status(size_t &write_avail, size_t &max_write_avail, uint32_t &latency_usec)
{
	pa_threaded_mainloop_lock(mainloop);
	size_t writable_size = pa_stream_writable_size(stream);

	// Update latency information.
	pa_usec_t usec;
	int negative = 0;
	if (pa_stream_get_latency(stream, &usec, &negative) != 0)
		usec = 0;
	if (negative)
		usec = 0;
	latency_usec = usec;

	pa_threaded_mainloop_unlock(mainloop);

	write_avail = to_frames(writable_size);
	max_write_avail = buffer_frames;

	if (write_avail > max_write_avail)
		LOGW("Write avail %zu > max write avail %zu?\n", write_avail, max_write_avail);

	return true;
}

Backend *create_pulse_backend(BackendCallback *callback, float sample_rate, unsigned channels)
{
	auto *backend = new Pulse(callback);
	if (!backend->init(sample_rate, channels))
	{
		delete backend;
		return nullptr;
	}

	return backend;
}
}
}
