/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include "system.h"
#include "power.h"
#include "powermgmt.h"
#include "lcd.h"
#include "led.h"
#include "audio.h"
#include "thread.h"
#include "enc_config.h"
#include "playback.h"
#if defined(HAVE_SPDIF_IN) || defined(HAVE_SPDIF_OUT)
#include "spdif.h"
#endif
#include "pcm_record.h"
#include "recording.h"
#include "button.h"
#include "kernel.h"
#include "settings.h"
#include "lang.h"
#include "font.h"
#include "icons.h"
#include "icon.h"
#include "screens.h"
#include "peakmeter.h"
#include "menu.h"
#include "sound_menu.h"
#include "timefuncs.h"
#include "debug.h"
#include "misc.h"
#include "pathfuncs.h"
#include "tree.h"
#include "string.h"
#include "dir.h"
#include "errno.h"
#include "talk.h"
#include "sound.h"
#include "storage.h"
#if (CONFIG_STORAGE & STORAGE_ATA) && (CONFIG_LED == LED_REAL) \
   && !defined(SIMULATOR)
#include "ata.h"
#endif
#include "splash.h"
#include "screen_access.h"
#include "action.h"
#ifdef HAVE_FMRADIO_REC
# include "radio.h"
#endif
#include "viewport.h"
#include "list.h"
#include "general.h"
#include "appevents.h"

#ifdef HAVE_RECORDING
/* This array holds the record size interval lengths, in mebibytes */
static const unsigned short rec_size_mbytes[] =
{
    0,     /* 0 means OFF */
    5,     /* 5MB */
    10,    /* 10MB */
    15,    /* 15MB */
    32,    /* 32MB */
    64,    /* 64MB */
    75,    /* 75MB */
    100,   /* 100MB */
    128,   /* 128MB */
    256,   /* 256MB */
    512,   /* 512MB */
    650,   /* 650MB */
    700,   /* 700MB */
    1024,  /* 1GB */
    1536,  /* 1.5GB */
    1792,  /* 1.75GB  */
};

static unsigned long rec_sizesplit_bytes(void)
{
    unsigned long sz_mbytes = rec_size_mbytes[global_settings.rec_sizesplit];
    unsigned long sz_bytes = sz_mbytes << 20;
    return sz_bytes;
}

void settings_apply_trigger(void)
{
    int start_thres, stop_thres;
    if (global_settings.peak_meter_dbfs)
    {
        start_thres = global_settings.rec_start_thres_db - 1;
        stop_thres = global_settings.rec_stop_thres_db - 1;
    }
    else
    {
        start_thres = global_settings.rec_start_thres_linear;
        stop_thres = global_settings.rec_stop_thres_linear;
    }

    peak_meter_define_trigger(
        start_thres,
        global_settings.rec_start_duration*HZ,
        MIN(global_settings.rec_start_duration*HZ / 2, 2*HZ),
        stop_thres,
        global_settings.rec_stop_postrec*HZ,
        global_settings.rec_stop_gap*HZ
    );
}
/* recording screen status flags */
enum rec_status_flags
{
    RCSTAT_IN_RECSCREEN      = 0x00000001,
    RCSTAT_BEEN_IN_USB_MODE  = 0x00000002,
    RCSTAT_CREATED_DIRECTORY = 0x00000004,
    RCSTAT_HAVE_RECORDED     = 0x00000008,
};

static int rec_status = 0;

bool in_recording_screen(void)
{
    return (rec_status & RCSTAT_IN_RECSCREEN) != 0;
}

#define MAX_FILE_SIZE 0x7F800000 /* 2 GB - 4 MB */

#ifndef HAVE_REMOTE_LCD
static const int screen_update = NB_SCREENS;
#else
static int screen_update = NB_SCREENS;
static bool remote_display_on = true;
#endif

/* as we have the ability to disable the remote, we need an alternative loop */
#define FOR_NB_ACTIVE_SCREENS(i) for(int i = 0; i < screen_update; i++)

static bool update_list = false;   /* (GIU) list needs updating */

/** File name creation **/
#if CONFIG_RTC == 0
/* current file number to assist in creating unique numbered filenames
   without actually having to create the file on disk */
static int file_number = -1;
#endif /* CONFIG_RTC */

#define REC_FILE_ENDING(rec_format) \
    (audio_formats[rec_format_afmt[rec_format]].ext_list)

/* path for current file */
static char path_buffer[MAX_PATH];

/** Automatic Gain Control (AGC) **/
#ifdef HAVE_AGC
/* Timing counters:
 * peak_time is incremented every 0.2s, every 2nd run of record screen loop.
 * hist_time is incremented every 0.5s, display update.
 * peak_time is the counter of the peak hold read and agc process,
 * overflow every 13 years 8-)
 */
static long peak_time = 0;

static short peak_valid_mem[4];
#define BAL_MEM_SIZE 24
static short balance_mem[BAL_MEM_SIZE];

/* Automatic Gain Control */
#define AGC_MODE_SIZE 5
#define AGC_SAFETY_MODE 0

/* Note iriver devices overwrite these strings */ 
static const char* agc_preset_str[] =
{ "Off", "S", "L", "D", "M", "V" };
/*  "Off",
    "Safety (clip)",
    "Live (slow)",
    "DJ-Set (slow)",
    "Medium",
    "Voice (fast)"  */
#define AGC_CLIP 32766
#define AGC_PEAK 29883 /* fast gain reduction threshold -0.8dB */
#define AGC_HIGH 27254 /* accelerated gain reduction threshold -1.6dB */
#define AGC_IMG    823 /* threshold for balance control -32dB */
/* autogain high level thresholds (-3dB, -7dB, -4dB, -5dB, -5dB) */
static const short agc_th_hi[AGC_MODE_SIZE] =
{ 23197, 14637, 21156, 18428, 18426 };
/* autogain low level thresholds (-14dB, -11dB, -6dB, -7dB, -8dB) */
static const short agc_th_lo[AGC_MODE_SIZE] =
{ 6538, 9235, 16422, 14636, 13045 };
/* autogain threshold times [1/5s] or [200ms] */
static const short agc_tdrop[AGC_MODE_SIZE] =
{ 900,  225, 150, 60, 8 };
static const short agc_trise[AGC_MODE_SIZE] =
{ 9000, 750, 400, 150, 20 };
static const short agc_tbal[AGC_MODE_SIZE] =
{ 4500, 500, 300, 100, 15 };
/* AGC operation */
static bool agc_enable = true;
static short agc_preset;
/* AGC levels */
static int agc_left = 0;
static int agc_right = 0;
/* AGC time since high target volume was exceeded */
static short agc_droptime = 0;
/* AGC time since volume fallen below low target */
static short agc_risetime = 0;
/* AGC balance time exceeding +/- 0.7dB */
static short agc_baltime = 0;
/* AGC maximum gain */
static short agc_maxgain;
#endif /* HAVE_AGC */
#if defined(HAVE_AGC)
static long hist_time = 0;
#endif /* HAVE_AGC */

