#include "media_transform.h"

#include <vpx/vpx_codec.h>
#include <vpx/vpx_decoder.h>
#include <mutex>
#include <vpx/vp8dx.h>
#include <cassert>

//implemets the default vp9 decoder with internal
//framebuffers and frame by frame decoding.
//(Also with postprocessing by default, maybe
//add a flag to request)
class libvpx_vp9_ram_decoder: public video_decoder {
	const vpx_codec_iface_t* const iface;
	vpx_codec_ctx ctx;
	//not needed for vp9
	vpx_codec_iter_t iter;
	vpx_image_t* probe_frame = nullptr;
	const vpx_codec_dec_cfg cfg;
	const int mflags;
	const int init_err;
	std::once_flag once_flag;
	bool default_calling_probe = false;
	stream_desc out_stream;
public:
	libvpx_vp9_ram_decoder(stream_desc* upstream, uint32_t threads = 1, int flags = VPX_CODEC_USE_POSTPROC):
		video_decoder(decoder_type::VD_VP9_RAM_VPX_IMG_DECODER),iface(vpx_codec_vp9_dx()), 
		cfg{threads, upstream->detail.video.width, upstream->detail.video.height}, mflags(flags),
		init_err(vpx_codec_dec_init(&ctx, iface, &cfg, mflags)), iter(NULL) {
		assert(upstream->type == stream_desc::MTYPE_VIDEO);
		assert(upstream->detail.video.codec == stream_desc::video_info::VCODEC_VP9);
		out_stream.type =stream_desc::MTYPE_VIDEO;
		out_stream.detail.video = upstream->detail.video;
		out_stream.detail.video.codec = stream_desc::video_info::VCODEC_RAW;
		out_stream.upstream = this;
		out_stream.time_base = upstream->time_base;
		upstream->downstream = this;
		desc_in = upstream;
		desc_out = &out_stream;
	}
	virtual ~libvpx_vp9_ram_decoder() override final
	{
		vpx_codec_destroy(&ctx);
	}
	//for decoders, this means sending a packet into decoder
	virtual int QueueBuffer(_buffer_desc& buffer) override final
	{
		//make checks here
		//assume 64 bit
		assert(sizeof(void*)==sizeof(buffer.start_timestamp));
		return vpx_codec_decode(&ctx, (uint8_t*)buffer.detail.pkt.buffer,buffer.detail.pkt.size, (void*)buffer.start_timestamp, 0);
	}
	//for decoders, this means getting a frame from decoder
	virtual int FetchBuffer(_buffer_desc& buffer) override final
	{
		default_calling_probe = true;
		std::call_once(once_flag, &libvpx_vp9_ram_decoder::Probe, this);
		//buffer.type checks here
		[[unlikely]] if (probe_frame) {
			translate_from_vpx_img(buffer,probe_frame);
			probe_frame = nullptr;
			return S_OK;
		}
		vpx_image_t* img = vpx_codec_get_frame(&ctx, &iter);
		if(!img)
			return E_AGAIN;
		else {
			translate_from_vpx_img(buffer, img);
			buffer.stream = desc_out;
			//assume 64 bit
			buffer.start_timestamp = (uint64_t)img->user_priv;
			return S_OK;
		}
	}
	//This is for cases where the frame is owned or refed
	//by the user and needs to be freed
	virtual int ReleaseBuffer(_buffer_desc& buffer) override final
	{
		return S_OK;
	}
	//This is for cases where the frame is owned or refed
	//by the user and needs to be allocated manually
	virtual int AllocBuffer(_buffer_desc& buffer) override final
	{
		return E_INVALID_OPERATION;
	}
	//This means that the last packet is recieved.
	virtual int Flush() override final
	{
		return vpx_codec_decode(&ctx, nullptr, 0, 0, 0);
	}
	//This means that a packet is dropped and requests
	//an error concealment frame if supported
	//(mostly used in audio codecs)
	virtual int Dropped(int samples) override final
	{
		return E_UNIMPLEMENTED;
	}
	//Probe the format
	virtual int Probe() override final
	{
		assert(desc_out);
		if (probe_frame)
			return E_INVALID_OPERATION;
		vpx_image_t* img = vpx_codec_get_frame(&ctx, &iter);
		if (!img)
			return S_OK;
		auto do_probe = [this, img] () {
			probe_frame = img;
			color_space spc = translate_from_vpx_cs(img->cs);
			desc_in->detail.video.space;
			desc_out->detail.video.space;
			video_sample_format fmt;
			translate_from_vpx_fmt(fmt,img->fmt);
			desc_in->detail.video.fmt = fmt;
			desc_out->detail.video.fmt = fmt;
			color_range range = translate_from_vpx_cr(img->range);
			desc_in->detail.video.range = range;
			desc_out->detail.video.range = range;
		};
		if(!default_calling_probe)
			std::call_once(once_flag, do_probe);
		else
			do_probe();
		return S_OK;
	}
private:
	static vpx_color_space translate_to_vpx_cs(color_space my_space) noexcept
	{
		switch (my_space) {
			case CS_UNKNOWN:
				return VPX_CS_UNKNOWN;
			case CS_BT_601:
				return VPX_CS_BT_601;
			case CS_BT_709:
				return VPX_CS_BT_709;
			case CS_BT_2020:
				return VPX_CS_BT_2020;
			case CS_SRGB:
				return VPX_CS_SRGB;
			case CS_SMPTE_170:
				return VPX_CS_SMPTE_170;
			case CS_SMPTE_240:
				return VPX_CS_SMPTE_240;
			default:
				return VPX_CS_UNKNOWN;
		}
	}
	static color_space translate_from_vpx_cs(vpx_color_space from_space) noexcept {
		switch (from_space) {
			case VPX_CS_UNKNOWN:
				return CS_UNKNOWN;
			case VPX_CS_BT_601:
				return CS_BT_601;
			case VPX_CS_BT_709:
				return CS_BT_709;
			case VPX_CS_BT_2020:
				return CS_BT_2020;
			case VPX_CS_SRGB:
				return CS_SRGB;
			case VPX_CS_SMPTE_170:
				return CS_SMPTE_170;
			case VPX_CS_SMPTE_240:
				return CS_SMPTE_240;
			default:
				return CS_UNKNOWN;
		}
	}
	static vpx_img_fmt translate_to_vpx_fmt(const video_sample_format& fmt) noexcept
	{
		if (fmt.planar) {
			if (fmt.subsample_horiz) {
				if (fmt.subsample_vert) {
					if (fmt.bitdepth > 8)
						return VPX_IMG_FMT_I42016;
					else
						return VPX_IMG_FMT_I420;
				}
				else {
					if (fmt.bitdepth > 8)
						return VPX_IMG_FMT_I42216	;
					else 
						return VPX_IMG_FMT_I422;
				}
			}
			else {
				if (fmt.subsample_vert) {
					if (fmt.bitdepth > 8)
						return VPX_IMG_FMT_I44016;
					else
						return VPX_IMG_FMT_I440;
				}
				else {
					if (fmt.bitdepth > 8)
						return VPX_IMG_FMT_I44416;
					else
						return VPX_IMG_FMT_I444;
				}
			}
		}
		else {
			return VPX_IMG_FMT_NONE;
		}
	}
	static void translate_from_vpx_fmt(video_sample_format& fmt, vpx_img_fmt in_fmt) noexcept 
	{
		memset(&fmt,0,sizeof(fmt));
		fmt.planar = (in_fmt & VPX_IMG_FMT_PLANAR) != 0;
		fmt.bitdepth = in_fmt & VPX_IMG_FMT_HIGHBITDEPTH? 16: 8;
		fmt.invert_uv = (in_fmt & VPX_IMG_FMT_UV_FLIP) != 0;
		switch (in_fmt & (0xffffffffffffffff ^ VPX_IMG_FMT_HIGHBITDEPTH)) {
			case VPX_IMG_FMT_YV12:
			case VPX_IMG_FMT_I420:
				fmt.subsample_horiz = 1;
				fmt.subsample_vert = 1;
				break;
			case VPX_IMG_FMT_I422:
				fmt.subsample_horiz = 1;
				fmt.subsample_vert = 0;
				break;
			case VPX_IMG_FMT_I440:
				fmt.subsample_horiz = 0;
				fmt.subsample_vert = 1;
				break;
			case VPX_IMG_FMT_I444:
				fmt.subsample_horiz = 0;
				fmt.subsample_vert = 0;
				break;
			default:
				0;
		}
	}
	static vpx_color_range translate_to_vpx_cr(color_range range) noexcept
	{
		switch (range) {
			case CR_STUDIO_RANGE:
				return VPX_CR_STUDIO_RANGE;
			case CR_FULL_RANGE:
				return VPX_CR_FULL_RANGE;
			default:
				return VPX_CR_STUDIO_RANGE;
		}
	}
	static color_range translate_from_vpx_cr(vpx_color_range range) noexcept
	{
		switch (range) {
			case VPX_CR_STUDIO_RANGE:
				return CR_STUDIO_RANGE;
			case VPX_CR_FULL_RANGE:
				return CR_FULL_RANGE;
			default:
				return CR_STUDIO_RANGE;
		}
	}
	void translate_from_vpx_img(_buffer_desc& desc, vpx_image_t* img)
	{
		desc.detail.image.planar = 1;
		desc.detail.image.width = img->w;
		desc.detail.image.height = img->h;
		desc.detail.image.space = translate_from_vpx_cs(img->cs);
		desc.detail.image.range = translate_from_vpx_cr(img->range);
		for (int i = 0; i < 4; ++i) {
			desc.detail.image.line_size[i] = img->stride[i];
			desc.detail.image.planes[i] = img->planes[i];
		}
	}
};


video_decoder* video_decoder_factory::CreateDefaultVP9Decoder(stream_desc* upstream, uint32_t threads, bool use_post_proc)
{
	assert(upstream);
	int flags = 0;
	if(use_post_proc)
		flags |= VPX_CODEC_USE_POSTPROC;
	if(threads>1)
		flags |= VPX_CODEC_USE_FRAME_THREADING;
	else 
		threads = 1;
	return new libvpx_vp9_ram_decoder(upstream,threads,flags);
}
