//
//    This file is part of Dire Wolf, an amateur radio packet TNC.
//
//    Copyright (C) 2011, 2014, 2015, 2016, 2019  John Langner, WB2OSZ
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//


/*------------------------------------------------------------------
 *
 * Module:      gen_tone.c
 *
 * Purpose:     Convert bits to AFSK for writing to .WAV sound file 
 *		or a sound device.
 *
 *
 *---------------------------------------------------------------*/

#include "direwolf.h"


#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>


#include "audio.h"
#include "gen_tone.h"
#include "textcolor.h"
#include "hdlc_send.h"

#include "fsk_demod_state.h"	/* for MAX_FILTER_SIZE which might be overly generous for here. */
				/* but safe if we use same size as for receive. */
#include "dsp.h"


// Properties of the digitized sound stream & modem.

static struct audio_s *save_audio_config_p = NULL;

/*
 * 8 bit samples are unsigned bytes in range of 0 .. 255.
 *
 * 16 bit samples are signed short in range of -32768 .. +32767.
 */


/* Constants after initialization. */

#define TICKS_PER_CYCLE ( 256.0 * 256.0 * 256.0 * 256.0 )

static int ticks_per_sample[MAX_CHANS];	/* Same for both channels of same soundcard */
					/* because they have same sample rate */
					/* but less confusing to have for each channel. */

static int ticks_per_bit[MAX_CHANS];
static int f1_change_per_sample[MAX_CHANS];
static int f2_change_per_sample[MAX_CHANS];


static short sine_table[256];


/* Accumulators. */

static unsigned int tone_phase[MAX_CHANS]; // Phase accumulator for tone generation.
					   // Upper bits are used as index into sine table.

#define PHASE_SHIFT_180 ( 128u << 24 )
#define PHASE_SHIFT_90  (  64u << 24 )
#define PHASE_SHIFT_45  (  32u << 24 )


static int bit_len_acc[MAX_CHANS];	// To accumulate fractional samples per bit.

static int lfsr[MAX_CHANS];		// Shift register for scrambler.

static int bit_count[MAX_CHANS];	// Counter incremented for each bit transmitted
					// on the channel.   This is only used for QPSK.
					// The LSB determines if we save the bit until
					// next time, or send this one with the previously saved.
					// The LSB+1 position determines if we add an
					// extra 180 degrees to the phase to compensate
					// for having 1.5 carrier cycles per symbol time.

					// For 8PSK, it has a different meaning.  It is the
					// number of bits in 'save_bit' so we can accumulate
					// three for each symbol.
static int save_bit[MAX_CHANS];


static int prev_dat[MAX_CHANS];		// Previous data bit.  Used for G3RUH style.




/*------------------------------------------------------------------
 *
 * Name:        gen_tone_init
 *
 * Purpose:     Initialize for AFSK tone generation which might
 *		be used for RTTY or amateur packet radio.
 *
 * Inputs:      audio_config_p		- Pointer to modem parameter structure, modem_s.
 *
 *				The fields we care about are:
 *
 *					samples_per_sec
 *					baud
 *					mark_freq
 *					space_freq
 *					samples_per_sec
 *
 *		amp		- Signal amplitude on scale of 0 .. 100.
 *
 *				  100% uses the full 16 bit sample range of +-32k.
 *
 *		gen_packets	- True if being called from "gen_packets" utility
 *				  rather than the "direwolf" application.
 *
 * Returns:     0 for success.
 *              -1 for failure.
 *
 * Description:	 Calculate various constants for use by the direct digital synthesis
 * 		audio tone generation.
 *
 *----------------------------------------------------------------*/

static int amp16bit;	/* for 9600 baud */