static void set_gain(void)
{
    switch(global_settings.rec_source) {
#ifdef HAVE_MIC_REC
    case AUDIO_SRC_MIC:
        if (global_settings.rec_mic_gain > sound_max(SOUND_MIC_GAIN))
            global_settings.rec_mic_gain = sound_max(SOUND_MIC_GAIN);

        if (global_settings.rec_mic_gain < sound_min(SOUND_MIC_GAIN))
            global_settings.rec_mic_gain = sound_min(SOUND_MIC_GAIN);

        audio_set_recording_gain(global_settings.rec_mic_gain,
                                 0, AUDIO_GAIN_MIC);
        break;
#endif /* MIC */
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
    HAVE_LINE_REC_(case AUDIO_SRC_LINEIN:)
    HAVE_FMRADIO_REC_(case AUDIO_SRC_FMRADIO:)
        if (global_settings.rec_left_gain > sound_max(SOUND_LEFT_GAIN))
            global_settings.rec_left_gain = sound_max(SOUND_LEFT_GAIN);

        if (global_settings.rec_left_gain < sound_min(SOUND_LEFT_GAIN))
            global_settings.rec_left_gain = sound_min(SOUND_LEFT_GAIN);

        if (global_settings.rec_right_gain > sound_max(SOUND_RIGHT_GAIN))
            global_settings.rec_right_gain = sound_max(SOUND_RIGHT_GAIN);

        if (global_settings.rec_right_gain < sound_min(SOUND_RIGHT_GAIN))
            global_settings.rec_right_gain = sound_min(SOUND_RIGHT_GAIN);

        audio_set_recording_gain(global_settings.rec_left_gain,
                                 global_settings.rec_right_gain,
                                 AUDIO_GAIN_LINEIN);
        break;
#endif
    }

    /* reset the clipping indicators */
    peak_meter_set_clip_hold(global_settings.peak_meter_clip_hold);
    update_list = true;
}

#ifdef HAVE_AGC
/* Read peak meter values & calculate balance.
 * Returns validity of peak values.
 * Used for automatic gain control and history diagram.
 */
static bool read_peak_levels(int *peak_l, int *peak_r, int *balance)
{
    peak_meter_get_peakhold(peak_l, peak_r);
    peak_valid_mem[peak_time % 3] = *peak_l;
    if (((peak_valid_mem[0] == peak_valid_mem[1]) &&
         (peak_valid_mem[1] == peak_valid_mem[2])) &&
        ((*peak_l < 32767) || storage_disk_is_active()))
            return false;

    if (*peak_r > *peak_l)
        balance_mem[peak_time % BAL_MEM_SIZE] = (*peak_l ?
            MIN((10000 * *peak_r) / *peak_l - 10000, 15118) : 15118);
    else
        balance_mem[peak_time % BAL_MEM_SIZE] = (*peak_r ?
            MAX(10000 - (10000 * *peak_l) / *peak_r, -15118) : -15118);
    *balance = 0;
    int i;
    for (i = 0; i < BAL_MEM_SIZE; i++)
        *balance += balance_mem[i];
    *balance = *balance / BAL_MEM_SIZE;

    return true;
}

/* AGC helper function to check if maximum gain is reached */
static bool agc_gain_is_max(bool left, bool right)
{
    /* range -128...+108 [0.5dB] */
    short gain_current_l;
    short gain_current_r;

    if (agc_preset == 0)
        return false;

    switch (global_settings.rec_source)
    {
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
    HAVE_LINE_REC_(case AUDIO_SRC_LINEIN:)
    HAVE_FMRADIO_REC_(case AUDIO_SRC_FMRADIO:)
        gain_current_l = global_settings.rec_left_gain;
        gain_current_r = global_settings.rec_right_gain;
        break;
#endif /* LINE, FMRADIO */
#if defined(HAVE_MIC_REC)
    case AUDIO_SRC_MIC:
    default:
        gain_current_l = global_settings.rec_mic_gain;
        gain_current_r = global_settings.rec_mic_gain;
#endif /* MIC */
    }

    return ((left && (gain_current_l >= agc_maxgain)) ||
            (right && (gain_current_r >= agc_maxgain)));
}

static void change_recording_gain(bool increment, bool left, bool right)
{
#if !defined(HAVE_LINE_REC) || !defined(HAVE_FMRADIO_REC)
    (void)left;
    (void)right;
#endif

    int factor = (increment ? 1 : -1);

    switch (global_settings.rec_source)
    {
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
    HAVE_LINE_REC_(case AUDIO_SRC_LINEIN:)
    HAVE_FMRADIO_REC_(case AUDIO_SRC_FMRADIO:)
        if (left) global_settings.rec_left_gain +=
                      factor * sound_steps(SOUND_LEFT_GAIN);
        if (right) global_settings.rec_right_gain +=
                       factor * sound_steps(SOUND_RIGHT_GAIN);
        break;
#endif /* LINE, FMRADIO */
#if defined(HAVE_MIC_REC)
    case AUDIO_SRC_MIC:
         global_settings.rec_mic_gain += factor * sound_steps(SOUND_MIC_GAIN);
#endif
    }
}

/*
 * Handle automatic gain control (AGC).
 * Change recording gain if peak_x levels are above or below
 * target volume for specified timeouts.
 */
static void auto_gain_control(int *peak_l, int *peak_r, int *balance)
{
    int agc_mono;
    short agc_mode;
    bool increment;

    if (*peak_l > agc_left)
        agc_left = *peak_l;
    else
        agc_left -= (agc_left - *peak_l + 3) >> 2;
    if (*peak_r > agc_right)
        agc_right = *peak_r;
    else
        agc_right -= (agc_right - *peak_r + 3) >> 2;
    agc_mono = (agc_left + agc_right) / 2;

    agc_mode = abs(agc_preset) - 1;
    if (agc_mode < 0) {
        agc_enable = false;
        return;
    }

    if (agc_mode != AGC_SAFETY_MODE) {
        /* Automatic balance control - only if not in safety mode */
        if ((agc_left > AGC_IMG) && (agc_right > AGC_IMG))
        {
            if (*balance < -556)
            {
                if (*balance > -900)
                    agc_baltime -= !(peak_time % 4); /* 0.47 - 0.75dB */
                else if (*balance > -4125)
                    agc_baltime--;                   /* 0.75 - 3.00dB */
                else if (*balance > -7579)
                    agc_baltime -= 2;                /* 3.00 - 4.90dB */
                else
                    agc_baltime -= !(peak_time % 8); /* 4.90 - inf dB */
                if (agc_baltime > 0)
                    agc_baltime -= (peak_time % 2);
            }
            else if (*balance > 556)
            {
                if (*balance < 900)
                    agc_baltime += !(peak_time % 4);
                else if (*balance < 4125)
                    agc_baltime++;
                else if (*balance < 7579)
                    agc_baltime += 2;
                else
                    agc_baltime += !(peak_time % 8);
                if (agc_baltime < 0)
                    agc_baltime += (peak_time % 2);
            }

            if ((*balance * agc_baltime) < 0)
            {
                if (*balance < 0)
                    agc_baltime -= peak_time % 2;
                else
                    agc_baltime += peak_time % 2;
            }

            increment = ((agc_risetime / 2) > agc_droptime);

            if (agc_baltime < -agc_tbal[agc_mode])
            {
                if (!increment || !agc_gain_is_max(!increment, increment)) {
                    change_recording_gain(increment, !increment, increment);
                    set_gain();
                }
                agc_baltime = 0;
            }
            else if (agc_baltime > +agc_tbal[agc_mode])
            {
                if (!increment || !agc_gain_is_max(increment, !increment)) {
                    change_recording_gain(increment, increment, !increment);
                    set_gain();
                }
                agc_baltime = 0;
            }
        }
        else if (!(hist_time % 4))
        {
            if (agc_baltime < 0)
                agc_baltime++;
            else
                agc_baltime--;
        }
    }

    /* Automatic gain control */
    if ((agc_left > agc_th_hi[agc_mode]) || (agc_right > agc_th_hi[agc_mode]))
    {
        if ((agc_left > AGC_CLIP) || (agc_right > AGC_CLIP))
            agc_droptime += agc_tdrop[agc_mode] /
                            (global_settings.rec_agc_cliptime + 1);
        if (agc_left  > AGC_HIGH) {
            agc_droptime++;
            agc_risetime = 0;
            if (agc_left > AGC_PEAK)
                agc_droptime += 2;
        }
        if (agc_right > AGC_HIGH) {
            agc_droptime++;
            agc_risetime=0;
            if (agc_right > AGC_PEAK)
                agc_droptime += 2;
        }
        if (agc_mono > agc_th_hi[agc_mode])
            agc_droptime++;
        else
            agc_droptime += !(peak_time % 2);

        if (agc_droptime >= agc_tdrop[agc_mode])
        {
            change_recording_gain(false, true, true);
            agc_droptime = 0;
            agc_risetime = 0;
            set_gain();
        }
        agc_risetime = MAX(agc_risetime - 1, 0);
    }
    else if (agc_mono < agc_th_lo[agc_mode])
    {
        if (agc_mono < (agc_th_lo[agc_mode] / 8))
            agc_risetime += !(peak_time % 5);
        else if (agc_mono < (agc_th_lo[agc_mode] / 2))
            agc_risetime += 2;
        else
            agc_risetime++;

        if (agc_risetime >= agc_trise[agc_mode]) {
            if ((agc_mode != AGC_SAFETY_MODE) &&
                (!agc_gain_is_max(true, true))) {
                change_recording_gain(true, true, true);
                set_gain();
            }
            agc_risetime = 0;
            agc_droptime = 0;
        }
        agc_droptime = MAX(agc_droptime - 1, 0);
    }
    else if (!(peak_time % 6)) /* on target level every 1.2 sec */
    {
        agc_risetime = MAX(agc_risetime - 1, 0);
        agc_droptime = MAX(agc_droptime - 1, 0);
    }
}
#endif /* HAVE_AGC */

