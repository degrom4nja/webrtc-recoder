#pragma once

#ifndef WEBM_ENCODER_H_
#define WEBM_ENCODER_H_

extern "C" {
#include "opus.h"
#include "vpx/vpx_encoder.h"
#include "vpx/vp8cx.h"
}

#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"

#define MAX_FRAME_SIZE 6*960
#define MAX_PACKET_SIZE 3828

namespace webrtc
{
	class WebmEncoder
	{
	private:
		mkvmuxer::MkvWriter* mkv_writer;
		mkvmuxer::Segment* muxer_segment;
		mkvmuxer::SegmentInfo* segment_info;

		OpusEncoder* opus_encoder;
		uint64_t audio_track;
		opus_int16 pcm[MAX_FRAME_SIZE];
		uint8_t buffer[MAX_PACKET_SIZE];

		vpx_codec_ctx_t vpx_ctx;
		vpx_codec_pts_t vpx_pts;
		vpx_image_t raw;
		uint64_t video_track;
		int keyframe_interval;
		const double NANOS = 1000000000.0;
		int fps_num;
		int fps_den;
		int frame_cnt;
		uint64_t s_nanos;
		uint64_t l_nanos;
		int width;
		int height;

		char output[128];
		int bit_rate;
		double frame_rate;
		int sample_rate;
		int channels;
		bool initialized_;

		int audio_encoder_create(uint8_t* header, int *preskip);
		int video_encoder_create(int width, int height);
		int vpx_encode_frame(vpx_codec_ctx_t* codec, vpx_image_t* img, vpx_codec_pts_t pts, int flags);
		void opus_encode_header(uint8_t* header, int sample_rate, int channels, int *preskip);

	public:
		int create(int width, int height);
		bool initialize(const char* output, int bitrate, double frame_rate, int sample_rate, int channels);
		int audio_on_data(const uint8_t* audio_data, int bits_per_sample, int sample_rate, size_t number_of_channels, size_t number_of_frames);
		int video_on_frame(uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int strideY, int strideU, int strideV, int width, int height);
		int webm_encoder_destroy();
		bool initialized() { return initialized_; }
		bool ready;

		WebmEncoder();
		~WebmEncoder();
	};
}
#endif //WEBM_ENCODER_H_