int gen_tone_init (struct audio_s *audio_config_p, int amp, int gen_packets)
{
	int j;
	int chan = 0;

#if DEBUG
	text_color_set(DW_COLOR_DEBUG);
	dw_printf ("gen_tone_init ( audio_config_p=%p, amp=%d, gen_packets=%d )\n",
			audio_config_p, amp, gen_packets);
#endif
	
/* 
 * Save away modem parameters for later use. 
 */

	save_audio_config_p = audio_config_p;
	
	amp16bit = (int)((32767 * amp) / 100);

	for (chan = 0; chan < MAX_CHANS; chan++) {

	  if (audio_config_p->achan[chan].medium == MEDIUM_RADIO) {

	    int a = ACHAN2ADEV(chan);

#if DEBUG
	text_color_set(DW_COLOR_DEBUG);
	dw_printf ("gen_tone_init: chan=%d, modem_type=%d, bps=%d, samples_per_sec=%d\n",
		chan,
		save_audio_config_p->achan[chan].modem_type,
		audio_config_p->achan[chan].baud,
		audio_config_p->adev[a].samples_per_sec);
#endif

	    tone_phase[chan] = 0;
	    bit_len_acc[chan] = 0;
	    lfsr[chan] = 0;

	    ticks_per_sample[chan] = (int) ((TICKS_PER_CYCLE / (double)audio_config_p->adev[a].samples_per_sec ) + 0.5);

	    // The terminology is all wrong here.  Didn't matter with 1200 and 9600.
	    // The config speed should be bits per second rather than baud.
	    // ticks_per_bit should be ticks_per_symbol.

	    switch (save_audio_config_p->achan[chan].modem_type) {

	      case MODEM_QPSK:

	        audio_config_p->achan[chan].mark_freq = 1800;
	        audio_config_p->achan[chan].space_freq = audio_config_p->achan[chan].mark_freq;	// Not Used.

	        // symbol time is 1 / (half of bps)
	        ticks_per_bit[chan] = (int) ((TICKS_PER_CYCLE / ((double)audio_config_p->achan[chan].baud * 0.5)) + 0.5);
	        f1_change_per_sample[chan] = (int) (((double)audio_config_p->achan[chan].mark_freq * TICKS_PER_CYCLE / (double)audio_config_p->adev[a].samples_per_sec ) + 0.5);
	        f2_change_per_sample[chan] = f1_change_per_sample[chan];	// Not used.

	        tone_phase[chan] = PHASE_SHIFT_45;	// Just to mimic first attempt.
	        break;

	      case MODEM_8PSK:

	        audio_config_p->achan[chan].mark_freq = 1800;
	        audio_config_p->achan[chan].space_freq = audio_config_p->achan[chan].mark_freq;	// Not Used.

	        // symbol time is 1 / (third of bps)
	        ticks_per_bit[chan] = (int) ((TICKS_PER_CYCLE / ((double)audio_config_p->achan[chan].baud / 3.)) + 0.5);
	        f1_change_per_sample[chan] = (int) (((double)audio_config_p->achan[chan].mark_freq * TICKS_PER_CYCLE / (double)audio_config_p->adev[a].samples_per_sec ) + 0.5);
	        f2_change_per_sample[chan] = f1_change_per_sample[chan];	// Not used.
	        break;

	      case MODEM_BASEBAND:
	      case MODEM_SCRAMBLE:
	      case MODEM_AIS:

		// Tone is half baud.
	        ticks_per_bit[chan] = (int) ((TICKS_PER_CYCLE / (double)audio_config_p->achan[chan].baud ) + 0.5);
	        f1_change_per_sample[chan] = (int) (((double)audio_config_p->achan[chan].baud * 0.5 * TICKS_PER_CYCLE / (double)audio_config_p->adev[a].samples_per_sec ) + 0.5);
	        break;

	      default:		// AFSK

	        ticks_per_bit[chan] = (int) ((TICKS_PER_CYCLE / (double)audio_config_p->achan[chan].baud ) + 0.5);
	        f1_change_per_sample[chan] = (int) (((double)audio_config_p->achan[chan].mark_freq * TICKS_PER_CYCLE / (double)audio_config_p->adev[a].samples_per_sec ) + 0.5);
	        f2_change_per_sample[chan] = (int) (((double)audio_config_p->achan[chan].space_freq * TICKS_PER_CYCLE / (double)audio_config_p->adev[a].samples_per_sec ) + 0.5);
	        break;
	    }
	  }
	}

        for (j=0; j<256; j++) {
	  double a;
	  int s;

	  a = ((double)(j) / 256.0) * (2 * M_PI);
	  s = (int) (sin(a) * 32767 * amp / 100.0);

	  /* 16 bit sound sample must fit in range of -32768 .. +32767. */
	
	  if (s < -32768) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("gen_tone_init: Excessive amplitude is being clipped.\n");
	    s = -32768;
	  }
	  else if (s > 32767) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("gen_tone_init: Excessive amplitude is being clipped.\n");
	    s = 32767;
	  }
	  sine_table[j] = s;
        }
	
		//Resets scrambler parameters and NRZI to be sure to start in a known state.
		gen_tone_reset(chan);
		hdlc_send_reset_nrzi(chan);

	return (0);

 } /* end gen_tone_init */


