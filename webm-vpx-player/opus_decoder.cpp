#include "media_transform.h"

#include <opus/opus.h>
#include <cassert>

class opus_decoder: public audio_decoder {
	int err = 0;
	int chan;
	int32_t freq;
	OpusDecoder* handle;
	//for debugging without downstream
	float buffer[5760 * 2]{};
	int nb_samples = 0;
	stream_desc out_stream;
public:
	opus_decoder(stream_desc* upstream):
		audio_decoder(decoder_type::AD_OPUS_DIRECT), 
		err([upstream](){assert(upstream && upstream->type == stream_desc::MTYPE_AUDIO 
								&& upstream->detail.audio.codec == stream_desc::audio_info::ACODEC_OPUS);
								return 0; }()),
		chan(upstream->detail.audio.layout == stream_desc::audio_info::CH_LAYOUT_MONO?1:2),
		freq((upstream->detail.audio.Hz== 48000||upstream->detail.audio.Hz == 24000||
			upstream->detail.audio.Hz == 16000||upstream->detail.audio.Hz == 12000||
			upstream->detail.audio.Hz == 8000)? upstream->detail.audio.Hz:48000),
		handle(opus_decoder_create(freq,chan,&err)){
		out_stream.type = stream_desc::MTYPE_AUDIO;
		out_stream.upstream = this;
		out_stream.format_info = upstream->format_info;
		out_stream.detail.audio = upstream->detail.audio;
		out_stream.detail.audio.Hz= freq;
		out_stream.detail.audio.codec = stream_desc::audio_info::ACODEC_PCM;
		out_stream.detail.audio.format.isfloat = 1;
		out_stream.detail.audio.format.isBE = 0;
		out_stream.detail.audio.format.isunsigned = 0;
		out_stream.detail.audio.format.bitdepth_minus_one = 31;
		desc_in = upstream;
		desc_out = &out_stream;
	}
	virtual ~opus_decoder() override final {
		if(handle)
			opus_decoder_destroy(handle);
	}
	virtual int QueueBuffer(_buffer_desc& in_buffer) override final
	{
		nb_samples = opus_decoder_get_nb_samples(handle, (uint8_t*)in_buffer.data, in_buffer.data1);
		int err = opus_decode_float(handle, (uint8_t*)in_buffer.data, in_buffer.data1, buffer, nb_samples, 0);
		if (err) nb_samples = 0;
		return err;
	}
	virtual int FetchBuffer(_buffer_desc& out_buffer) override final
	{
		if (!nb_samples)
			return E_AGAIN;
		out_buffer.data = buffer;
		out_buffer.data1 = nb_samples;
		nb_samples = 0;
		return S_OK;
	}
	virtual int Probe()
	{
		return S_OK;
	}
	virtual int Dropped(int samples) override final
	{
		return opus_decode_float(handle,nullptr,0,buffer,samples,1);
	}
	virtual int Flush() override final
	{
		return opus_decode_float(handle,nullptr,0,buffer,0,0);
	}
	virtual int ReleaseBuffer(_buffer_desc& buffer) override final
	{
		return S_OK;
	}
	virtual int AllocBuffer(_buffer_desc& buffer) override final
	{
		return E_INVALID_OPERATION;
	}
};

audio_decoder* audio_decoder_factory::CreateDefaultOpusDecoder(stream_desc* upstream)
{
	return  new opus_decoder(upstream);
}

