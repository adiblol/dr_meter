#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <assert.h>

#include "libavformat/avformat.h"

#define MAX_CHANNELS 2
#define SAMPLE_RATE 44100
#define BUFFSIZE (SAMPLE_RATE * MAX_CHANNELS * 3) // 3 seconds
#define MAX_FRAGMENTS 32768 // more than 24h

#define USE_GLOBAL_PEAK

typedef double sample;

const char throbbler[5] = "/-\\|";

sample rms_values[MAX_CHANNELS][MAX_FRAGMENTS];
#ifndef USE_GLOBAL_PEAK
sample peak_values[MAX_CHANNELS][MAX_FRAGMENTS];
#endif
uint8_t current_channel;

int compare_fragments(const void *s1, const void *s2) {
	sample rms1 = rms_values[current_channel][((size_t*)s1)[0]];
	sample rms2 = rms_values[current_channel][((size_t*)s2)[0]];
	if (rms1 > rms2) return -1;
	else if (rms1 < rms2) return 1;
	return 0;
}

sample to_db(const sample linear) {
	return 20.0 * log10(linear);
}

void print_av_error(const char *function_name, int error) {
	char errorbuf[128];
	char *error_ptr = errorbuf;
	if (av_strerror(error, errorbuf, sizeof(errorbuf)) < 0) {
		error_ptr = strerror(AVUNERROR(error));
	}
	fprintf(stderr, "dr_meter: %s: %s\n", function_name, error_ptr);
	exit(EXIT_FAILURE);
}

struct stream_context {
	AVFormatContext *format_ctx;
	int stream_index; // the stream we are decoding
	AVPacket real_pkt;
	AVPacket pkt;
	enum {
		STATE_UNINITIALIZED,
		STATE_INITIALIZED,
		STATE_OPEN,
		STATE_VALID_PACKET,
		STATE_NEED_FLUSH,
		STATE_CLOSED,
	} state;
	void *buf;
	size_t buf_size; // the number of bytes present
	size_t buf_alloc_size; // the number of bytes allocated
	void *pos;
	size_t remaining;
};

void sc_init(struct stream_context *self) {
	self->format_ctx = NULL;
	self->stream_index = 0;
	self->buf = NULL;
	self->buf_size = 0;
	self->buf_alloc_size = 0;
	self->pos = NULL;
	self->remaining = 0;
	self->state = STATE_INITIALIZED;
}

int sc_open(struct stream_context *self, const char *filename) {
	int err;

	sc_init(self);
	err = avformat_open_input(&self->format_ctx, filename, NULL, NULL);
	if (err < 0) { return err; }
	err = av_find_stream_info(self->format_ctx);
	if (err < 0) { return err; }

	self->state = STATE_OPEN;

	return 0;
}

bool sc_eof(struct stream_context *self) {
	return self->state == STATE_CLOSED;
}

/* return the AVCodecContext for the active stream */
AVCodecContext *sc_get_codec(struct stream_context *self) {
	return self->format_ctx->streams[self->stream_index]->codec;
}

int sc_start_stream(struct stream_context *self, int stream_index) {
	self->stream_index = stream_index;
	AVCodecContext *ctx = sc_get_codec(self);
	AVCodec *codec = avcodec_find_decoder(ctx->codec_id);
	if (codec == NULL) {
		return AVERROR_DECODER_NOT_FOUND;
	}
	/* XXX check codec */
	return avcodec_open(ctx, codec);
}