/*-------------------------------------------------------------------
 *
 * Name:        tone_gen_put_bit
 *
 * Purpose:     Generate tone of proper duration for one data bit.
 *
 * Inputs:      chan	- Audio channel, 0 = first.
 *
 *		dat	- 0 for f1, 1 for f2.
 *
 * 			  	-1 inserts half bit to test data	
 *				recovery PLL.
 *
 * Assumption:  fp is open to a file for write.
 *
 * Version 1.4:	Attempt to implement 2400 and 4800 bps PSK modes.
 *
 * Version 1.6: For G3RUH, rather than generating square wave and low
 *		pass filtering, generate the waveform directly.
 *		This avoids overshoot, ringing, and adding more jitter.
 *		Alternating bits come out has sine wave of baud/2 Hz.
 *
 * Version 1.6:	MFJ-2400 compatibility for V.26.
 *
 *--------------------------------------------------------------------*/

static const int gray2phase_v26[4] = {0, 1, 3, 2};
static const int gray2phase_v27[8] = {1, 0, 2, 3, 6, 7, 5, 4};


void tone_gen_put_bit_no_scramble (int chan, int dat)
{
	int a = ACHAN2ADEV(chan);	/* device for channel. */

	assert (save_audio_config_p != NULL);

	if (save_audio_config_p->achan[chan].medium != MEDIUM_RADIO) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Invalid channel %d for tone generation.\n", chan);
	  return;
	}

        if (dat < 0) { 
	  /* Hack to test receive PLL recovery. */
	  bit_len_acc[chan] -= ticks_per_bit[chan]; 
	  dat = 0; 
	} 

