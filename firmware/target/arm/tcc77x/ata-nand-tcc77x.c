/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2007 Dave Chapman
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
#include "ata.h"
#include "ata-target.h"
#include "ata_idle_notify.h"
#include "system.h"
#include <string.h>
#include "thread.h"
#include "led.h"
#include "disk.h"
#include "panic.h"
#include "usb.h"

/* for compatibility */
int ata_spinup_time = 0;

long last_disk_activity = -1;

/* Used to store (fake?) identify info */
static unsigned short identify_info[256];

/** static, private data **/ 
static bool initialized = false;

static long next_yield = 0;
#define MIN_YIELD_PERIOD 2000

/* API Functions */

void ata_led(bool onoff)
{
    led(onoff);
}

int ata_read_sectors(IF_MV2(int drive,) unsigned long start, int incount,
                     void* inbuf)
{

}

int ata_write_sectors(IF_MV2(int drive,) unsigned long start, int count,
                      const void* outbuf)
{
}

void ata_spindown(int seconds)
{
    (void)seconds;
}

bool ata_disk_is_active(void)
{
    return 0;
}

void ata_sleep(void)
{
}

void ata_spin(void)
{
}

/* Hardware reset protocol as specified in chapter 9.1, ATA spec draft v5 */
int ata_hard_reset(void)
{
    return 0;
}

int ata_soft_reset(void)
{
    return 0;
}

void ata_enable(bool on)
{
}

int ata_init(void)
{
     return 0;
}

/* TEMP: This will return junk, it's here for compilation only */
unsigned short* ata_get_identify(void)
{
    return identify_info;
}
