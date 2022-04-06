#include "WebmEncoder.h"
#include <iostream>
#include <iomanip>
#include "mkvmuxer/mkvmuxerutil.h"
#include "rtc_base/time_utils.h"

namespace webrtc
{
	int WebmEncoder::audio_encoder_create(uint8_t *header, int *preskip)
	{
		int err;
		opus_encoder = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &err);
		if (err < 0)
			return 0;

		err = opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(64000));
		if (err < 0)
			goto fail1;

		err = opus_encoder_ctl(opus_encoder, OPUS_GET_LOOKAHEAD(preskip));
		if (err < 0)
			goto fail1;

		opus_encode_header(header, sample_rate, channels, preskip);

		return 1;

	fail1:
		opus_encoder_destroy(opus_encoder);
		return 0;
	}

	int WebmEncoder::video_encoder_create(int width, int height)
	{
		if (!vpx_img_alloc(&raw, VPX_IMG_FMT_I420, width, height, 1))
			return 0;

		vpx_codec_iface_t* iface = vpx_codec_vp8_cx();
		vpx_codec_enc_cfg_t cfg;
		vpx_codec_err_t res;
		if (vpx_codec_enc_config_default(iface, &cfg, 0))
			goto fail1;

		cfg.g_w = width;
		cfg.g_h = height;
		cfg.g_timebase.num = fps_num;
		cfg.g_timebase.den = fps_den;
		cfg.rc_target_bitrate = bit_rate;
		//cfg.g_bit_depth = VPX_BITS_8;

		if(vpx_codec_enc_init(&vpx_ctx, iface, &cfg, 0))
			goto fail1;

		//if (vpx_codec_control(&vpx_ctx, VP8E_SET_CQ_LEVEL, 10) != VPX_CODEC_OK)
			//goto fail2;

		return 1;

	fail2:
		vpx_codec_destroy(&vpx_ctx);
	fail1:
		vpx_img_free(&raw);
		return 0;
	}

	void WebmEncoder::opus_encode_header(uint8_t* header, int sample_rate, int channels, int *preskip)
	{
		memcpy(header, "OpusHead", 8);
		header[8] = 1;
		header[9] = channels;
		header[10] = *preskip & 0x000000ff;
		header[11] = *preskip >> 8 & 0x000000ff;
		header[12] = sample_rate & 0x000000ff;
		header[13] = sample_rate >> 8 & 0x000000ff;
		header[14] = sample_rate >> 16 & 0x000000ff;
		header[15] = sample_rate >> 24 & 0x000000ff;
		header[16] = header[17] = header[18] = 0;
	}

	int WebmEncoder::audio_on_data(const uint8_t* audio_data, int bits_per_sample, int sample_rate, size_t number_of_channels, size_t number_of_frames)
	{
		if (!ready || !l_nanos)
			return 0;

		size_t number_of_bytes = number_of_frames * number_of_channels;

		for (int i = 0; i < number_of_bytes; i++)
			pcm[i] = audio_data[2 * i + 1] << 8 | audio_data[2 * i];

		int bytes_size = opus_encode(opus_encoder, pcm, number_of_bytes, buffer, MAX_PACKET_SIZE);
		if (bytes_size < 1)
			return 0;

		uint64_t timestamp = rtc::SystemTimeNanos() - s_nanos;
		muxer_segment->AddFrame(buffer, bytes_size, audio_track, timestamp, true);
		return 1;
	}

	int WebmEncoder::video_on_frame(uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int strideY, int strideU, int strideV, int width, int height)
	{
		if (this->width != width || this->height != height)
			return -1;

		if (!ready)
			return 0;

		raw.planes[0] = dataY;
		raw.planes[1] = dataU;
		raw.planes[2] = dataV;
		raw.stride[0] = strideY;
		raw.stride[1] = strideU;
		raw.stride[2] = strideV;

		int flags = 0;
		if (!(vpx_pts % keyframe_interval))
			flags |= VPX_EFLAG_FORCE_KF;

		return vpx_encode_frame(&vpx_ctx, &raw, vpx_pts++, flags);
	}

	int WebmEncoder::vpx_encode_frame(vpx_codec_ctx_t* codec, vpx_image_t* img, vpx_codec_pts_t pts, int flags)
	{
		if (vpx_codec_encode(codec, img, pts, 1, flags, VPX_DL_GOOD_QUALITY))
			return 0;

		vpx_codec_iter_t iter = NULL;
		const vpx_codec_cx_pkt_t* pkt = vpx_codec_get_cx_data(codec, &iter);
		if (!pkt || pkt->kind != VPX_CODEC_CX_FRAME_PKT)
			return 0;

		uint64_t nanos = rtc::SystemTimeNanos();
		if (!s_nanos)
			s_nanos = nanos;

		l_nanos = nanos - s_nanos;
		if (muxer_segment->AddFrame(static_cast<uint8_t*>(pkt->data.frame.buf), pkt->data.frame.sz, video_track, l_nanos, flags))
		{
			frame_cnt++;
			std::cout << "\r" << std::fixed << std::setprecision(3) << l_nanos / NANOS << " sec";
		}

		return 1;
	}

	int WebmEncoder::create(int width, int height)
	{
		this->width = width;
		this->height = height;
		int preskip = 0;
		uint8_t header[19];
		if (!audio_encoder_create(header, &preskip))
			return 0;

		if (!video_encoder_create(width, height))
			return 0;

		mkvmuxer::Colour colour;
		colour.set_bits_per_channel(8);
		colour.set_chroma_subsampling_horz(1);
		colour.set_chroma_subsampling_vert(1);

		audio_track = muxer_segment->AddAudioTrack(sample_rate, channels, 2);
		mkvmuxer::AudioTrack* audio = static_cast<mkvmuxer::AudioTrack*>(muxer_segment->GetTrackByNumber(audio_track));

		audio->set_codec_id(mkvmuxer::Tracks::kOpusCodecId);
		audio->set_seek_pre_roll(80000000);
		audio->set_codec_delay(preskip * NANOS / sample_rate);
		audio->SetCodecPrivate(header, 19);

		video_track = muxer_segment->AddVideoTrack(width, height, 1);
		mkvmuxer::VideoTrack* video = static_cast<mkvmuxer::VideoTrack*>(muxer_segment->GetTrackByNumber(video_track));
		video->set_codec_id(mkvmuxer::Tracks::kVp8CodecId);
		video->set_frame_rate(frame_rate);
		video->SetColour(colour);
		
		muxer_segment->CuesTrack(audio_track);
		muxer_segment->CuesTrack(video_track);

		int maj, min, bld, rev;
		mkvmuxer::GetVersion(&maj, &min, &bld, &rev);
		std::cout << "Webm Project libwebm v" << maj << "." << min << "." << bld << "." << rev << std::endl;
		std::cout << "INFO:   file      " << output << std::endl;
		std::cout << "INFO:   size      " << width << "x" << height << std::endl;
		std::cout << "INFO:   bitrate   " << bit_rate << std::endl;
		std::cout << "INFO:   fps       " << frame_rate << std::endl;
		std::cout << "Recording..." << std::endl;

		initialized_ = true;
		ready = true;
		return 1;
	}

	int WebmEncoder::webm_encoder_destroy()
	{
		ready = false;
		if (&vpx_ctx)
			vpx_codec_destroy(&vpx_ctx);

		if (&raw)
			vpx_img_free(&raw);

		if (opus_encoder)
			opus_encoder_destroy(opus_encoder);

		if (muxer_segment && mkv_writer)
		{
			double duration = l_nanos / 1000000.0;
			segment_info->set_duration(duration);
			segment_info->Finalize(mkv_writer);

			muxer_segment->Finalize();

			mkv_writer->Close();

			delete muxer_segment;
			delete mkv_writer;
			std::cout << "webm encoder destroy." << std::endl;
		}
		return 1;
	}

	bool WebmEncoder::initialize(const char* output, int bitrate, double frame_rate, int sample_rate, int channels)
	{
		int size = strlen(output);
		memcpy(this->output, output, size);
		this->output[size] = 0x00;
		this->bit_rate = bitrate;
		this->frame_rate = frame_rate;
		this->sample_rate = sample_rate;
		this->channels = channels;

		double f = floor(frame_rate);
		double decimal = frame_rate - f;
		fps_num = 1000;
		fps_den = f * 1000;

		if (decimal > 0.000)
		{
			fps_num += 1;
			fps_den += 1000;
		}

		keyframe_interval = round((double)fps_den / (double)fps_num * 2.0);

		mkv_writer = new mkvmuxer::MkvWriter();
		if (!mkv_writer->Open(output))
			goto fail1;

		muxer_segment = new mkvmuxer::Segment();
		if (!muxer_segment->Init(mkv_writer))
			goto fail2;

		segment_info = muxer_segment->GetSegmentInfo();
		segment_info->set_duration(1.0);

		muxer_segment->AccurateClusterDuration(true);
		muxer_segment->set_mode(mkvmuxer::Segment::kLive);

		return true;

	fail2:
		delete muxer_segment;
	fail1:
		delete mkv_writer;
		return false;
	}

	WebmEncoder::WebmEncoder() :
		initialized_(false),
		ready(false), 
		s_nanos(0), 
		vpx_pts(0), 
		frame_cnt(0),
		l_nanos(0)
	{
	}

	WebmEncoder::~WebmEncoder()
	{
		webm_encoder_destroy();
	}
}