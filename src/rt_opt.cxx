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

#include "config.h"
#include "ConfigOption.hxx"

#include "system/FatalError.hxx"
#include "Log.hxx"
#include "util/Domain.hxx"


#include "ConfigData.hxx"
#include "ConfigGlobal.hxx"
#include "ConfigOption.hxx"

#include "rt_opt.hxx"

#include <glib.h>

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>


static constexpr Domain rt_opt_domain("rt_opt");

#define IS_ENABLE_RTOPT			enable_rtopt
#define IS_ENABLE_MEMLOCK		enable_memlock
#define IS_ENABLE_PRIORITY(p)	( (p) != RTOPT_DISABLE )

#define AUDIO_OUTPUT_PRIORITY      ((const char *)"priority")
#define AUDIO_OUTPUT_NAME          ((const char *)"name")
#define AUDIO_OUTPUT_TIMERSLACK    ((const char *)"timerslack")
#define DEFAULT_TIMERSLACK         ((const unsigned)100)

#define IS_EQUAL_PRIORITY(p1,p2) (((p1)->policy    == (p2)->policy) && \
		 				      ((p1)->priority == (p2)->priority))

#define MIN_PRIORITY				1
#define MAX_PRIORITY				99

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct policy_info {
	const char*  name;
	const int    policy;
};

static struct policy_info policy_tab[] = {
		{  RTOPT_SCHED_OTHER,  SCHED_OTHER },
		{  RTOPT_SCHED_FIFO,   SCHED_FIFO },
		{  RTOPT_SCHED_RR,    SCHED_RR },
		{  RTOPT_SCHED_BATCH,  SCHED_BATCH },
#ifdef SCHED_IDLE
		{  RTOPT_SCHED_IDLE,  SCHED_IDLE }
#endif
};

static const char* priority_keys[] = {
		RTOPT_MAIN_PRIORITY_NAME,
		RTOPT_IO_PRIORITY_NAME,
		RTOPT_PLAYER_PRIORITY_NAME,
		RTOPT_DECODER_PRIORITY_NAME,
		RTOPT_UPDATE_PRIORITY_NAME
};


static struct rtopt_priority priority_tab[ARRAY_SIZE(priority_keys)];

static struct rtopt_priority **output_priority_tab = NULL;
static unsigned output_count = 0;

static bool enable_rtopt = false;
static bool enable_memlock = false;
static size_t stack_reserve = RTOPT_DEFAULT_STACK_RESERVE;
static size_t heap_reserve  = RTOPT_DEFAULT_HEAP_RESERVE;


static void setUnlimited( const int target, const char *target_name);
static int get_policy(char *name);
static void init_priority_tab(void);
static unsigned audio_output_config_count(void);
static unsigned init_output_priority_tab(void);
static int strtointeger(char *str, int *ival);
static void parse_priority(const char *paramstr, struct rtopt_priority *priority);
static void set_parameter(void);
static const struct rtopt_priority *get_priority_param(const char *key);
static const struct rtopt_priority *get_output_priority_param(const char *key);
static void reset_limit(void);
static int get_current_priority(struct rtopt_priority *priority);
static int change_priority(const struct rtopt_priority *priority);

static void ioprio_set_idle();
static void SetThreadIdlePriority(const char *name);

#ifdef HAVE_PRCTL
#include <sys/prctl.h>
#endif

static inline void
SetThreadTimerSlackUS(unsigned long slack_us)
{
#if defined(HAVE_PRCTL) && defined(PR_SET_TIMERSLACK)
  prctl(PR_SET_TIMERSLACK, slack_us * 1000ul,0,0,0);
  FormatDebug(rt_opt_domain, "set timerslack %lu usec",slack_us);
#else
  FormatDebug(rt_opt_domain,"timerslack is not supported");
  (void)slack_us;
#endif
}

static void
setUnlimited( const int target, const char *target_name) {
	const struct rlimit unlimited = {
	  RLIM_INFINITY,
	  RLIM_INFINITY
	};
	const int res = setrlimit(target,&unlimited);
	if ( res < 0 ) {
	  FormatFatalError("setrlimit %s error %d(%s)\n",target_name,errno,strerror(errno));
	}
}

static int
get_policy(char *name) {
	for (unsigned i = 0; i < ARRAY_SIZE(policy_tab); i++ ) {
		if (strcmp(name,policy_tab[i].name) == 0) {
			return policy_tab[i].policy;
		}
	}
	return RTOPT_DISABLE;
}