static char *fmt_gain(int snd, int val, char *str, int len)
{
    format_sound_value(str, len, snd, val);
    return str;
}

/* the list below must match enum audio_sources in audio.h */
static const char* const prestr[] =
{
    HAVE_MIC_IN_([AUDIO_SRC_MIC]          = "R_MIC_",)
    HAVE_LINE_REC_([AUDIO_SRC_LINEIN]     = "R_LINE_",)
    HAVE_SPDIF_IN_([AUDIO_SRC_SPDIF]      = "R_SPDIF_",)
    HAVE_FMRADIO_REC_([AUDIO_SRC_FMRADIO] = "R_FM_",)
};

char *rec_create_filename(char *buffer)
{
    char ext[16];
    const char *pref = "R_";

    /* Directory existence and writeablility should have already been
     * verified - do not pass NULL pointers to pcmrec */

    if((unsigned)global_settings.rec_source < AUDIO_NUM_SOURCES)
    {
        pref = prestr[global_settings.rec_source];
    }

    strcpy(buffer, !strcmp(global_settings.rec_directory, "/")?
                        "": global_settings.rec_directory);

    snprintf(ext, sizeof(ext), ".%s",
             REC_FILE_ENDING(global_settings.rec_format));

#if CONFIG_RTC == 0
    return create_numbered_filename(buffer, buffer, pref, ext, 4,
                                    &file_number);
#else
    /* We'll wait at least up to the start of the next second so no duplicate
       names are created */
    return create_datetime_filename(buffer, buffer, pref, ext, true);
#endif
}

#if CONFIG_RTC == 0
/* Hit disk to get a starting filename for the type */
static void rec_init_filename(void)
{
    file_number = -1;
    rec_create_filename(path_buffer);
    file_number--;
}
#endif

int rec_create_directory(void)
{
    int rc = 0;
    const char * const folder = global_settings.rec_directory;

    if (strcmp(folder, "/") && !dir_exists(folder))
    {
        rc = mkdir(folder);

        if(rc < 0)
        {
            while (action_userabort(HZ) == false)
            {
                splashf(0, "%s %s",
                        str(LANG_REC_DIR_NOT_WRITABLE),
                        str(LANG_OFF_ABORT));
            }
        }
        else
        {
            rec_status |= RCSTAT_CREATED_DIRECTORY;
            rc = 1;
        }
    }

    return rc;
}

void rec_init_recording_options(struct audio_recording_options *options)
{
    options->rec_source            = global_settings.rec_source;
    options->rec_frequency         = global_settings.rec_frequency;
    options->rec_channels          = global_settings.rec_channels;
    options->rec_prerecord_time    = global_settings.rec_prerecord_time;
    options->rec_mono_mode         = global_settings.rec_mono_mode;
    options->rec_source_flags      = 0;
    options->enc_config.rec_format = global_settings.rec_format;
    global_to_encoder_config(&options->enc_config);
}

void rec_set_source(int source, unsigned flags)
{
    /* Set audio input source, power up/down devices */
    audio_set_input_source(source, flags);

    /* Set peakmeters for recording or reset to playback */
    peak_meter_playback((flags & SRCF_RECORDING) == 0);
    peak_meter_enable(true);
}

void rec_set_recording_options(struct audio_recording_options *options)
{
    rec_set_source(options->rec_source,
                   options->rec_source_flags | SRCF_RECORDING);
    audio_set_recording_options(options);
}

void rec_command(enum recording_command cmd)
{
    switch(cmd)
    {
        case RECORDING_CMD_STOP_SHUTDOWN:
            pm_activate_clipcount(false);
            audio_stop_recording();
            audio_close_recording();
            sys_poweroff();
            break;
        case RECORDING_CMD_STOP:
            pm_activate_clipcount(false);
            audio_stop_recording();
            break;
        case RECORDING_CMD_START:
        case RECORDING_CMD_START_NEWFILE:
            /* create unique filename and start recording*/
            pm_reset_clipcount();
            pm_activate_clipcount(true);
            audio_record(rec_create_filename(path_buffer));
            break;
        case RECORDING_CMD_PAUSE:
            pm_activate_clipcount(false);
            audio_pause_recording();
            break;
        case RECORDING_CMD_RESUME:
            pm_activate_clipcount(true);
            audio_resume_recording();
            break;
    }
    update_list = true;
}

/* used in trigger_listerner and recording_screen */
static unsigned int last_seconds = 0;

/**
 * Callback function so that the peak meter code can send an event
 * to this application. This function can be passed to
 * peak_meter_set_trigger_listener in order to activate the trigger.
 */
static void trigger_listener(int trigger_status)
{
    switch (trigger_status)
    {
        case TRIG_GO:
            if(!(audio_status() & AUDIO_STATUS_RECORD))
            {
                rec_status |= RCSTAT_HAVE_RECORDED;
            }

            /* if we're already recording this is a retrigger */
            else
            {
                if((audio_status() & AUDIO_STATUS_PAUSE) &&
                    (global_settings.rec_trigger_type == TRIG_TYPE_PAUSE))
                {
                    rec_command(RECORDING_CMD_RESUME);
                }
                /* New file on trig start*/
                else if (global_settings.rec_trigger_type != TRIG_TYPE_NEW_FILE)
                {
                    rec_command(RECORDING_CMD_START_NEWFILE);
                    /* tell recording_screen to reset the time */
                    last_seconds = 0;
                }
            }
            break;

        /* A _change_ to TRIG_READY means the current recording has stopped */
        case TRIG_READY:
            if(audio_status() & AUDIO_STATUS_RECORD)
            {
                switch(global_settings.rec_trigger_type)
                {
                    case TRIG_TYPE_STOP: /* Stop */
                        rec_command(RECORDING_CMD_STOP);
                        break;

                    case TRIG_TYPE_PAUSE: /* Pause */
                        rec_command(RECORDING_CMD_PAUSE);
                        break;

                    case TRIG_TYPE_NEW_FILE: /* New file on trig stop*/
                        rec_command(RECORDING_CMD_START_NEWFILE);
                        /* tell recording_screen to reset the time */
                        last_seconds = 0;
                        break;

                    case 3: /* Stop and shutdown */
                        rec_command(RECORDING_CMD_STOP_SHUTDOWN);
                        break;
                 }

                if (global_settings.rec_trigger_mode != TRIG_MODE_REARM)
                {
                    peak_meter_set_trigger_listener(NULL);
                    peak_meter_trigger(false);
                }
            }
            break;
    }
}