int sc_refill(struct stream_context *self) {
	int err;

	if (self->state == STATE_CLOSED) {
		return AVERROR_EOF;
	}

	while (self->state == STATE_OPEN) {
		err = av_read_frame(self->format_ctx, &self->real_pkt);
		if (err == AVERROR_EOF || err == AVERROR_IO) {
			av_init_packet(&self->pkt);
			self->pkt.data = NULL;
			self->pkt.size = 0;
			self->state = STATE_NEED_FLUSH;
		} else if (err < 0) {
			return err;
		} else if (self->real_pkt.stream_index == self->stream_index) {
			self->pkt = self->real_pkt;
			self->state = STATE_VALID_PACKET;
		} else {
			// we don't care about this frame; try another
			av_free_packet(&self->real_pkt);
		}
	}

	AVCodecContext *codec_ctx = sc_get_codec(self);

	// Allocate a buffer.
	size_t buf_size = self->pkt.size * av_get_bytes_per_sample(codec_ctx->sample_fmt);
	buf_size = FFMAX3(buf_size, FF_MIN_BUFFER_SIZE, AVCODEC_MAX_AUDIO_FRAME_SIZE);

	if (self->buf_alloc_size < buf_size) {
		self->buf = realloc(self->buf, buf_size);
		self->buf_size = buf_size;
		self->buf_alloc_size = buf_size;
		self->remaining = 0;
	}
	if (self->buf == NULL) {
		return AVERROR(ENOMEM);
	}

	// Decode the audio.

	// The third parameter gives the size of the output buffer, and is set
	// to the number of bytes used of the output buffer.
	// The return value is the number of bytes read from the packet.
	// The codec is not required to read the entire packet, so we may need
	// to keep it around for a while.
	int buf_size_out = buf_size;
	err = avcodec_decode_audio3(codec_ctx, self->buf, &buf_size_out, &self->pkt);
	if (err < 0) { return err; }
	size_t bytes_used = (size_t)err;

	self->pos = self->buf;
	self->buf_size = buf_size_out;
	self->remaining = buf_size_out;

	if (self->state == STATE_VALID_PACKET) {
		if (0 < bytes_used && (signed)bytes_used < self->pkt.size) {
			self->pkt.data += bytes_used;
			self->pkt.size -= bytes_used;
		} else  {
			self->state = STATE_OPEN;
			av_free_packet(&self->real_pkt);
		}
	} else if (self->state == STATE_NEED_FLUSH) {
		avcodec_close(codec_ctx);
		self->state = STATE_CLOSED;
	}

	return 0;
}

ssize_t sc_read(struct stream_context *self, void *buf, size_t buf_size) {
	size_t orig_buf_size = buf_size;
	size_t read_size = 0;
	int err;
	while (buf_size) {
		if (self->remaining <= 0) {
			err = sc_refill(self);
			if (err == AVERROR_EOF) {
				break;
			} else if (err < 0) {
				return err;
			}
		}

		read_size = FFMIN(self->remaining, buf_size);
		//printf("%d, %d, %d\n", read_size, buf_size, self->pkt.size);
		memmove(buf, self->pos, read_size);
		self->pos += read_size;
		self->remaining -= read_size;
		buf += read_size;
		buf_size -= read_size;
	}

	// return the number of bytes copied
	return orig_buf_size - buf_size;
}