static void
init_priority_tab(void) {
	for (unsigned i = 0; i < ARRAY_SIZE(priority_tab); i++) {
		priority_tab[i].name = priority_keys[i];
		priority_tab[i].policy = RTOPT_DISABLE;
		priority_tab[i].priority = 0;
/*
		priority_tab[i].policy = SCHED_OTHER;
		priority_tab[i].priority = 0;
*/
	}
}

/*  from output_all.c  */
static unsigned
audio_output_config_count(void)
{
	unsigned int nr = 0;
	const struct config_param *param = NULL;

	while ((param = config_get_next_param(CONF_AUDIO_OUTPUT, param)))
		nr++;
//	if (!nr)
//		nr = 1; /* we'll always have at least one device  */
	return nr;
}

static unsigned
init_output_priority_tab(void) {
	const struct config_param *param = NULL;
	const char *p = NULL;
	const char *name = NULL;
	struct rtopt_priority *pri = NULL;

	unsigned cnt = audio_output_config_count();
	if ( cnt == 0 ) {
		return 0;
	}

	output_priority_tab = (struct rtopt_priority **)malloc(sizeof(struct rtopt_priority *) * cnt);
	for ( unsigned i = 0; i < cnt; i++ ) {
		output_priority_tab[i] = NULL;
	}

	unsigned idx = 0;
	for ( unsigned i = 0; i < cnt; i++) {
		param = config_get_next_param(CONF_AUDIO_OUTPUT, param);
		assert(param);

		name = param->GetBlockValue(AUDIO_OUTPUT_NAME);
		if ( name != NULL ) {
			pri = (struct rtopt_priority *)malloc(sizeof( struct rtopt_priority ));
			pri->name = name;
			p = param->GetBlockValue(AUDIO_OUTPUT_PRIORITY);

			parse_priority(p, pri);
			pri->timerslack = param->GetBlockValue(AUDIO_OUTPUT_TIMERSLACK,DEFAULT_TIMERSLACK);
			FormatDebug(rt_opt_domain,
				    "realtime_option(init_output_priority_tab): output priority name %s policy %d  priority %d timerslack %lu\n",
				    pri->name,pri->policy,pri->priority,pri->timerslack);
			output_priority_tab[idx++] = pri;
		} else {
			FormatWarning(rt_opt_domain,
				      "realtime_option(init_output_priority_tab): Missing \"name\" configuration\n");
		}
	}
	return idx;
}


static int
strtointeger(char *str, int *ival) {
	char *endptr = NULL;

	*ival = strtol(str, &endptr, 10);
	return (*endptr == '\0') ? 0 : -1;
}

static void
parse_priority(const char *paramstr, struct rtopt_priority *priority) {
	char *policyname = NULL;
	char *pstr = NULL;
	int  priority_val;
	int  policy_val;

	priority->policy = RTOPT_DISABLE;
	priority->priority = 0;

	if ( paramstr == NULL ) {
		return;
	}
	priority->policy = SCHED_OTHER;

	policyname = (char *)alloca(strlen(paramstr) + 1);
	strcpy(policyname,paramstr);
	pstr = strchr(policyname,':');

	if ( pstr != NULL ) {
		*pstr++ = '\0';
	}

	if ( strcmp(policyname,RTOPT_SCHED_OTHER) == 0 ) {
		return;
	} else if ( (policy_val = get_policy(policyname)) < 0 ) {
		FormatWarning(rt_opt_domain,
			      "realtime_option(parse_priority): illegal policy name = '%s'   priority = '%s'\n",
			      priority->name,paramstr);
		return;
	}

	if ( pstr == NULL ) {
		FormatWarning(rt_opt_domain,
			      "realtime_option(parse_priority): undefined priority  name = '%s'   priority = '%s'\n",
			      priority->name,paramstr);
		return;
	}
	if ( strtointeger(pstr, &priority_val) != 0 ) {
		FormatWarning(rt_opt_domain,
			      "realtime_option(parse_priority): priority isn't number name = '%s'   priority = '%s'\n",
			      priority->name,paramstr);
		return;
	}

	if ( (priority_val < MIN_PRIORITY) ||
			(priority_val > MAX_PRIORITY) ) {
		FormatWarning(rt_opt_domain,
			      "realtime_option(parse_priority): illegal priority  name = '%s'   priority = '%s'\n",
			      priority->name,paramstr);
		return;
	}

	priority->policy = policy_val;
	priority->priority = priority_val;
}


