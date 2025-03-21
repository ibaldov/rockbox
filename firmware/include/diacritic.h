/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2009 Tomer Shalev
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
#ifndef _DIACRITIC_H_
#define _DIACRITIC_H_
#include "system.h"

#define FIRST_DIACRITIC (0x0300)
/* Tests whether a given charactor code is a diacritic mark.
 * Sets is_rtl (if it's not NULL) to whether the character
 * belongs to an RTL language.
*/
bool is_diacritic(const unsigned short char_code, bool *is_rtl);

/* Note IS_DIACRITIC macros may elide the function call
 * therefore there is a separate _RTL version that requires a bool pointer
 * as it sets the variable to false when the is_diacritic func is skipped
*/
#define IS_DIACRITIC(c) (c >= FIRST_DIACRITIC && is_diacritic(c, NULL))
/* sets the rtl variable to false when function call is skipped */
#define IS_DIACRITIC_RTL(c, rtl) ((c >= FIRST_DIACRITIC || ( (*rtl = false) )) && is_diacritic(c, rtl))

#endif