int main(int argc, char** argv) {
	av_register_all();

	if (argc < 2) {
		fprintf(stderr, "Usage: dr_meter file\n");
		exit(1);
	}

	struct stream_context sc;
	int err;

	err = sc_open(&sc, argv[1]);
	if (err < 0) { print_av_error("sc_open", err); }

	err = sc_start_stream(&sc, 0);
	if (err < 0) { print_av_error("sc_start_stream", err); }

	{
	AVCodecContext *codec_ctx = sc_get_codec(&sc);
	char codecinfobuf[256];
	avcodec_string(codecinfobuf, sizeof(codecinfobuf), codec_ctx, 0);
	fprintf(stderr, "input: %.256s\n", codecinfobuf);
	assert(codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO);
	assert(codec_ctx->sample_rate == SAMPLE_RATE);
	assert(codec_ctx->channels == MAX_CHANNELS);
	assert(codec_ctx->sample_fmt == SAMPLE_FMT_S16);
	};

	const uint8_t chan_num = 2;
	int16_t buff[BUFFSIZE];
	uint8_t ch = 0;
	size_t fragment = 0;
#ifdef USE_GLOBAL_PEAK
	sample peak[MAX_CHANNELS];
	for (uint8_t i = 0; i < MAX_CHANNELS; i++) peak[i] = 0;
#endif
	uint8_t throbbler_stage = 0;
	fprintf(stderr, "Collecting fragments information...\n");
	while (!sc_eof(&sc)) {
		if (fragment >= MAX_FRAGMENTS) {
			fprintf(stderr, "FATAL ERROR: Input too long! Max length %is.\n", MAX_FRAGMENTS*3);
			return 240;
		}
		ssize_t err = sc_read(&sc, buff, sizeof(buff));
		if (err < 0) {
			print_av_error("sc_read", err);
		}
		size_t values_read = (size_t)err / sizeof(int16_t);
		ch = 0;
		sample sum[MAX_CHANNELS];
		for (size_t i = 0; i < chan_num; i++) sum[i] = 0;
		for (size_t i = 0; i < values_read; i++) {
			sample value = (sample)buff[i] / 32768.0;
			sum[ch] += value * value;
			value = fabs(value);
#ifdef USE_GLOBAL_PEAK
			if (peak[ch] < value) peak[ch] = value;
#else
			if (peak_values[ch][fragment] < value) peak_values[ch][fragment] = value;
#endif
			ch++;
			ch %= chan_num;
		}
		for (ch = 0; ch < chan_num; ch++) {
			rms_values[ch][fragment] = sqrt(2.0 * sum[ch] / ((sample)(values_read / chan_num)));
		}
		fragment++;
		if ((throbbler_stage % 4) == 0) {
			fprintf(stderr, "\033[1K\033[1G %c  %2i:%02i ",
			                throbbler[throbbler_stage / 4],
			                (fragment * 3) / 60,
			                (fragment * 3) % 60);
		}
		throbbler_stage += 1;
		throbbler_stage %= 16;
	}

	fprintf(stderr, "\nDoing some statistics...\n");
	sample rms_score[MAX_CHANNELS];
#ifndef USE_GLOBAL_PEAK
	sample peak_score[MAX_CHANNELS];
#endif
	sample dr_channel[MAX_CHANNELS];
	size_t fragments[MAX_CHANNELS][fragment];
	sample dr_sum = 0;
	for (uint8_t ch = 0; ch < chan_num; ch++) {
		for (size_t i = 0; i < fragment; i++) fragments[ch][i] = i;
		current_channel = ch;
		qsort(fragments[ch], fragment, sizeof(size_t), compare_fragments);
		sample rms_sum = 0;
#ifndef USE_GLOBAL_PEAK
		sample peak_sum = 0;
#endif
		size_t values_to_use = fragment / 5;
		for (size_t i = 0; i < values_to_use; i++) {
			rms_sum += rms_values[ch][fragments[ch][i]];
#ifndef USE_GLOBAL_PEAK
			peak_sum += peak_values[ch][fragments[ch][i]];
#endif
//			fprintf(stderr, "DEBUG: %i: fragment #%i: Peak %8.2f, RMS %8.2f\n", i, fragments[ch][i], peak_values[ch][fragments[ch][i]], rms_values[ch][fragments[ch][i]]);
		}
		rms_score[ch] = rms_sum / values_to_use;
#ifndef USE_GLOBAL_PEAK
		peak_score[ch] = peak_sum / values_to_use;
		dr_channel[ch] = to_db(peak_score[ch] / rms_score[ch]);
		printf("Ch. %i:  Peak %8.2f    RMS %8.2f    DR = %6.2f\n",
		       ch,
		       to_db(peak_score[ch]),
		       to_db(rms_score[ch]),
		       dr_channel[ch]);
#else
		dr_channel[ch] = to_db(peak[ch] / rms_score[ch]);
		printf("Ch. %i:  Peak %8.2f    RMS %8.2f    DR = %6.2f\n",
		       ch,
		       to_db(peak[ch]),
		       to_db(rms_score[ch]),
		       dr_channel[ch]);
#endif
		dr_sum += dr_channel[ch];
	}
	printf("Overall dynamic range: DR%i\n",
	       (int)round(dr_sum / ((sample)chan_num)));

	return 0;
}