static void
set_parameter(void) {
	const struct config_param *param = NULL;
	struct rtopt_priority *pri = NULL;
	const char *pstr;

	init_priority_tab();

	enable_rtopt = false;
	param = config_get_next_param(CONF_RTOPT,NULL);
	if ( param == NULL ) {
		return;
	}
	enable_rtopt = true;

	enable_memlock = param->GetBlockValue(RTOPT_MEMLOCK_NAME,false);

	/* Work around 'call of overloaded ‘GetBlockValue(const char*, size_t)’ is ambiguous' compiler errors */
#if 0
	stack_reserve = param->GetBlockValue(RTOPT_STACKRESERVE_NAME,RTOPT_DEFAULT_STACK_RESERVE) * 1024;

	heap_reserve  = param->GetBlockValue(RTOPT_HEAPRESERVE_NAME,RTOPT_DEFAULT_HEAP_RESERVE) * 1024;
#else
	stack_reserve = param->GetBlockValue(((const char*)"stack_reserve"), 0) * 1024;

	heap_reserve  = param->GetBlockValue(((const char*)"heap_reserve"), 0) * 1024;
#endif

	if ( enable_memlock ) {
	  FormatDebug(rt_opt_domain,
		      "realtime_option(set_parameter): memlock enable  stack_reserve : %zd   heap_reserve : %zd\n",
		      stack_reserve,heap_reserve);
	}

	for (unsigned i = 0; i < ARRAY_SIZE(priority_tab); i++ ) {
		pri = priority_tab + i;
		pstr = param->GetBlockValue(pri->name);
		parse_priority(pstr, pri);
		FormatDebug(rt_opt_domain,
			    "realtime_option(set_parameter): %s  policy %d  priority %d\n",
			    pri->name,pri->policy,pri->priority);
	}
	output_count = init_output_priority_tab();
}

static const struct rtopt_priority
*get_priority_param(const char *key) {
	for (unsigned i = 0; i < ARRAY_SIZE(priority_keys); i++) {
		if ( strcmp(key,priority_keys[i]) == 0 ) {
			return priority_tab + i;
		}
	}
	return NULL;
}

static const struct rtopt_priority
*get_output_priority_param(const char *key) {
	for ( unsigned i = 0; i < output_count; i++ ) {
		if ( output_priority_tab[i] == NULL ) {
			return NULL;
		}
		if ( strcmp(key,output_priority_tab[i]->name) == 0 ) {
			return output_priority_tab[i];
		}
	}
	return NULL;
}

static void
reset_limit() {
	setUnlimited(RLIMIT_MEMLOCK,"memlock");
	setUnlimited(RLIMIT_RTPRIO, "rtprio");
}

static int get_current_priority(struct rtopt_priority *priority) {
	struct sched_param param;
	int res;

	res = sched_getparam(0,&param);
	if ( res < 0 ) {
		FormatWarning(rt_opt_domain,
			      "realtime_option(get_current_priority): sched_getparm error errno = %s(%d)\n",
			      strerror(errno),errno);
		return -1;
	}

	res = sched_getscheduler(0);
	if ( res < 0 ) {
		FormatWarning(rt_opt_domain,
			      "realtime_option(get_current_priority): sched_getscheduler error errno = %s(%d)\n",
			      strerror(errno),errno);
		return -1;
	}
	priority->policy = res;
	priority->priority = param.sched_priority;
	return 0;
}

static int change_priority(const struct rtopt_priority *priority) {
	struct sched_param param = { priority->priority };

	int res = sched_setscheduler(0,priority->policy,&param);
	if ( res < 0 ) {
		FormatWarning(rt_opt_domain,
			      "realtime_option(change_priority): sched_setscheduler error errno = %s(%d)\n",
			      strerror(errno),errno);
	}
	FormatDebug(rt_opt_domain,
		    "realtime_option(change_priority): name %s  policy %d   priority %d\n",
		    priority->name,priority->policy,param.sched_priority);
	return res;
}


static int
ioprio_set(int which, int who, int ioprio)
{
        return syscall(__NR_ioprio_set, which, who, ioprio);
}

static void ioprio_set_idle()
{
        static constexpr int _IOPRIO_WHO_PROCESS = 1;
        static constexpr int _IOPRIO_CLASS_IDLE = 3;
        static constexpr int _IOPRIO_CLASS_SHIFT = 13;
        static constexpr int _IOPRIO_IDLE =
                (_IOPRIO_CLASS_IDLE << _IOPRIO_CLASS_SHIFT) | 7;

        ioprio_set(_IOPRIO_WHO_PROCESS, 0, _IOPRIO_IDLE);
}

static void SetThreadIdlePriority(const char *name)
{
#ifdef SCHED_IDLE
  struct rtopt_priority  param = {name, SCHED_IDLE,0,0l};
  change_priority(&param);
#endif

  ioprio_set_idle();
}