// TODO: change to switch instead of if if if

	if (save_audio_config_p->achan[chan].modem_type == MODEM_QPSK) {

	  int dibit;
	  int symbol;

	  dat &= 1;	// Keep only LSB to be extra safe.

	  if ( ! (bit_count[chan] & 1)) {
	    save_bit[chan] = dat;
	    bit_count[chan]++;
	    return;
	  }

	  // All zero bits should give us steady 1800 Hz.
	  // All one bits should flip phase by 180 degrees each time.

	  dibit = (save_bit[chan] << 1) | dat;

	  symbol = gray2phase_v26[dibit];
	  tone_phase[chan] += symbol * PHASE_SHIFT_90;
	  if (save_audio_config_p->achan[chan].v26_alternative == V26_B) {
	    tone_phase[chan] += PHASE_SHIFT_45;
	  }
	  bit_count[chan]++;
	}

	if (save_audio_config_p->achan[chan].modem_type == MODEM_8PSK) {

	  int tribit;
	  int symbol;

	  dat &= 1;	// Keep only LSB to be extra safe.

	  if (bit_count[chan] < 2) {
	    save_bit[chan] = (save_bit[chan] << 1) | dat;
	    bit_count[chan]++;
	    return;
	  }

	  // The bit pattern 001 should give us steady 1800 Hz.
	  // All one bits should flip phase by 180 degrees each time.

	  tribit = (save_bit[chan] << 1) | dat;

	  symbol = gray2phase_v27[tribit];
	  tone_phase[chan] += symbol * PHASE_SHIFT_45;

	  save_bit[chan] = 0;
	  bit_count[chan] = 0;
	}

	  
	do {		/* until enough audio samples for this symbol. */

	  int sam;

	  switch (save_audio_config_p->achan[chan].modem_type) {

	    case MODEM_AFSK:

#if DEBUG2
	      text_color_set(DW_COLOR_DEBUG);
	      dw_printf ("tone_gen_put_bit %d AFSK\n", __LINE__);
#endif
	      tone_phase[chan] += dat ? f2_change_per_sample[chan] : f1_change_per_sample[chan];
              sam = sine_table[(tone_phase[chan] >> 24) & 0xff];
	      gen_tone_put_sample (chan, a, sam);
	      break;

	    case MODEM_QPSK:
	    case MODEM_8PSK:

#if DEBUG2
	      text_color_set(DW_COLOR_DEBUG);
	      dw_printf ("tone_gen_put_bit %d PSK\n", __LINE__);
#endif
	      tone_phase[chan] += f1_change_per_sample[chan];
              sam = sine_table[(tone_phase[chan] >> 24) & 0xff];
	      gen_tone_put_sample (chan, a, sam);
	      break;

	    case MODEM_BASEBAND:
	    case MODEM_SCRAMBLE:
	    case MODEM_AIS:

	      if (dat != prev_dat[chan]) {
	        tone_phase[chan] += f1_change_per_sample[chan];
	      }
	      else {
	        if (tone_phase[chan] & 0x80000000)
	          tone_phase[chan] = 0xc0000000;	// 270 degrees.
	        else
	          tone_phase[chan] = 0x40000000;	// 90 degrees.
	      }
              sam = sine_table[(tone_phase[chan] >> 24) & 0xff];
	      gen_tone_put_sample (chan, a, sam);
	      break;

	    default:
	      text_color_set(DW_COLOR_ERROR);
	      dw_printf ("INTERNAL ERROR: %s %d achan[%d].modem_type = %d\n",
				__FILE__, __LINE__, chan, save_audio_config_p->achan[chan].modem_type);
	      exit (EXIT_FAILURE);
	  }

	  /* Enough for the bit time? */

	  bit_len_acc[chan] += ticks_per_sample[chan];

        } while (bit_len_acc[chan] < ticks_per_bit[chan]);

	bit_len_acc[chan] -= ticks_per_bit[chan];

	prev_dat[chan] = dat;		// Only needed for G3RUH baseband/scrambled.

}  /* end tone_gen_put_bit */

void tone_gen_put_bit (int chan, int dat)
{
	int a = ACHAN2ADEV(chan);	/* device for channel. */

	assert (save_audio_config_p != NULL);

	if (save_audio_config_p->achan[chan].medium != MEDIUM_RADIO) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Invalid channel %d for tone generation.\n", chan);
	  return;
	}

        if (dat < 0) { 
	  /* Hack to test receive PLL recovery. */
	  bit_len_acc[chan] -= ticks_per_bit[chan]; 
	  dat = 0; 
	} 

