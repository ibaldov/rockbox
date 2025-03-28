/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright © 2008 Rafaël Carré
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
#include "powermgmt.h"
#include "pmu-target.h"
#include "power.h"
#include "audiohw.h"

unsigned short battery_level_disksafe = 3600;

unsigned short battery_level_shutoff = 3350;

/* voltages (millivolt) of 0%, 10%, ... 100% when charging disabled */
unsigned short percent_to_volt_discharge[11] =
{
    3550, 3783, 3830, 3882, 3911, 3949, 3996, 4067, 4148, 4228, 4310
};

#if CONFIG_CHARGING
/* voltages (millivolt) of 0%, 10%, ... 100% when charging enabled */
unsigned short percent_to_volt_charge[11] =
{
    3550, 3783, 3830, 3882, 3911, 3949, 3996, 4067, 4148, 4228, 4310
};
#endif /* CONFIG_CHARGING */

/* ADC should read 0x3ff=6.00V */
#define BATTERY_SCALE_FACTOR 6000
/* full-scale ADC readout (2^10) in millivolt */


/* Returns battery voltage from ADC [millivolts] */
int _battery_voltage(void)
{
    int compensation = (10 * (pmu_read_battery_current() - 7)) / 12;
    if (charging_state()) return pmu_read_battery_voltage() - compensation;
    return pmu_read_battery_voltage() + compensation;
}


#ifdef HAVE_ACCESSORY_SUPPLY
void accessory_supply_set(bool enable)
{
    if (enable)
    {
        /* Accessory voltage supply on */
        pmu_ldo_power_on(6);
    }
    else
    {
        /* Accessory voltage supply off */
        pmu_ldo_power_off(6);
    }
}
#endif

#ifdef HAVE_LINEOUT_POWEROFF
void lineout_set(bool enable)
{
    /* Call audio hardware driver implementation */
    audiohw_enable_lineout(enable);
}
#endif