static unsigned long get_output_timerslack(const char *name) {
	const struct rtopt_priority *param = get_output_priority_param(name);
	if ( param == NULL ) {
		FormatDebug(rt_opt_domain,
			 "realtime_option(output_timerslack): name not found name = '%s'\n",name);
		return DEFAULT_TIMERSLACK;
	}
	FormatDebug(rt_opt_domain,
		 "realtime_option(output_timerslack): name %s   policy %d  timerslack %lu\n",
		 param->name,param->policy,param->timerslack);
	return param->timerslack;
}

void rtopt_init() {
	set_parameter();
	if ( !IS_ENABLE_RTOPT ) {
		return;
	}
	reset_limit();
}


void rtopt_memlock() {
	void *ptr = NULL;

	if ( !IS_ENABLE_RTOPT ) {
		FormatDebug(rt_opt_domain,
			    "realtime_option(rtopt_memlock): realtime_option disabled\n");
		return;
	}

	if ( stack_reserve != (size_t)0 ) {
		FormatDebug(rt_opt_domain,
			 "realtime_option(rtopt_memlock): stack_reserve %zd",stack_reserve);
		bzero(alloca(stack_reserve), stack_reserve);
	}

	if ( heap_reserve != (size_t)0 ) {
		FormatDebug(rt_opt_domain,
			 "realtime_option(rtopt_memlock): heap_reserve %zd",heap_reserve);
		ptr = malloc(heap_reserve);
		if ( ptr != NULL ) {
			bzero(ptr, heap_reserve);
			free(ptr);
		} else {
			FormatFatalError("realtime_option(rtopt_memlock): heap allocate error reserved size = %d\n",
					 heap_reserve);
		}
	}

	if ( !IS_ENABLE_MEMLOCK ) {
		FormatDebug(rt_opt_domain,
			 "realtime_option(rtopt_memlock): memlock disabled\n");
		return;
	}

	int stat = mlockall(MCL_CURRENT);
	if ( stat < 0 ) {
		FormatFatalError("realtime_option(rtopt_memlock): mlockall error errno = %d(%s)\n",
				 errno,strerror(errno));
	}
}

int rtopt_change_priority(const char *name) {
	const struct rtopt_priority *param = get_priority_param(name);
	if ( param == NULL ) {
		FormatDebug(rt_opt_domain,
			 "realtime_option(rtopt_change_priority): name not found name = '%s'\n",name);
		return -1;
	}
	if ( !IS_ENABLE_PRIORITY(param->policy) ) {
		if ( strcmp(param->name,RTOPT_UPDATE_PRIORITY_NAME) == 0 ) {
			SetThreadIdlePriority(param->name);
			FormatDebug(rt_opt_domain,
				    "realtime_option(rtopt_change_priority): name %s  SCHED_IDLE",
				    param->name);
		}
		return 1;
        }
	FormatDebug(rt_opt_domain,
		 "realtime_option(rtopt_change_priority): name %s   policy %d  priority %d\n",
		 param->name,param->policy,param->priority);
	return rtopt_change_thread_priority(param);
}

int rtopt_change_output_priority(const char *name) {
	const struct rtopt_priority *param = get_output_priority_param(name);
	if ( param == NULL ) {
		FormatDebug(rt_opt_domain,
			 "realtime_option(rtopt_change_output_priority): name not found name = '%s'\n",name);
		return -1;
	}
	FormatDebug(rt_opt_domain,
		 "realtime_option(rtopt_change_output_priority): name %s   policy %d  priority %d\n",
		 param->name,param->policy,param->priority);
	return rtopt_change_thread_priority(param);
}

int rtopt_change_thread_priority(const struct rtopt_priority *new_priority) {
	struct rtopt_priority save;

	if ( !IS_ENABLE_RTOPT ) {
		return 1;
	}
	if ( !IS_ENABLE_PRIORITY(new_priority->policy) ) {
		return 1;
	}

	if ( get_current_priority(&save) < 0 ) {
		return 1;
	}

	if ( IS_EQUAL_PRIORITY(new_priority, &save) ) {
		FormatDebug(rt_opt_domain,
			 "realtime_option(rtopt_change_thread_priority): name %s not changed",
			 new_priority->name);
		return 1;
	}

	return change_priority(new_priority);
}

void rtopt_change_output_timerslack(const char *name) {
  unsigned long t = get_output_timerslack(name);

  SetThreadTimerSlackUS(t);
  FormatDebug(rt_opt_domain,"output:%s  timerslack %lu", name,t);
}
