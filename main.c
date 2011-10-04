#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#define MAX_CHANNELS 2
#define BUFFSIZE (44100*6) // 3 seconds * 2 channels
#define MAX_FRAGMENTS 32768 // more than 24h

#define USE_GLOBAL_PEAK

typedef long double sample;

const char throbbler[5] = "/-\\|";

sample rms_values[MAX_CHANNELS][MAX_FRAGMENTS];
#ifndef USE_GLOBAL_PEAK
sample peak_values[MAX_CHANNELS][MAX_FRAGMENTS];
#endif
uint8_t current_channel;

int compare_fragments(const void *s1, const void *s2) {
	sample rms1 = rms_values[current_channel][((size_t*)s1)[0]];
	sample rms2 = rms_values[current_channel][((size_t*)s2)[0]];
	if (rms1>rms2) return -1; else if (rms1<rms2) return 1; else return 0;
	/*if ( (*((sample*)s1)) > (*((sample*)s2)) ) return -1; else
	if ( (*((sample*)s1)) < (*((sample*)s2)) ) return 1; else*/
	return 0;
}

sample to_db(const sample linear) {
	return 20.0l*logl(linear)/M_LN10;
}

int main(int argc, char** argv) {
	FILE* f_in = stdin;

	const uint8_t chan_num = 2;
	int16_t buff[BUFFSIZE];
	uint8_t ch = 0;
	size_t fragment = 0;
#ifdef USE_GLOBAL_PEAK
	sample peak[MAX_CHANNELS];
	for (uint8_t i=0;i<MAX_CHANNELS;i++) peak[i] = 0;
#endif
	uint8_t throbbler_stage = 0;
	fprintf(stderr, "Collecting fragments information...\n");
	while (!feof(f_in)) {
		if (fragment>=MAX_FRAGMENTS) {
			fprintf(stderr, "FATAL ERROR: Input too long! Max length %is.\n", MAX_FRAGMENTS*3);
			return 240;
		}
		size_t values_read = fread(buff, sizeof(int16_t), BUFFSIZE, f_in);
		ch = 0;
		sample sum[MAX_CHANNELS];
		for (size_t i=0;i<chan_num;i++) sum[i] = 0;
		for (size_t i=0;i<values_read;i++) {
			sample value = (sample)buff[i]/32768.0l;
			sum[ch] += value*value;
#ifdef USE_GLOBAL_PEAK
			if (value>peak[ch]) peak[ch] = value;
#else
			if (value>peak_values[ch][fragment]) peak_values[ch][fragment] = value;
#endif
			ch++;
			ch %= chan_num;
		}
		for (ch=0;ch<chan_num;ch++) {
			rms_values[ch][fragment] = sqrtl(sum[ch]/((sample)(values_read/chan_num)));
		}
		fragment++;
		if ((throbbler_stage%4)==0) {
			fprintf(stderr, "\033[1K\033[1G %c  %2i:%02i ", throbbler[throbbler_stage/4], (fragment*3)/60, (fragment*3)%60);
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
	for (uint8_t ch=0;ch<chan_num;ch++) {
		for (size_t i=0;i<fragment;i++) fragments[ch][i] = i;
		current_channel = ch;
		qsort(fragments[ch], fragment, sizeof(size_t), compare_fragments);
		sample rms_sum = 0;
#ifndef USE_GLOBAL_PEAK
		sample peak_sum = 0;
#endif
		int values_to_use = fragment/5;
		/*int values_to_use = 20;
		if (rms_i<values_to_use) {
			values_to_use = rms_i;
			fprintf(stderr, "NOTICE: rms_i = %i < values_to_use\n", rms_i);
		}*/
		for (size_t i=0;i<values_to_use;i++) {
			rms_sum += rms_values[ch][fragments[ch][i]];
#ifndef USE_GLOBAL_PEAK
			peak_sum += peak_values[ch][fragments[ch][i]];
#endif
//			fprintf(stderr, "DEBUG: %i: fragment #%i: Peak %8.2Lf, RMS %8.2Lf\n", i, fragments[ch][i], peak_values[ch][fragments[ch][i]], rms_values[ch][fragments[ch][i]]);
		}
		rms_score[ch] = rms_sum/values_to_use;
#ifndef USE_GLOBAL_PEAK
		peak_score[ch] = peak_sum/values_to_use;
		dr_channel[ch] = to_db(peak_score[ch]/*)-to_db(*//rms_score[ch]);
		printf("Ch. %i:  Peak %8.2Lf    RMS %8.2Lf    DR = %6.2Lf\n", ch, to_db(peak_score[ch]), to_db(rms_score[ch]), dr_channel[ch]);
#else
		dr_channel[ch] = to_db(peak[ch]/rms_score[ch]);
		printf("Ch. %i:  Peak %8.2Lf    RMS %8.2Lf    DR = %6.2Lf\n", ch, to_db(peak[ch]), to_db(rms_score[ch]), dr_channel[ch]);
#endif
		dr_sum += dr_channel[ch];
	}
	printf("Overall dynamic range: DR%i\n", (int)roundl(dr_sum/((sample)chan_num)));
	return 0;

}