/* Stuff for drawing the screen */

enum rec_list_items_stereo {
#ifndef HAVE_RECORDING_WITHOUT_MONITORING
    ITEM_VOLUME,
#endif
    ITEM_GAIN,
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
    ITEM_GAIN_L,
    ITEM_GAIN_R,
#endif
#ifdef HAVE_AGC
    ITEM_AGC_MODE,
    ITEM_AGC_MAXDB,
#endif
#ifdef HAVE_SPDIF_REC
    ITEM_SAMPLERATE,
#endif
    ITEM_FILENAME,
    ITEM_COUNT,
};

static int listid_to_enum[ITEM_COUNT];

static const char* reclist_get_name(int selected_item, void * data,
                                    char * buffer, size_t buffer_len)
{
    char buf2[32];
#ifdef HAVE_AGC
    char buf3[32];
#endif
    (void)data; /* not used */
    if(selected_item >= ITEM_COUNT)
        return "";

    switch (listid_to_enum[selected_item])
    {
#ifndef HAVE_RECORDING_WITHOUT_MONITORING
        case ITEM_VOLUME:
            snprintf(buffer, buffer_len, "%s: %s", str(LANG_VOLUME),
                     fmt_gain(SOUND_VOLUME,
                              global_status.volume,
                              buf2, sizeof(buf2)));
            break;
#endif
        case ITEM_GAIN:
            switch(global_settings.rec_source) {
#ifdef HAVE_MIC_REC
            case AUDIO_SRC_MIC:
                /* Draw MIC recording gain */
                snprintf(buffer, buffer_len, "%s: %s", str(LANG_GAIN),
                         fmt_gain(SOUND_MIC_GAIN,
                                  global_settings.rec_mic_gain,
                                  buf2, sizeof(buf2)));
                break;
#endif /* MIC */
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
            HAVE_LINE_REC_(case AUDIO_SRC_LINEIN:)
            HAVE_FMRADIO_REC_(case AUDIO_SRC_FMRADIO:) {
                int avg_gain = (global_settings.rec_left_gain +
                                global_settings.rec_right_gain) / 2;
                snprintf(buffer, buffer_len, "%s: %s", str(LANG_GAIN),
                         fmt_gain(SOUND_LEFT_GAIN,
                                  avg_gain,
                                  buf2, sizeof(buf2)));
            } break;
#endif
            }
            break;
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
        case ITEM_GAIN_L:
            snprintf(buffer, buffer_len, "%s: %s",
                     str(LANG_GAIN_LEFT),
                     fmt_gain(SOUND_LEFT_GAIN,
                              global_settings.rec_left_gain,
                              buf2, sizeof(buf2)));
            break;
        case ITEM_GAIN_R:
            snprintf(buffer, buffer_len, "%s: %s",
                     str(LANG_GAIN_RIGHT),
                     fmt_gain(SOUND_RIGHT_GAIN,
                              global_settings.rec_right_gain,
                              buf2, sizeof(buf2)));
            break;
#endif
#ifdef HAVE_AGC
        case ITEM_AGC_MODE:
            snprintf(buffer, buffer_len, "%s: %s",
                     str(LANG_RECORDING_AGC_PRESET),
                     agc_preset_str[agc_preset]);
            break;
        case ITEM_AGC_MAXDB: {
            int bias, which;
            switch(global_settings.rec_source) {
            default:
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
            HAVE_LINE_IN_(case AUDIO_SRC_LINEIN:)
            HAVE_FMRADIO_IN_(case AUDIO_SRC_FMRADIO:)
                which = SOUND_LEFT_GAIN;
                bias = (global_settings.rec_left_gain + global_settings.rec_right_gain) / 2;
                break;
#endif
#if defined(HAVE_MIC_REC)
            case AUDIO_SRC_MIC:
                which = SOUND_MIC_GAIN;
                bias = global_settings.rec_mic_gain;
                break;
#endif /* MIC*/
            }

            if(agc_preset == 0) {
                snprintf(buffer, buffer_len, "%s: %s",
                         str(LANG_RECORDING_AGC_MAXGAIN),
                         fmt_gain(which, agc_maxgain, buf2, sizeof(buf2)));
            } else {
                snprintf(buffer, buffer_len, "%s: %s (%s)",
                         str(LANG_RECORDING_AGC_MAXGAIN),
                         fmt_gain(which, agc_maxgain, buf2, sizeof(buf2)),
                         fmt_gain(which, agc_maxgain - bias, buf3, sizeof(buf3)));
            }
        } break;
#endif
#ifdef HAVE_SPDIF_REC
        case ITEM_SAMPLERATE:
            snprintf(buffer, buffer_len, "%s: %lu",
                     str(LANG_FREQUENCY), pcm_rec_sample_rate());
            break;
#endif
        case ITEM_FILENAME:
        {
            if(audio_status() & AUDIO_STATUS_RECORD)
            {
                size_t tot_len = strlen(path_buffer) +
                                strlen(str(LANG_RECORDING_FILENAME)) + 1;
                if(tot_len > buffer_len)
                {
                    snprintf(buffer, buffer_len, "%s %s",
                            str(LANG_RECORDING_FILENAME),
                            path_buffer + tot_len - buffer_len);
                }
                else
                {
                    snprintf(buffer, buffer_len, "%s %s",
                            str(LANG_RECORDING_FILENAME), path_buffer);
                }
            }
            else
            {
                return str(LANG_RECORDING_FILENAME);
            }
            break;
        }
        default:
            return "";
    }
    return buffer;
}

static void recording_step_levels(int setting_id, int steps)
{
    steps *= sound_steps(setting_id);
    switch(setting_id)
    {
        case SOUND_VOLUME:
            global_status.volume += steps;
            setvol();
            break;
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
        case SOUND_LEFT_GAIN:
            global_settings.rec_left_gain += steps;
            break;
        case SOUND_RIGHT_GAIN:
            global_settings.rec_right_gain += steps;
            break;
#endif
#ifdef HAVE_MIC_REC
        case SOUND_MIC_GAIN:
            global_settings.rec_mic_gain += steps;
            break;
#endif /* MIC */
        default:
            break;
    }
}

bool recording_start_automatic = false;

