/*
 * Copyright (C) 2003-2010 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

//
// mpd.conf
//
// realtime_option {
//     main_priority        "POLICY:PRIORITY"
//     io_priority          "POLICY:PRIORITY"
//     decorder_priority    "POLICY:PRIORITY"
//     player_priority      "POLICY:PRIORITY"
//     update_priority      "POLICY:PRIORITY"
//
//     memlock              "yes" or "no"
//     stackreserve	       "1024"
//     heapreserve	       "10240"
//
//   }
//
//  POLICY  "OTHER" | "FIFO" | "RR" | "BATCH" | "IDLE"
//  PRIORITY
//            OTHER,BATCH,IDLE   0
//            FIFO, RR           1 - 99
//
//   audio_output {
//       ....
//       ....
//     priority              "POLICY:PRIORITY"
//     timerslack            unsigned long(default value = 100)
//   }
//

#ifndef RT_OPT_H_
#define RT_OPT_H_

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sched.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef ENABLE_RTOPT
#define RTOPT_MAIN_PRIORITY_NAME	((const char *)"main_priority")
#define RTOPT_IO_PRIORITY_NAME	        ((const char *)"io_priority")
#define RTOPT_DECODER_PRIORITY_NAME	((const char *)"decoder_priority")
#define RTOPT_PLAYER_PRIORITY_NAME	((const char *)"player_priority")
#define RTOPT_UPDATE_PRIORITY_NAME      ((const char *)"update_priority")
#define RTOPT_MEMLOCK_NAME		((const char *)"memlock")
#define RTOPT_STACKRESERVE_NAME         ((const char *)"stack_reserve")
#define RTOPT_HEAPRESERVE_NAME          ((const char *)"heap_reserve")


#define RTOPT_MAIL_PRIORITY    0
#define RTOPT_DECODER_PRIORITY 1
#define RTOPT_PLAYER_PRIORITY  2

#define RTOPT_DEFAULT_STACK_RESERVE ((size_t)0)
#define RTOPT_DEFAULT_HEAP_RESERVE  ((size_t)0)


#define RTOPT_SCHED_OTHER  "OTHER"
#define RTOPT_SCHED_FIFO   "FIFO"
#define RTOPT_SCHED_RR     "RR"
#define RTOPT_SCHED_BATCH  "BATCH"
#define RTOPT_SCHED_IDLE   "IDLE"

#define RTOPT_DISABLE (-1)

struct rtopt_priority {
	const char	*name;
	int		policy;
	int		priority;
        unsigned long   timerslack;
};

/*
static inline GQuark
rtopt_quark(void)
{
	return g_quark_from_static_string("rt_opt");
}
*/

void rtopt_init(void);
void rtopt_memlock(void);
int  rtopt_change_priority(const char *name);
int  rtopt_change_output_priority(const char *name);
int  rtopt_change_thread_priority(const struct rtopt_priority *new_priority);
void rtopt_change_output_timerslack(const char *name);

#endif /* ENABLE_RTOPT */

#endif /* RT_OPT_H_ */