// TODO: change to switch instead of if if if

	if (save_audio_config_p->achan[chan].modem_type == MODEM_QPSK) {

	  int dibit;
	  int symbol;

	  dat &= 1;	// Keep only LSB to be extra safe.

	  if ( ! (bit_count[chan] & 1)) {
	    save_bit[chan] = dat;
	    bit_count[chan]++;
	    return;
	  }

	  // All zero bits should give us steady 1800 Hz.
	  // All one bits should flip phase by 180 degrees each time.

	  dibit = (save_bit[chan] << 1) | dat;

	  symbol = gray2phase_v26[dibit];
	  tone_phase[chan] += symbol * PHASE_SHIFT_90;
	  if (save_audio_config_p->achan[chan].v26_alternative == V26_B) {
	    tone_phase[chan] += PHASE_SHIFT_45;
	  }
	  bit_count[chan]++;
	}

	if (save_audio_config_p->achan[chan].modem_type == MODEM_8PSK) {

	  int tribit;
	  int symbol;

	  dat &= 1;	// Keep only LSB to be extra safe.

	  if (bit_count[chan] < 2) {
	    save_bit[chan] = (save_bit[chan] << 1) | dat;
	    bit_count[chan]++;
	    return;
	  }

	  // The bit pattern 001 should give us steady 1800 Hz.
	  // All one bits should flip phase by 180 degrees each time.

	  tribit = (save_bit[chan] << 1) | dat;

	  symbol = gray2phase_v27[tribit];
	  tone_phase[chan] += symbol * PHASE_SHIFT_45;

	  save_bit[chan] = 0;
	  bit_count[chan] = 0;
	}

	if (save_audio_config_p->achan[chan].modem_type == MODEM_SCRAMBLE) {
	  int x;

	  x = (dat ^ (lfsr[chan] >> 16) ^ (lfsr[chan] >> 11)) & 1;
	  lfsr[chan] = (lfsr[chan] << 1) | (x & 1);
	  dat = x;
	}
	  
	do {		/* until enough audio samples for this symbol. */

	  int sam;

	  switch (save_audio_config_p->achan[chan].modem_type) {

	    case MODEM_AFSK:

#if DEBUG2
	      text_color_set(DW_COLOR_DEBUG);
	      dw_printf ("tone_gen_put_bit %d AFSK\n", __LINE__);
#endif
	      tone_phase[chan] += dat ? f2_change_per_sample[chan] : f1_change_per_sample[chan];
              sam = sine_table[(tone_phase[chan] >> 24) & 0xff];
	      gen_tone_put_sample (chan, a, sam);
	      break;

	    case MODEM_QPSK:
	    case MODEM_8PSK:

#if DEBUG2
	      text_color_set(DW_COLOR_DEBUG);
	      dw_printf ("tone_gen_put_bit %d PSK\n", __LINE__);
#endif
	      tone_phase[chan] += f1_change_per_sample[chan];
              sam = sine_table[(tone_phase[chan] >> 24) & 0xff];
	      gen_tone_put_sample (chan, a, sam);
	      break;

	    case MODEM_BASEBAND:
	    case MODEM_SCRAMBLE:
	    case MODEM_AIS:

	      if (dat != prev_dat[chan]) {
	        tone_phase[chan] += f1_change_per_sample[chan];
	      }
	      else {
	        if (tone_phase[chan] & 0x80000000)
	          tone_phase[chan] = 0xc0000000;	// 270 degrees.
	        else
	          tone_phase[chan] = 0x40000000;	// 90 degrees.
	      }
              sam = sine_table[(tone_phase[chan] >> 24) & 0xff];
	      gen_tone_put_sample (chan, a, sam);
	      break;

	    default:
	      text_color_set(DW_COLOR_ERROR);
	      dw_printf ("INTERNAL ERROR: %s %d achan[%d].modem_type = %d\n",
				__FILE__, __LINE__, chan, save_audio_config_p->achan[chan].modem_type);
	      exit (EXIT_FAILURE);
	  }

	  /* Enough for the bit time? */

	  bit_len_acc[chan] += ticks_per_sample[chan];

        } while (bit_len_acc[chan] < ticks_per_bit[chan]);

	bit_len_acc[chan] -= ticks_per_bit[chan];

	prev_dat[chan] = dat;		// Only needed for G3RUH baseband/scrambled.

}  /* end tone_gen_put_bit_no_scramble */

void gen_tone_reset(int chan){
	if (chan < MAX_CHANS && save_audio_config_p->achan[chan].modem_type == MODEM_SCRAMBLE) {
	  lfsr[chan] = 0;
	  prev_dat[chan] = 0;
	  dw_printf("Reset lfsr buffer.\n");
	  tone_phase[chan] = 0;
	}
}