bool recording_screen(bool no_source)
{
    int button;
    int done = -1;      /* negative to re-init, positive to quit, zero to run */
    char buf[32];       /* for preparing strings */
    char buf2[16];      /* for preparing strings */
    int w, h;           /* character width/height */
    int update_countdown = 0;   /* refresh counter */
    unsigned int seconds;
    int hours, minutes;
    int audio_stat = 0;         /* status of the audio system */
    int last_audio_stat = -1;   /* previous status so we can act on changes */
    struct viewport vp_list[NB_SCREENS], vp_top[NB_SCREENS]; /* the viewports */
    const unsigned long split_seconds = (unsigned) global_settings.rec_timesplit * 60;
    const unsigned long split_bytes = rec_sizesplit_bytes();

    int warning_counter = 0;
    #define WARNING_PERIOD 7

#ifdef HAVE_SPDIF_REC
    unsigned long prev_sample_rate = 0;
#endif

#ifdef HAVE_FMRADIO_REC
    /* Radio is left on if:
     *   1) Is was on at the start and the initial source is FM Radio
     *   2) 1) and the source was never changed to something else
     */
    int radio_status = (global_settings.rec_source != AUDIO_SRC_FMRADIO) ?
                            FMRADIO_OFF : get_radio_status();
#endif
#if (CONFIG_LED == LED_REAL)
    bool led_state = false;
    int led_countdown = 2;
#endif
#ifdef HAVE_AGC
    bool peak_read = false;
    int peak_l, peak_r;
    int balance = 0;
#endif
    int pm_x[NB_SCREENS];           /* peakmeter (and trigger bar) x pos */
    int pm_y[NB_SCREENS];           /* peakmeter y pos */
    int pm_h[NB_SCREENS];           /* peakmeter height */
    int trig_ypos[NB_SCREENS];      /* trigger bar y pos */
    int trig_width[NB_SCREENS];     /* trigger bar width */
    int top_height_req[NB_SCREENS]; /* required height for top half */
                                     /* tweak layout tiny screens / big fonts */
    bool compact_view[NB_SCREENS] = { false };
    struct gui_synclist lists;      /* the list in the bottom vp */
#if defined(HAVE_AGC)
    bool peak_valid = false;
#endif
#if defined(HAVE_HISTOGRAM)
    unsigned short hist_pos_y = 0;
    unsigned short hist_size_h = 0;
#endif
#ifdef HAVE_FMRADIO_REC
    int prev_rec_source = global_settings.rec_source; /* detect source change */
#endif

    struct audio_recording_options rec_options;
    rec_status = RCSTAT_IN_RECSCREEN;
    push_current_activity(ACTIVITY_RECORDING);

#if (CONFIG_STORAGE & STORAGE_ATA) && (CONFIG_LED == LED_REAL) \
   && !defined(SIMULATOR)
    ata_set_led_enabled(false);
#endif

    /* hardware samplerate gets messed up so prevent mixer playing */
    int keyclick = global_settings.keyclick;
    global_settings.keyclick = 0;

    /* recording_menu gets messed up: so prevent manus talking */
    talk_disable(true);
    /* audio_init_recording stops anything playing when it takes the audio
       buffer */

#ifdef HAVE_AGC
    peak_meter_get_peakhold(&peak_l, &peak_r);
#endif

    pm_reset_clipcount();
    pm_activate_clipcount(false);
    settings_apply_trigger();

#ifdef HAVE_AGC
    agc_preset_str[0] = str(LANG_OFF);
    agc_preset_str[1] = str(LANG_AGC_SAFETY);
    agc_preset_str[2] = str(LANG_AGC_LIVE);
    agc_preset_str[3] = str(LANG_AGC_DJSET);
    agc_preset_str[4] = str(LANG_AGC_MEDIUM);
    agc_preset_str[5] = str(LANG_AGC_VOICE);
#endif /* HAVE_AGC */

#ifdef HAVE_SPEAKER
    /* Disable speaker to prevent feedback */
    audio_enable_speaker(0);
#endif

    audio_init_recording();
    sound_set_volume(global_status.volume);

#if CONFIG_RTC == 0
    /* Create new filename for recording start */
    rec_init_filename();
#endif

    /* start of the loop: we stay in this loop until user quits recscreen */
    while(done <= 0)
    {
        if(done < 0)
        {
            /* request to re-init stuff, done after settings screen */
            done = 0;
#ifdef HAVE_FMRADIO_REC
            /* If input changes away from FM Radio,
               radio will remain off when recording screen closes. */
            if (global_settings.rec_source != prev_rec_source
                && prev_rec_source == AUDIO_SRC_FMRADIO)
                radio_status = FMRADIO_OFF;
            prev_rec_source = global_settings.rec_source;
#endif

            /* viewport init and calculations that only needs to be done once */
            FOR_NB_SCREENS(i)
            {
                struct viewport *v;
                /* top vp, 4 lines, force sys font if total screen < 6 lines
                NOTE: one could limit the list to 1 line and get away with 5 lines */
                top_height_req[i] = 4;
#if defined(HAVE_HISTOGRAM)
                if((global_settings.histogram_interval) && (!i))
                    top_height_req[i] += 1; /* use one line for histogram */
#endif
                v = &vp_top[i];
                viewport_set_defaults(v, i);
                if (viewport_get_nb_lines(v) < top_height_req[i])
                {
                    /* compact needs 4 lines total */
                    v->font = FONT_SYSFIXED;
                    compact_view[i] = false;
                }
                else
                {
                    /*top=4,list=2*/
                    if (viewport_get_nb_lines(v) < (top_height_req[i]+2))
                        compact_view[i] = true;
                    else
                        compact_view[i] = false;
                }
                vp_list[i] = *v; /* get a copy now so it can be sized more easily */
                v->height = (font_get(v->font)->height)*(compact_view[i] ? 3 :
                                                                    top_height_req[i]);

                /* list section, rest of the screen */
                vp_list[i].y += vp_top[i].height;
                vp_list[i].height -= vp_top[i].height;
                screens[i].set_viewport(&vp_top[i]); /* req for next calls */

                screens[i].getstringsize("W", &w, &h);
                pm_y[i] = font_get(vp_top[i].font)->height * 2;
                trig_ypos[i] = font_get(vp_top[i].font)->height * 3;
                if(compact_view[i])
                    trig_ypos[i] -= (font_get(vp_top[i].font)->height)/2;
            }

            /* init the bottom list */
            gui_synclist_init(&lists, reclist_get_name, NULL, false, 1, vp_list);
            gui_synclist_set_title(&lists, NULL, Icon_NOICON);

            send_event(GUI_EVENT_ACTIONUPDATE, (void*)1); /* force a redraw */

#if defined(HAVE_HISTOGRAM)
            hist_pos_y = (compact_view[0] ? 3 : 4) * (font_get(vp_top[0].font)->height)
                                                                                    + 1;
            hist_size_h = font_get(vp_top[0].font)->height - 2;
            histogram_init();
#endif

            FOR_NB_SCREENS(i)
            {
                pm_x[i] = 0;
                if(global_settings.peak_meter_clipcounter)
                {
                    int clipwidth = 0;
                    screens[i].getstringsize(str(LANG_PM_CLIPCOUNT),
                                             &clipwidth, &h); /* h is same */
                    pm_x[i] = clipwidth+1;
                }
                if(global_settings.rec_trigger_mode == TRIG_MODE_OFF)
                    pm_h[i] = font_get(vp_top[i].font)->height * 2;
                else
                    pm_h[i] = font_get(vp_top[i].font)->height;
                if(compact_view[i])
                    pm_h[i] /= 2;
                trig_width[i] = vp_top[i].width - pm_x[i];
            }

            rec_init_recording_options(&rec_options);
            rec_set_recording_options(&rec_options);

            if(rec_create_directory() < 0)
            {
                rec_status = 0;
                goto rec_abort;
            }

#if CONFIG_RTC == 0
            /* If format changed, a new number is required */
            rec_init_filename();
#endif

#ifdef HAVE_AGC
#ifdef HAVE_MIC_REC
            if (global_settings.rec_source == AUDIO_SRC_MIC) {
                agc_preset = global_settings.rec_agc_preset_mic;
                agc_maxgain = global_settings.rec_agc_maxgain_mic;
            }
            else
#endif /* MIC */
            {
                agc_preset = global_settings.rec_agc_preset_line;
                agc_maxgain = global_settings.rec_agc_maxgain_line;
            }
#endif /* HAVE_AGC */

            set_gain();
            update_countdown = 0; /* Update immediately */

            int listi = 0;

            /* populate translation table for list id -> enum */
#ifdef HAVE_SPDIF_REC
            if(global_settings.rec_source == AUDIO_SRC_SPDIF)
            {
#ifndef HAVE_RECORDING_WITHOUT_MONITORING
                listid_to_enum[listi++] = ITEM_VOLUME;
#endif
                listid_to_enum[listi++] = ITEM_SAMPLERATE;
                listid_to_enum[listi++] = ITEM_FILENAME;

                gui_synclist_set_nb_items(&lists, listi);   /* spdif */
            }
            else
#endif
            {
#ifndef HAVE_RECORDING_WITHOUT_MONITORING
                listid_to_enum[listi++] = ITEM_VOLUME;
#endif
                listid_to_enum[listi++] = ITEM_GAIN;
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
                if(HAVE_MIC_REC_((global_settings.rec_source != AUDIO_SRC_MIC) || )
                   (global_settings.rec_channels != 1)) {
                    listid_to_enum[listi++] = ITEM_GAIN_L;
                    listid_to_enum[listi++] = ITEM_GAIN_R;
                }
#endif
#ifdef HAVE_AGC
                listid_to_enum[listi++] = ITEM_AGC_MODE;
                listid_to_enum[listi++] = ITEM_AGC_MAXDB;
#endif
                listid_to_enum[listi++] = ITEM_FILENAME;

                gui_synclist_set_nb_items(&lists, listi);   /* stereo */
            }

            gui_synclist_draw(&lists);
        } /* if(done < 0) */

        audio_stat = audio_status();

#if (CONFIG_LED == LED_REAL)

        /*
         * Flash the LED while waiting to record.  Turn it on while
         * recording.
         */
        if(audio_stat & AUDIO_STATUS_RECORD)
        {
            if (audio_stat & AUDIO_STATUS_PAUSE)
            {
                if (--led_countdown <= 0)
                {
                    led_state = !led_state;
                    led(led_state);
                    led_countdown = 2;
                }
            }
            else
            {
                /* trigger is on in status TRIG_READY (no check needed) */
                led(true);
            }
        }
        else
        {
            int trig_stat = peak_meter_trigger_status();
            /*
             * other trigger stati than trig_off and trig_steady
             * already imply that we are recording.
             */
            if (trig_stat == TRIG_STEADY)
            {
                if (--led_countdown <= 0)
                {
                    led_state = !led_state;
                    led(led_state);
                    led_countdown = 2;
                }
            }
            else
            {
                /* trigger is on in status TRIG_READY (no check needed) */
                led(false);
            }
        }
#endif /* CONFIG_LED */

        /* Wait for a button a while (HZ/10) drawing the peak meter */
        button = peak_meter_draw_get_btn(CONTEXT_RECSCREEN,
                                         pm_x, pm_y, pm_h,
                                         screen_update, vp_top);
        if (last_audio_stat != audio_stat)
        {
            if (audio_stat & AUDIO_STATUS_RECORD)
            {
                rec_status |= RCSTAT_HAVE_RECORDED;
            }
            last_audio_stat = audio_stat;
            update_list = true;
        }

        if (recording_start_automatic)
        {
            /* simulate a button press */
            button = ACTION_REC_PAUSE;
            recording_start_automatic = false;
        }

        /* let list handle the button */
        gui_synclist_do_button(&lists, &button);


        switch(button)
        {
            case ACTION_SETTINGS_INC:
            case ACTION_SETTINGS_INCREPEAT:
                switch (listid_to_enum[gui_synclist_get_sel_pos(&lists)])
                {
#ifndef HAVE_RECORDING_WITHOUT_MONITORING
                    case ITEM_VOLUME:
                        recording_step_levels(SOUND_VOLUME, 1);
                        break;
#endif
                    case ITEM_GAIN:
                        switch(global_settings.rec_source) {
#ifdef HAVE_MIC_REC
                        case AUDIO_SRC_MIC:
                            recording_step_levels(SOUND_MIC_GAIN, 1);
                            break;
#endif /* MIC */
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
                        HAVE_LINE_REC_(case AUDIO_SRC_LINEIN:)
                        HAVE_FMRADIO_REC_(case AUDIO_SRC_FMRADIO:)
                            recording_step_levels(SOUND_LEFT_GAIN, 1);
                            recording_step_levels(SOUND_RIGHT_GAIN, 1);
                            break;
#endif
                        }
                        break;
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
                    case ITEM_GAIN_L:
                        recording_step_levels(SOUND_LEFT_GAIN, 1);
                        break;
                    case ITEM_GAIN_R:
                        recording_step_levels(SOUND_RIGHT_GAIN, 1);
                        break;
#endif
#ifdef HAVE_AGC
                    case ITEM_AGC_MODE:
                        agc_preset = MIN(agc_preset + 1, AGC_MODE_SIZE);
                        agc_enable = (agc_preset != 0);
#ifdef HAVE_MIC_REC
                        if (global_settings.rec_source == AUDIO_SRC_MIC) {
                            global_settings.rec_agc_preset_mic = agc_preset;
                            agc_maxgain = global_settings.rec_agc_maxgain_mic;
                        } else
#endif /* MIC */
                        {
                            global_settings.rec_agc_preset_line = agc_preset;
                            agc_maxgain = global_settings.rec_agc_maxgain_line;
                        }
                        break;
                    case ITEM_AGC_MAXDB:
                        switch(global_settings.rec_source) {
#if defined(HAVE_LINE_IN) || defined(HAVE_FMRADIO_IN)
                        HAVE_LINE_IN_(case AUDIO_SRC_LINEIN:)
                        HAVE_FMRADIO_IN_(case AUDIO_SRC_FMRADIO:)
                            agc_maxgain = MIN(agc_maxgain + 1,
                                              sound_max(SOUND_LEFT_GAIN));
                            global_settings.rec_agc_maxgain_line = agc_maxgain;
                            break;
#endif
#ifdef HAVE_MIC_REC
                        case AUDIO_SRC_MIC:
                            agc_maxgain = MIN(agc_maxgain + 1,
                                              sound_max(SOUND_MIC_GAIN));
                            global_settings.rec_agc_maxgain_mic = agc_maxgain;
                            break;
#endif /* MIC */
                        }
                        break;
#endif /* HAVE_AGC */
                }
                set_gain();
                update_countdown = 0; /* Update immediately */
                break;
            case ACTION_SETTINGS_DEC:
            case ACTION_SETTINGS_DECREPEAT:
                switch (listid_to_enum[gui_synclist_get_sel_pos(&lists)])
                {
#ifndef HAVE_RECORDING_WITHOUT_MONITORING
                    case ITEM_VOLUME:
                        recording_step_levels(SOUND_VOLUME, -1);
                        break;
#endif
                    case ITEM_GAIN:
                        switch(global_settings.rec_source) {
#ifdef HAVE_MIC_REC
                        case AUDIO_SRC_MIC:
                            recording_step_levels(SOUND_MIC_GAIN, -1);
                            break;
#endif /* MIC */
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
                        HAVE_LINE_REC_(case AUDIO_SRC_LINEIN:)
                        HAVE_FMRADIO_REC_(case AUDIO_SRC_FMRADIO:)
                            recording_step_levels(SOUND_LEFT_GAIN, -1);
                            recording_step_levels(SOUND_RIGHT_GAIN, -1);
                            break;
#endif
                        }
                        break;
#if defined(HAVE_LINE_REC) || defined(HAVE_FMRADIO_REC)
                    case ITEM_GAIN_L:
                        recording_step_levels(SOUND_LEFT_GAIN, -1);
                        break;
                    case ITEM_GAIN_R:
                        recording_step_levels(SOUND_RIGHT_GAIN, -1);
                        break;
#endif
#ifdef HAVE_AGC
                    case ITEM_AGC_MODE:
                        agc_preset = MAX(agc_preset - 1, 0);
                        agc_enable = (agc_preset != 0);
#ifdef HAVE_MIC_REC
                        if (global_settings.rec_source == AUDIO_SRC_MIC) {
                            global_settings.rec_agc_preset_mic = agc_preset;
                            agc_maxgain = global_settings.rec_agc_maxgain_mic;
                        } else
#endif /* MIC */
                        {
                            global_settings.rec_agc_preset_line = agc_preset;
                            agc_maxgain = global_settings.rec_agc_maxgain_line;
                        }
                        break;
                    case ITEM_AGC_MAXDB:
                        switch(global_settings.rec_source) {
#if defined(HAVE_LINE_IN) || defined(HAVE_FMRADIO_IN)
                        HAVE_LINE_IN_(case AUDIO_SRC_LINEIN:)
                        HAVE_FMRADIO_IN_(case AUDIO_SRC_FMRADIO:)
                            agc_maxgain = MAX(agc_maxgain - 1,
                                              sound_min(SOUND_LEFT_GAIN));
                            global_settings.rec_agc_maxgain_line = agc_maxgain;
                            break;
#endif
#ifdef HAVE_MIC_REC
                        case AUDIO_SRC_MIC:
                            agc_maxgain = MAX(agc_maxgain - 1,
                                              sound_min(SOUND_MIC_GAIN));
                            global_settings.rec_agc_maxgain_mic = agc_maxgain;
                            break;
#endif /* MIC */
                        }
                        break;
#endif /* HAVE_AGC */
                }
                set_gain();
                update_countdown = 0; /* Update immediately */
                break;
            case ACTION_STD_CANCEL:
                /* turn off the trigger */
                peak_meter_trigger(false);
                peak_meter_set_trigger_listener(NULL);

                if(audio_stat & AUDIO_STATUS_RECORD)
                {
                    rec_command(RECORDING_CMD_STOP);
                }
                else
                {
                    done = 1;
                }
                update_countdown = 0; /* Update immediately */
                break;
#ifdef HAVE_REMOTE_LCD
            case ACTION_REC_LCD:
                /* this feature exists for some h1x0/h3x0 targets that suffer
                   from noise caused by remote LCD updates
                   NOTE 1: this will leave the list on the remote
                   NOTE 2: to be replaced by a global LCD_off() routine */
                if(remote_display_on)
                {
                    /* switch to single screen, leave message on remote */
                    screen_update = 1;
                    screens[1].clear_viewport();
                    screens[1].puts(0, 0, str(LANG_REMOTE_LCD_OFF));
                    screens[1].puts(0, 1, str(LANG_REMOTE_LCD_ON));
                    screens[1].update_viewport();
                }
                else
                {
                    /* remote switched on again */
                    update_list = true;
                    screen_update = NB_SCREENS;
                }
                remote_display_on = !remote_display_on; /* toggle */
                update_countdown = 0; /* Update immediately */
                break;
#endif
            case ACTION_REC_PAUSE:
            case ACTION_REC_NEWFILE:
                /* Only act if the mpeg is stopped */
                if(!(audio_stat & AUDIO_STATUS_RECORD))
                {
                    /* is this manual or triggered recording? */
                    if ((global_settings.rec_trigger_mode == TRIG_MODE_OFF) ||
                         (peak_meter_trigger_status() != TRIG_OFF))
                    {
                        /* manual recording */
                        rec_status |= RCSTAT_HAVE_RECORDED;
                        rec_command(RECORDING_CMD_START);
                        last_seconds = 0;
                        if (global_settings.talk_menu)
                        {
                            /* no voice possible here, but a beep */
                            audio_beep(HZ/2); /* longer beep on start */
                        }
                    }
                    /* this is triggered recording */
                    else
                    {
                        /* we don't start recording now, but enable the
                        trigger and let the callback function
                        trigger_listener control when the recording starts */
                        peak_meter_trigger(true);
                        peak_meter_set_trigger_listener(&trigger_listener);
                    }
                }
                else
                {
                    /*if new file button pressed, start new file */
                    if (button == ACTION_REC_NEWFILE)
                    {
                        rec_command(RECORDING_CMD_START_NEWFILE);
                        last_seconds = 0;
                    }
                    else
                        /* if pause button pressed, pause or resume */
                    {
                        if(audio_stat & AUDIO_STATUS_PAUSE)
                        {
                            rec_command(RECORDING_CMD_RESUME);
                            if (global_settings.talk_menu)
                            {
                                /* no voice possible here, but a beep */
                                audio_beep(HZ/4); /* short beep on resume */
                            }
                        }
                        else
                        {
                            rec_command(RECORDING_CMD_PAUSE);
                        }
                    }
                }
                update_countdown = 0; /* Update immediately */
                break;
            case ACTION_STD_MENU:
                if(!(audio_stat & AUDIO_STATUS_RECORD))
                {
#if (CONFIG_LED == LED_REAL)
                    /* led is restored at begin of loop / end of function */
                    led(false);
#endif
                    if (recording_menu(no_source))
                    {
                        done = 1;
                        rec_status |= RCSTAT_BEEN_IN_USB_MODE;
#ifdef HAVE_FMRADIO_REC
                        radio_status = FMRADIO_OFF;
#endif
                    }
                    else
                    {
                        done = -1;
                        /* the init is now done at the beginning of the loop */
                    }
                }
                break;

            case SYS_POWEROFF:
            case SYS_REBOOT:
                default_event_handler(button);
                done = true;
                break;

            case SYS_USB_CONNECTED:
                /* Only accept USB connection when not recording */
                if(!(audio_stat & AUDIO_STATUS_RECORD))
                {
                    FOR_NB_SCREENS(i)
                        screens[i].set_viewport(NULL);
                    default_event_handler(SYS_USB_CONNECTED);
                    done = true;
                    rec_status |= RCSTAT_BEEN_IN_USB_MODE;
#ifdef HAVE_FMRADIO_REC
                    radio_status = FMRADIO_OFF;
#endif
                }
                break;
        } /*switch(button)*/

#ifdef HAVE_AGC
        peak_read = !peak_read;
        if (peak_read) { /* every 2nd run of loop */
            peak_time++;
            peak_valid = read_peak_levels(&peak_l, &peak_r, &balance);
        }

        /* Handle AGC every 200ms when enabled and peak data is valid */
        if (peak_read && agc_enable && peak_valid)
            auto_gain_control(&peak_l, &peak_r, &balance);
#endif

        seconds = audio_recorded_time() / HZ;

        /* start of vp_top drawing */
        if(update_countdown-- == 0 || seconds > last_seconds)
        {
            unsigned int dseconds, dhours, dminutes;
            unsigned long num_recorded_bytes, dsize, dmb;

            FOR_NB_SCREENS(i)
            {
                screens[i].set_viewport(&vp_top[i]);
                screens[i].clear_viewport();
            }
            update_countdown = 5;
            last_seconds = seconds;

            dseconds = split_seconds;
            dsize = split_bytes;
            num_recorded_bytes = audio_num_recorded_bytes();

            if ((audio_stat & AUDIO_STATUS_WARNING)
                && (warning_counter++ % WARNING_PERIOD) < WARNING_PERIOD/2)
            {
                /* Switch back and forth displaying warning on first available
                   line to ensure visibility - the motion should also help
                   draw attention */
                /* Don't use language string unless agreed upon to make this
                   method permanent - could do something in the statusbar */
                snprintf(buf, sizeof(buf), "Warning: %08lX",
                         (unsigned long)pcm_rec_get_warnings());
            }
            else
            if ((global_settings.rec_sizesplit) &&
                (global_settings.rec_split_method))
            {
                dmb = dsize >> 20;
                snprintf(buf, sizeof(buf), "%s %luMB",
                         str(LANG_SPLIT_SIZE), dmb);
            }
            else
            {
                hours = seconds / 3600;
                minutes = (seconds - (hours * 3600)) / 60;
                snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d",
                         str(LANG_RECORDING_TIME),
                             hours, minutes, seconds%60);
            }

            FOR_NB_ACTIVE_SCREENS(i)
                screens[i].puts(0, 0, buf);

            if(audio_stat & AUDIO_STATUS_PRERECORD)
            {
                /* Tracks amount of prerecorded data in buffer */
                snprintf(buf, sizeof(buf), "%s (%lu/%ds)...",
                         str(LANG_RECORD_PRERECORD),
                         audio_prerecorded_time() / HZ,
                         global_settings.rec_prerecord_time);
            }
            else
            {
                /* Display the split interval if the record timesplit
                   is active */
                if ((global_settings.rec_timesplit) &&
                     !(global_settings.rec_split_method))
                {
                    /* Display the record timesplit interval rather
                       than the file size if the record timer is active */
                    dhours = dseconds / 3600;
                    dminutes = (dseconds - (dhours * 3600)) / 60;
                    snprintf(buf, sizeof(buf), "%s %02d:%02d",
                             str(LANG_RECORDING_TIMESPLIT_REC),
                                 dhours, dminutes);
                }
                else
                {
                    output_dyn_value(buf2, sizeof buf2,
                                     num_recorded_bytes,
                                     byte_units, 4, true);
                    snprintf(buf, sizeof(buf), "%s %s",
                             str(LANG_RECORDING_SIZE), buf2);
                }
            }

            FOR_NB_ACTIVE_SCREENS(i)
                screens[i].puts(0, 1, buf);

            /* We will do file splitting regardless, either at the end of
            a split interval, or when the filesize approaches the 2GB
            FAT file size (compatibility) limit. */
            if ((audio_stat && !(global_settings.rec_split_method)
                 && global_settings.rec_timesplit && (seconds >= dseconds))
                 || (audio_stat && global_settings.rec_split_method
                 && global_settings.rec_sizesplit
                 && (num_recorded_bytes >= dsize))
                 || (num_recorded_bytes >= MAX_FILE_SIZE))
            {
                if (!(global_settings.rec_split_type)
                      || (num_recorded_bytes >= MAX_FILE_SIZE))
                {
                    rec_command(RECORDING_CMD_START_NEWFILE);
                    last_seconds = 0;
                }
                else
                {
                    peak_meter_trigger(false);
                    peak_meter_set_trigger_listener(NULL);
                    if( global_settings.rec_split_type == 1)
                        rec_command(RECORDING_CMD_STOP);
                    else
                        rec_command(RECORDING_CMD_STOP_SHUTDOWN);
                }
                update_countdown = 0;
            }

            /* draw the clipcounter just in front of the peakmeter */
            if(global_settings.peak_meter_clipcounter)
            {
                int clipcount = pm_get_clipcount();
                FOR_NB_ACTIVE_SCREENS(i)
                {
                    if(!compact_view[i])
                    {
                        screens[i].puts(0, 2, str(LANG_PM_CLIPCOUNT));
                        screens[i].putsf(0, 3, "%4d", clipcount);
                    }
                    else
                        screens[i].putsf(0, 2, "%4d", clipcount);
                }
            }

#ifdef HAVE_HISTOGRAM
            if(global_settings.histogram_interval)
            {
                histogram_draw(0,
                               screens[0].getwidth()/2,
                               hist_pos_y,
                               hist_pos_y,
                               screens[0].getwidth()/2,
                               hist_size_h);
            }
#endif

#ifdef HAVE_AGC
            hist_time++;
#endif
            /* draw the trigger status */
            if (peak_meter_trigger_status() != TRIG_OFF)
            {
                peak_meter_draw_trig(pm_x, trig_ypos, trig_width,
                                    screen_update);
                FOR_NB_ACTIVE_SCREENS(i)
                    screens[i].update_viewport_rect(pm_x[i], trig_ypos[i],
                                                trig_width[i] + 2, TRIG_HEIGHT);
            }

#ifdef HAVE_AGC
#ifdef HAVE_MIC_REC
            if (global_settings.rec_source == AUDIO_SRC_MIC)
            {
                if(agc_maxgain < (global_settings.rec_mic_gain))
                    change_recording_gain(false, true, true);
            }
            else
#endif /* MIC */
            {
                if(agc_maxgain < (global_settings.rec_left_gain))
                    change_recording_gain(false, true, false);
                if(agc_maxgain < (global_settings.rec_right_gain))
                    change_recording_gain(false, false, true);
            }
#endif /* HAVE_AGC */

#ifdef HAVE_SPDIF_REC
            if((global_settings.rec_source == AUDIO_SRC_SPDIF) &&
               (prev_sample_rate != pcm_rec_sample_rate()))
            {
                /* spdif samplerate changed */
                prev_sample_rate = pcm_rec_sample_rate();
                update_list = true;
            }
#endif

            if(update_list)
            {
                /* update_list is set whenever content changes */
                update_list = false;
                gui_synclist_draw(&lists);
            }

            /* draw peakmeter again (check if this can be removed) */
            FOR_NB_ACTIVE_SCREENS(i)
            {
                screens[i].set_viewport(&vp_top[i]);
                peak_meter_screen(&screens[i], pm_x[i], pm_y[i], pm_h[i]);
                screens[i].update();
            }
        } /* display update every second */

        if(audio_stat & AUDIO_STATUS_ERROR)
        {
            done = true;
        }
    } /* end while(!done) */

    audio_stat = audio_status();
    if (audio_stat & AUDIO_STATUS_ERROR)
    {
        splash(0, str(LANG_DISK_FULL));

        FOR_NB_SCREENS(i)
            screens[i].update();

        /* stop recording first and try to finish saving whatever it can */
        rec_command(RECORDING_CMD_STOP);
        audio_close_recording();

        audio_error_clear();

        while(1)
        {
             if (action_userabort(TIMEOUT_NOBLOCK))
                break;
        }
    }

