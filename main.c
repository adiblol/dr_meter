#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <assert.h>

#include "libavformat/avformat.h"
#include "libavutil/error.h"

#define MAX_CHANNELS 32
#define MAX_FRAGMENTS 32768 // more than 24h

#define FACTOR8 ((sample)1.0 / (sample)(1 << 7))
#define FACTOR16 ((sample)1.0 / (sample)(1 << 15))
#define FACTOR32 ((sample)1.0 / (sample)(1UL << 31))

#define min(a, b) ((a) < (b) ? (a) : (b))

typedef double sample;

const char throbbler[5] = "/-\\|";

sample *rms_values[MAX_CHANNELS];
sample *peak_values[MAX_CHANNELS];

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

/* return the AVCodecContext for the active stream */
AVCodecContext *sc_get_codec(struct stream_context *self) {
	return self->format_ctx->streams[self->stream_index]->codec;
}

void sc_close(struct stream_context *self) {
	if (STATE_OPEN <= self->state && self->state != STATE_CLOSED) {
		avcodec_close(sc_get_codec(self));
		av_close_input_stream(self->format_ctx);
		self->state = STATE_CLOSED;
	}
}

bool sc_eof(struct stream_context *self) {
	return self->state == STATE_CLOSED;
}

int sc_start_stream(struct stream_context *self, int stream_index) {
	self->stream_index = stream_index;
	AVCodecContext *ctx = sc_get_codec(self);
	AVCodec *codec = avcodec_find_decoder(ctx->codec_id);
	if (codec == NULL) {
		return AVERROR_DECODER_NOT_FOUND;
	}

	// Allocate a buffer.
	size_t buf_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
	if (self->buf_alloc_size < buf_size) {
		av_free(self->buf);
		// NOTE: the buffer MUST be allocated with av_malloc;
		// ffmpeg has some very strict alignment requirements.
		self->buf = av_malloc(buf_size);
		self->buf_size = 0;
		self->buf_alloc_size = buf_size;
		self->pos = self->buf;
		self->remaining = 0;
	}
	if (self->buf == NULL) {
		return AVERROR(ENOMEM);
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
		if (err == AVERROR_EOF || url_feof(self->format_ctx->pb)) {
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

	// Decode the audio.

	// The third parameter gives the size of the output buffer, and is set
	// to the number of bytes used of the output buffer.
	// The return value is the number of bytes read from the packet.
	// The codec is not required to read the entire packet, so we may need
	// to keep it around for a while.
	int buf_size = self->buf_alloc_size;
	err = avcodec_decode_audio3(codec_ctx, self->buf, &buf_size, &self->pkt);
	if (err < 0) { return err; }
	int bytes_used = err;

	self->buf_size = buf_size;

	self->pos = self->buf;
	self->remaining = buf_size;

	if (self->state == STATE_VALID_PACKET) {
		if (0 < bytes_used && bytes_used < self->pkt.size) {
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
		if (read_size) {
			memmove(buf, self->pos, read_size);
			self->pos += read_size;
			self->remaining -= read_size;
			buf += read_size;
			buf_size -= read_size;
		}
	}

	// return the number of bytes copied
	return orig_buf_size - buf_size;
}

/******************************************************************************/

int compare_samples(const void *s1, const void *s2) {
	sample rms1 = *(sample *)s1;
	sample rms2 = *(sample *)s2;
	if (rms1 > rms2) return -1;
	else if (rms1 < rms2) return 1;
	return 0;
}

sample get_sample(void *buf, size_t i, enum AVSampleFormat sample_fmt) {
	switch(sample_fmt) {
	case AV_SAMPLE_FMT_U8: return (sample)(((uint8_t *)buf)[i] - 0x80) * FACTOR8;
	case AV_SAMPLE_FMT_S16: return (sample)(((int16_t *)buf)[i]) * FACTOR16;
	case AV_SAMPLE_FMT_S32: return (sample)(((int32_t *)buf)[i]) * FACTOR32;
	case AV_SAMPLE_FMT_FLT: return (sample)(((float *)buf)[i]);
	case AV_SAMPLE_FMT_DBL: return (sample)(((double *)buf)[i]);
	default: return 0.0;
	}
}

sample to_db(const sample linear) {
	return 20.0 * log10(linear);
}

int print_av_error(const char *function_name, int error) {
	char errorbuf[128];
	char *error_ptr = errorbuf;
	if (av_strerror(error, errorbuf, sizeof(errorbuf)) < 0) {
		error_ptr = strerror(AVUNERROR(error));
	}
	fprintf(stderr, "dr_meter: %s: %s\n", function_name, error_ptr);
	return error;
}

int do_calculate_dr(const char *filename) {
	struct stream_context sc;
	int16_t *buff = NULL;
	int chan_num = 0;
	int err;

	err = sc_open(&sc, filename);
	if (err < 0) { return print_av_error("sc_open", err); }

	int stream_index = err = av_find_best_stream(
		sc.format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (err < 0) { print_av_error("av_find_best_stream", err); goto cleanup; }

	err = sc_start_stream(&sc, stream_index);
	if (err < 0) { print_av_error("sc_start_stream", err); goto cleanup; }

	// Print out the stream info
	AVCodecContext *codec_ctx = sc_get_codec(&sc);
	char codecinfobuf[256];
	avcodec_string(codecinfobuf, sizeof(codecinfobuf), codec_ctx, 0);
	fprintf(stderr, "%.256s\n", codecinfobuf);

	// Figure out the fragment size
	chan_num = codec_ctx->channels;
	const int sample_rate = codec_ctx->sample_rate;
	const int sample_fmt = codec_ctx->sample_fmt;
	const int sample_size = av_get_bytes_per_sample(sample_fmt);

	if (chan_num > MAX_CHANNELS) {
		fprintf(stderr, "FATAL ERROR: Too many channels! Max channels %is.\n", MAX_CHANNELS);
		err = 240; // ???
		goto cleanup;
	}

	if (sample_fmt != AV_SAMPLE_FMT_U8 &&
	    sample_fmt != AV_SAMPLE_FMT_S16 &&
	    sample_fmt != AV_SAMPLE_FMT_S32 &&
	    sample_fmt != AV_SAMPLE_FMT_FLT &&
	    sample_fmt != AV_SAMPLE_FMT_DBL) {
		fprintf(stderr, "FATAL ERROR: Unsupported sample format: %s\n", av_get_sample_fmt_name(sample_fmt));
		err = 240;
		goto cleanup;
	}

	// 3-second window
	const size_t buff_size = chan_num * sample_rate * sample_size * 3;
	assert(buff_size > 0);

	// Allocate the buffer
	buff = malloc(buff_size);
	if (buff == NULL) { err = AVERROR(ENOMEM); goto cleanup; }

	// Allocate RMS and peak storage
	for (int ch = 0; ch < chan_num; ch++) {
		rms_values[ch] = malloc(MAX_FRAGMENTS * sizeof(*rms_values[ch]));
		peak_values[ch] = malloc(MAX_FRAGMENTS * sizeof(*rms_values[ch]));
		if (rms_values[ch] == NULL || peak_values[ch] == NULL) {
			err = AVERROR(ENOMEM);
			goto cleanup;
		}
	}

	size_t fragment = 0;
	int throbbler_stage = 0;
	fprintf(stderr, "Collecting fragments information...\n");
	while (!sc_eof(&sc)) {
		if (fragment >= MAX_FRAGMENTS) {
			fprintf(stderr, "FATAL ERROR: Input too long! Max length %is.\n", MAX_FRAGMENTS*3);
			err = 240; // ???
			goto cleanup;
		}
		ssize_t bytes_read = sc_read(&sc, buff, buff_size);
		if (bytes_read < 0) {
			err = bytes_read;
			print_av_error("sc_read", err);
			goto cleanup;
		}
		size_t values_read = (size_t)bytes_read / sample_size;
		if (!values_read) { continue; }
		assert(values_read % chan_num == 0);

		sample sum[MAX_CHANNELS];
		for (int ch = 0; ch < chan_num; ch++) {
			sum[ch] = 0;
			peak_values[ch][fragment] = 0;
		}

		for (size_t i = 0; i < values_read; /* look down */) {
			for (int ch = 0; ch < chan_num; ch++, i++) {
				sample value = get_sample(buff, i, sample_fmt);
				sum[ch] += value * value;
				value = fabs(value);
				if (peak_values[ch][fragment] < value) {
					peak_values[ch][fragment] = value;
				}
			}
		}
		for (int ch = 0; ch < chan_num; ch++) {
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
	sample rms[MAX_CHANNELS];
	sample peak_score[MAX_CHANNELS];
	sample dr_channel[MAX_CHANNELS];
	sample dr_sum = 0;
	for (int ch = 0; ch < chan_num; ch++) {
		qsort(rms_values[ch], fragment, sizeof(**rms_values), compare_samples);
		sample rms_sum = 0;
		size_t values_to_use = fragment / 5;
		for (size_t i = 0; i < values_to_use; i++) {
			sample value = rms_values[ch][i];
			rms_sum += value * value;
		}
		rms_score[ch] = sqrt(rms_sum / values_to_use);

		rms_sum = 0;
		for (size_t i = 0; i < fragment; i++) {
			sample value = rms_values[ch][i];
			rms_sum += value * value;
		}
		rms[ch] = sqrt(rms_sum / fragment);

		qsort(peak_values[ch], fragment, sizeof(*peak_values[ch]), compare_samples);
		peak_score[ch] = peak_values[ch][min(1, fragment)];

		dr_channel[ch] = to_db(peak_score[ch] / rms_score[ch]);
		printf("Ch. %2i:  Peak %8.2f (%8.2f)   RMS %8.2f (%8.2f)   DR = %6.2f\n",
		       ch,
		       to_db(peak_values[ch][0]),
		       to_db(peak_score[ch]),
		       to_db(rms[ch]),
		       to_db(rms_score[ch]),
		       dr_channel[ch]);
		dr_sum += dr_channel[ch];
	}
	printf("Overall dynamic range: DR%i\n",
	       (int)round(dr_sum / ((sample)chan_num)));

cleanup:
	sc_close(&sc);

	for (int ch = 0; ch < chan_num; ch++) {
		free(rms_values[ch]);
		free(peak_values[ch]);
		rms_values[ch] = NULL;
		peak_values[ch] = NULL;
	}

	free(buff);

	if (err < 0) {
		return err;
	}

	return 0;
}

int main(int argc, char** argv) {
	av_register_all();

	bool err_occurred = false;
	int err;

	if (argc <= 1) {
		fprintf(stderr, "Reading from standard input...\n");
		err = do_calculate_dr("pipe:");
		if (err) {
			err_occurred = true;
		}
	} else {
		for (int i = 1; i < argc; i++) {
			printf("%s\n", argv[i]);
			err = do_calculate_dr(argv[i]);
			if (err) {
				err_occurred = true;
			}
		}
	}

	exit(err_occurred ? EXIT_FAILURE : EXIT_SUCCESS);
}
