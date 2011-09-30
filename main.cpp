#include <cstdio>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#define MAX_CHANNELS 2
#define BUFFSIZE (44100*3) // 3 seconds
#define MAX_RMS_VALUES 32768 // more than 24h

typedef long double sample;

const char throbbler[5] = "/-\\|";

int compare_samples(const void *s1, const void *s2) {
	if ( (*((sample*)s1)) > (*((sample*)s2)) ) return -1; else
	if ( (*((sample*)s1)) < (*((sample*)s2)) ) return 1; else
	return 0;
}

sample to_db(const sample linear) {
	return 20.0l*logl(linear)/M_LN10;
}

int main(int argc, char** argv) {
	FILE* f_in = stdin;
	sample *rms_values[MAX_CHANNELS];
	for (uint8_t i=0;i<MAX_CHANNELS;i++) rms_values[i] = new sample[MAX_RMS_VALUES];
	const uint8_t chan_num = 2;
	int16_t *buff = new int16_t[BUFFSIZE];
	uint8_t ch = 0;
	size_t rms_i = 0;
	sample peak[MAX_CHANNELS];
	for (uint8_t i=0;i<MAX_CHANNELS;i++) peak[i] = 0;
	uint8_t throbbler_stage;
	fprintf(stderr, "Collecting RMS samples...\n");
	while (!feof(f_in)) {
		size_t values_read = fread(buff, sizeof(int16_t), BUFFSIZE, f_in);
		ch = 0;
		sample sum[MAX_CHANNELS];
		for (size_t i=0;i<chan_num;i++) sum[i] = 0;
		for (size_t i=0;i<values_read;i++) {
			sample value = (sample)buff[i]/32768.0l;
			sum[ch] += value*value;
			if (value>peak[ch]) peak[ch] = value;
			ch++;
			ch %= chan_num;
		}
		for (ch=0;ch<chan_num;ch++) {
			rms_values[ch][rms_i] = sqrtl(sum[ch]/(values_read/chan_num));
		}
		rms_i++;
		if ((throbbler_stage%4)==0) {
			fprintf(stderr, "\033[1K\033[1G %c  %2i:%02i ", throbbler[throbbler_stage/4], (rms_i*3/2)/60, (rms_i*3/2)%60);
		}
		throbbler_stage += 1;
		throbbler_stage %= 16;
	}
	fprintf(stderr, "\nDoing some statistics...\n");
	sample rms_scores[MAX_CHANNELS];
	sample dr_channel[MAX_CHANNELS];
	sample dr_sum = 0;
	for (uint8_t ch=0;ch<chan_num;ch++) {
		qsort(rms_values[ch], rms_i, sizeof(sample), compare_samples);
		sample rms_sum = 0;
		int values_to_use = 20;
		if (rms_i<values_to_use) {
			values_to_use = rms_i;
			fprintf(stderr, "NOTICE: rms_i = %i < values_to_use\n", rms_i);
		}
		for (size_t i=0;i<values_to_use;i++) {
			rms_sum += rms_values[ch][i];
		}
		rms_scores[ch] = rms_sum/values_to_use;
		dr_channel[ch] = to_db(peak[ch])-to_db(rms_scores[ch]);
		printf("Ch. %i:  Peak %8.2Lf    RMS %8.2Lf    DR = %6.2Lf\n", ch, to_db(peak[ch]), to_db(rms_scores[ch]), dr_channel[ch]);
		dr_sum += dr_channel[ch];
	}
	printf("Overall dynamic range: DR%i\n", (int)roundl(dr_sum/((sample)chan_num)));
	return 0;

}