rec_abort:

    rec_command(RECORDING_CMD_STOP);
    audio_close_recording();

#ifdef HAVE_FMRADIO_REC
    if (radio_status != FMRADIO_OFF)
        /* Restore radio playback - radio_status should be unchanged if started
           through fm radio screen (barring usb connect) */
        rec_set_source(AUDIO_SRC_FMRADIO, (radio_status & FMRADIO_PAUSED) ?
                            SRCF_FMRADIO_PAUSED : SRCF_FMRADIO_PLAYING);
    else
#endif
        /* Go back to playback mode */
        rec_set_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);

    /* restore talking */
    talk_disable(false);

    /* restore keyclick */
    global_settings.keyclick = keyclick;

#ifdef HAVE_SPEAKER
    /* Re-enable speaker */
    audio_enable_speaker(global_settings.speaker_mode);
#endif

    /* make sure the trigger is really turned off */
    peak_meter_trigger(false);
    peak_meter_set_trigger_listener(NULL);

    rec_status &= ~RCSTAT_IN_RECSCREEN;
    sound_settings_apply();

    FOR_NB_SCREENS(i) {
        screens[i].set_viewport(NULL);
        screens[i].setfont(FONT_UI);
    }

    /* if the directory was created or recording happened, make sure the
       browser is updated */
    if (rec_status & (RCSTAT_CREATED_DIRECTORY | RCSTAT_HAVE_RECORDED))
        reload_directory();

#if (CONFIG_STORAGE & STORAGE_ATA) && (CONFIG_LED == LED_REAL) \
   && !defined(SIMULATOR)
    ata_set_led_enabled(true);
#endif

    settings_save();
    pop_current_activity();
    return (rec_status & RCSTAT_BEEN_IN_USB_MODE) != 0;
} /* recording_screen */

void audio_beep(int duration)
{
    /* dummy */
    (void)duration;
}

#endif /* HAVE_RECORDING */