void gen_tone_put_sample (int chan, int a, int sam) {

        /* Ship out an audio sample. */
	/* 16 bit is signed, little endian, range -32768 .. +32767 */
	/* 8 bit is unsigned, range 0 .. 255 */

	assert (save_audio_config_p != NULL);

	assert (save_audio_config_p->adev[a].num_channels == 1 || save_audio_config_p->adev[a].num_channels == 2);

	assert (save_audio_config_p->adev[a].bits_per_sample == 16 || save_audio_config_p->adev[a].bits_per_sample == 8);

	// Bad news if we are clipping and distorting the signal.
	// We are using the full range.
	// Too late to change now because everyone would need to recalibrate their
	// transmit audio level.

	if (sam < -32767) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Warning: Audio sample %d clipped to -32767.\n", sam);
	  sam = -32767;
	}
	else if (sam > 32767) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Warning: Audio sample %d clipped to +32767.\n", sam);
	  sam = 32767;
	}

	if (save_audio_config_p->adev[a].num_channels == 1) {

	  /* Mono */

	  if (save_audio_config_p->adev[a].bits_per_sample == 8) {
            audio_put (a, ((sam+32768) >> 8) & 0xff);
	  }
	  else {
            audio_put (a, sam & 0xff);
            audio_put (a, (sam >> 8) & 0xff);
	  }
 	}
	else {

	  if (chan == ADEVFIRSTCHAN(a)) {
	  
	    /* Stereo, left channel. */

	    if (save_audio_config_p->adev[a].bits_per_sample == 8) {
              audio_put (a, ((sam+32768) >> 8) & 0xff);
              audio_put (a, 0);
	    }
	    else {
              audio_put (a, sam & 0xff);
              audio_put (a, (sam >> 8) & 0xff);
 
              audio_put (a, 0);
              audio_put (a, 0);
	    }
	  }
	  else { 

	    /* Stereo, right channel. */
	  
	    if (save_audio_config_p->adev[a].bits_per_sample == 8) {
              audio_put (a, 0);
              audio_put (a, ((sam+32768) >> 8) & 0xff);
	    }
	    else {
              audio_put (a, 0);
              audio_put (a, 0);

              audio_put (a, sam & 0xff);
              audio_put (a, (sam >> 8) & 0xff);
	    }
	  }
	}
}



/*-------------------------------------------------------------------
 *
 * Name:        main
 *
 * Purpose:     Quick test program for above.
 *
 * Description: Compile like this for unit test:
 *
 *		gcc -Wall -DMAIN -o gen_tone_test gen_tone.c audio.c textcolor.c
 *
 *		gcc -Wall -DMAIN -o gen_tone_test.exe gen_tone.c audio_win.c textcolor.c -lwinmm
 *
 *--------------------------------------------------------------------*/


#if MAIN


int main ()
{
	int n;
	int chan1 = 0;
	int chan2 = 1;
	int r;
	struct audio_s my_audio_config;


/* to sound card */
/* one channel.  2 times:  one second of each tone. */

	memset (&my_audio_config, 0, sizeof(my_audio_config));
	strlcpy (my_audio_config.adev[0].adevice_in, DEFAULT_ADEVICE, sizeof(my_audio_config.adev[0].adevice_in));
	strlcpy (my_audio_config.adev[0].adevice_out, DEFAULT_ADEVICE, sizeof(my_audio_config.adev[0].adevice_out));

	audio_open (&my_audio_config);
	gen_tone_init (&my_audio_config, 100);

	for (r=0; r<2; r++) {

	  for (n=0; n<my_audio_config.baud[0] * 2 ; n++) {
 	    tone_gen_put_bit ( chan1, 1 );
	  }

	  for (n=0; n<my_audio_config.baud[0] * 2 ; n++) {
 	    tone_gen_put_bit ( chan1, 0 );
	  }
	}

	audio_close();

/* Now try stereo. */

	memset (&my_audio_config, 0, sizeof(my_audio_config));
	strlcpy (my_audio_config.adev[0].adevice_in, DEFAULT_ADEVICE, sizeof(my_audio_config.adev[0].adevice_in));
	strlcpy (my_audio_config.adev[0].adevice_out, DEFAULT_ADEVICE, , sizeof(my_audio_config.adev[0].adevice_out));
	my_audio_config.adev[0].num_channels = 2;

	audio_open (&my_audio_config);
	gen_tone_init (&my_audio_config, 100);

	for (r=0; r<4; r++) {

	  for (n=0; n<my_audio_config.baud[0] * 2 ; n++) {
 	    tone_gen_put_bit ( chan1, 1 );
	  }

	  for (n=0; n<my_audio_config.baud[0] * 2 ; n++) {
 	    tone_gen_put_bit ( chan1, 0 );
	  }

	  for (n=0; n<my_audio_config.baud[0] * 2 ; n++) {
 	    tone_gen_put_bit ( chan2, 1 );
	  }

	  for (n=0; n<my_audio_config.baud[0] * 2 ; n++) {
 	    tone_gen_put_bit ( chan2, 0 );
	  }
	}

	audio_close();

	return(0);
}

#endif


/* end gen_tone.c */
