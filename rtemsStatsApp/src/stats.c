#include <epicsStdio.h>
#include <registryFunction.h>
#include <epicsExport.h>
#include <iocsh.h>
#include <initHooks.h>
#include <epicsPrint.h>
#include <epicsThread.h>
#include <epicsInterrupt.h>
#include <epicsTime.h>
#include <aSubRecord.h>
#include <cantProceed.h>

#include <rtems.h>
#include <rtems/extension.h>
#include <capture/capture.h>
#include <cpuuse/cpuuse.h>

#include <stdlib.h>
#include <string.h>

static void rtems_stats_switching_context(rtems_tcb *, rtems_tcb *);
static void rtems_stats_task_begins(rtems_tcb *);
static void rtems_stats_task_exits(rtems_tcb *);
static int  rtems_stats_enabled(void);
static int  rtems_stats_enable(void);
static void rtems_stats_disable(void);
static void rtems_stats_snapshot(int);

#define MAX_EVENTS 4096

typedef enum {
	SWITCH,
	BEGIN,
	EXIT
} rtems_stats_event_type;

#define EVENT_GET_TYPE(ev)          ((rtems_stats_event_type)(ev->misc & 0xFF))
#define EVENT_GET_PRIO_CURRENT(ev)  ((rtems_stats_event_type)((ev->misc & 0xFF00) >> 8))
#define EVENT_GET_PRIO_REAL(ev)     ((rtems_stats_event_type)((ev->misc & 0xFF0000) >> 16))
#define EVENT_SET_MISC(t, c, r)     ((unsigned)(((r & 0xFF) << 16) + ((c & 0xFF) << 8) + (t & 0xFF)))

typedef struct {
	unsigned misc;
	States_Control state;
	rtems_id obj_id;
	rtems_id wait_id;
	struct timespec stamp;
} rtems_stats_event_with_timestamp;

typedef struct {
	unsigned misc;
	States_Control state;
	rtems_id obj_id;
	rtems_id wait_id;
	rtems_interval ticks;
} rtems_stats_event_with_ticks;

#if defined(WITH_INT_TIME)
 #define RTEMS_STATS_EVENT rtems_stats_event_with_timestamp
#else
 #define RTEMS_STATS_EVENT rtems_stats_event_with_ticks
#endif

#define MAX_TASKS 256
#define ARRAY_IDS_SIZE (MAX_TASKS / 32)

#define INCR_RB_POINTER(x) (x = (x + 1) % MAX_EVENTS)
#define SET_ACTIVE_TASK(prb, tid) { if (tid != 0x9010001u) prb->ids[(tid & 0xff) / 32] |= 1 << (tid % 32);  }
typedef struct {
	struct timespec stamp;
	unsigned ticks;
	unsigned num_events;
	unsigned head;
	uint32_t ids[ARRAY_IDS_SIZE];
	RTEMS_STATS_EVENT  thread_activations[MAX_EVENTS];
} rtems_stats_ring_buffer;

static rtems_stats_ring_buffer rb[2];
static rtems_stats_ring_buffer *rb_active = &rb[0];
static rtems_stats_ring_buffer *rb_export = &rb[1];

static void rtems_stats_reset_rb(rtems_stats_ring_buffer *);
static rtems_stats_ring_buffer *rtems_stats_switch_rb(void);

static rtems_extensions_table rtems_stats_extension_table = {
	.thread_switch  = rtems_stats_switching_context,
	.thread_begin   = rtems_stats_task_begins,
	.thread_exitted = rtems_stats_task_exits,
};



static int rtems_taking_snapshot = 0;
static int rtems_snapshot_count = 0;
static int rb_switch_trigger = 0;

static rtems_id rtems_stats_extension_table_id;
static rtems_name rtems_stats_table_name = rtems_build_name('R', 'T', 'S', 'T');
static rtems_id rtems_stats_sem;

int rtems_stats_enabled(void) {
	rtems_id id;

	return rtems_extension_ident(rtems_stats_table_name, &id);
}

int rtems_stats_enable(void) {
	rtems_status_code ret;
	char name[10], *res;

	if (rtems_stats_enabled() == RTEMS_SUCCESSFUL)
		return 0;

	// Created with count 0: used for synchronization
	if(rtems_semaphore_create(rtems_build_name('S', 'T', 'S', 'M'), 0,
			       RTEMS_SIMPLE_BINARY_SEMAPHORE, 0, &rtems_stats_sem) != RTEMS_SUCCESSFUL)
	{
		errlogMessage("Cannot create a semaphore for the stats module");
		return 1;
	}

	if ((ret = rtems_extension_create(rtems_stats_table_name,
					 &rtems_stats_extension_table,
					 &rtems_stats_extension_table_id)) != RTEMS_SUCCESSFUL)
	{
		rtems_semaphore_delete(rtems_stats_sem);
		switch (ret) {
			case RTEMS_TOO_MANY:
				errlogMessage("Too many extension sets. Can't enable rtemsStats");
				break;
			case RTEMS_INVALID_NAME:
				res = rtems_object_get_name(rtems_stats_extension_table_id, sizeof(name) - 1, name);
				if (res)
					errlogPrintf("Invalid name for the extensions table: %s\n", name);
				else
					errlogMessage("Invalid name for the extensions table");
				break;
			default:
				break;
		}
		return 1;
	}
	else {
		errlogMessage("rtemsStats enabled\n");
		return 0;
	}
}

void rtems_stats_disable(void) {
	if (rtems_extension_delete(rtems_stats_extension_table_id) == RTEMS_SUCCESSFUL) {
		rtems_semaphore_delete(rtems_stats_sem);
		rtems_stats_extension_table_id = 0;
		errlogMessage("rtemsStats disabled\n");
	}
	else {
		errlogMessage("rtemsStats doesn't seem to be enabled\n");
	}
}

static void rtems_stats_print_state(States_Control state, int nl) {
	char *mess = NULL;
	const char *state_text;
	char *tmp;
	int first = 1;
	States_Control current;

	if (state == STATES_READY) {
		asprintf(&mess, "(READY");
	}
	else {
		if (state & STATES_DORMANT) {
			asprintf(&mess, "(DORMANT");
			first = 0;
		}
		else {
			asprintf(&mess, "(");
		}

		for (current = 1; current != 0x80000000; current <<= 1) {
			if (current & state) {
				switch(current & state) {
					case STATES_SUSPENDED:		   state_text = "SUSPENDED"; break;
					case STATES_TRANSIENT:		   state_text = "TRANSIENT"; break;
					case STATES_DELAYING:		   state_text = "DELAYING"; break;
					case STATES_DORMANT:		   state_text = "DORMANT"; break;
					case STATES_WAITING_FOR_TIME:	   state_text = "WAITING FOR TIME"; break;
					case STATES_WAITING_FOR_BUFFER:	   state_text = "WAITING FOR BUFFER"; break;
					case STATES_WAITING_FOR_SEGMENT:   state_text = "WAITING FOR SEGMENT"; break;
					case STATES_WAITING_FOR_MESSAGE:   state_text = "WAITING FOR MESSAGE"; break;
					case STATES_WAITING_FOR_SEMAPHORE: state_text = "WAITING FOR SEMAPHORE"; break;
					case STATES_WAITING_FOR_MUTEX:     state_text = "WAITING FOR MUTEX"; break;
					case STATES_WAITING_FOR_EVENT:     state_text = "WAITING FOR EVENT"; break;
					case STATES_WAITING_FOR_CONDITION_VARIABLE:
									   state_text = "WAITING FOR CONDITION VARIABLE"; break;
					case STATES_WAITING_FOR_PERIOD:	   state_text = "WAITING FOR PERIOD"; break;
					case STATES_WAITING_FOR_SIGNAL:	   state_text = "WAITING FOR SIGNAL"; break;
					case STATES_WAITING_FOR_BARRIER:   state_text = "WAITING FOR BARRIER"; break;
					case STATES_WAITING_FOR_RWLOCK:	   state_text = "WAITING FOR RWLock"; break;
					case STATES_WAITING_FOR_JOIN_AT_EXIT:
									   state_text = "WAITING FOR JOIN AT EXIT"; break;
					case STATES_INTERRUPTIBLE_BY_SIGNAL:
									   state_text = "INTERRUPTIBLE BY SIGNAL"; break;
					case STATES_WAITING_FOR_RPC_REPLY: state_text = "WAITING FOR RPC REPLY"; break;
					default:
					     state_text = "UNKNOWN"; break;
				}
				if (!first) {
					asprintf(&tmp, "%s|%s", mess, state_text);
					first = 0;
				}
				else {
					asprintf(&tmp, "%s%s", mess, state_text);
				}
				free(mess);
				mess = tmp;
			}
		}
	}
	if (nl != 0)
		errlogPrintf("%s)\n", mess);
	else
		errlogPrintf("%s)", mess);
	free(mess);
}

void rtems_stats_show(rtems_stats_ring_buffer *tgt_rb) {
	errlogMessage("Currently not displaying stats\n");

	unsigned current_event;
	unsigned count;

	unsigned prev_id = 0;

	for (count = 0, current_event = tgt_rb->head; count < tgt_rb->num_events; INCR_RB_POINTER(current_event), count++)
	{
		RTEMS_STATS_EVENT *ce = &tgt_rb->thread_activations[current_event];
		unsigned known = 1;
		switch(EVENT_GET_TYPE(ce)) {
			case SWITCH:
				if (prev_id == 0)
					break;
				errlogPrintf("S | %x | %x | ", (unsigned int)prev_id,
							       (unsigned int)ce->obj_id);
				break;
			case BEGIN:
				errlogPrintf("E |         | %x | ", (unsigned int)ce->obj_id);
				break;
			case EXIT:
				errlogPrintf("B | %x |         | ", (unsigned int)ce->obj_id);
				break;
			default:
				errlogPrintf("U | ****\n");
				known = 0;
				break;
		}
		if (known) {
#if defined(WITH_INT_TIME)
			char tstamp_sec[30];
			struct tm t;

			if (gmtime_r(&(ce->stamp.tv_sec), &t) != NULL) {
				if (strftime(tstamp_sec, sizeof(tstamp_sec), "%Y-%m-%dT%H:%M:%S", &t) > 0) {
					errlogPrintf("%s.%09lu | ", tstamp_sec, ce->stamp.tv_nsec);
				}
			}
#else
			errlogPrintf("%lu | ", ce->ticks);
#endif
			rtems_stats_print_state(ce->state, 1);
		}
		prev_id = ce->obj_id;
	}
}

void rtems_stats_snapshot(int count) {
	rtems_stats_ring_buffer *local_rb = rb_active;
	if ((count < 0) || (count > MAX_EVENTS)) {
		errlogPrintf("Wrong number of events. Must be: 0 <= ev < %d; with 0 = max\n", MAX_EVENTS);
		return;
	}

	if (count == 0)
		count = MAX_EVENTS;

	if (rtems_stats_enabled() == RTEMS_SUCCESSFUL) {
		errlogMessage("rtemsStats is in continuous mode. Not taking snapshot");
		return;
	}

	printf("Taking %d events\n", count);
	rtems_stats_reset_rb(local_rb);
	rtems_taking_snapshot = 1;
	rtems_snapshot_count = count;
	rtems_stats_enable();
	if (rtems_stats_enabled() == RTEMS_SUCCESSFUL) {
		rtems_status_code got_lock;

		got_lock = rtems_semaphore_obtain(rtems_stats_sem, RTEMS_WAIT, 10000);
		rtems_stats_disable();
		if (got_lock == RTEMS_SUCCESSFUL) {
			rtems_stats_show(local_rb);
		}
		else {
			switch (got_lock) {
				case RTEMS_TIMEOUT:
					errlogMessage("Timed out waiting for the info to be collected\n");
					break;
				default:
					errlogMessage("Can't acquire the semaphore, somehow...\n");
					break;
			}
		}
	}
	else {
		rtems_taking_snapshot = 0;
	}
}

static void epicsTimeToTimespecInt(struct timespec *ts, epicsTimeStamp *ets) {
	ts->tv_sec  = (uint32_t)ets->secPastEpoch + (uint32_t)(POSIX_TIME_AT_EPICS_EPOCH);
	ts->tv_nsec = (uint32_t)ets->nsec;
}

void rtems_stats_reset_rb(rtems_stats_ring_buffer *local_rb) {
	epicsTimeStamp now;

	memset(local_rb, 0, sizeof(rtems_stats_ring_buffer));

	if (epicsTimeGetCurrent(&now) == epicsTimeOK) {
		// Closest tick to the timestamp that we can get...
		local_rb->ticks = rtems_clock_get_ticks_since_boot();
		epicsTimeToTimespecInt(&local_rb->stamp, &now);
	}
	else {
		errlogMessage("Can't get the time...\n");
	}
}

#define NEXT_ACTIVE_RB ((rb_active == &rb[0]) ? &rb[1] : &rb[0])
#define RB_SWAP       { rtems_stats_ring_buffer *next = rb_export; rb_export = rb_active; rb_active = next; }

#define CLEAR_NEXT_RB { rtems_stats_reset_rb(NEXT_ACTIVE_RB); }

// Returns the ring buffer that was being used at the moment of being called
rtems_stats_ring_buffer *rtems_stats_switch_rb(void) {
	if (rtems_stats_enabled() != RTEMS_SUCCESSFUL) {
		return NULL;
	}

	CLEAR_NEXT_RB;
	rb_switch_trigger = 1;
	if (rtems_semaphore_obtain(rtems_stats_sem, RTEMS_WAIT, 1000) != RTEMS_SUCCESSFUL) {
		return NULL;
	}

	return rb_export;
}

static void rtems_stats_add_event(RTEMS_STATS_EVENT *evt) {
	unsigned index;
#if defined(WITH_INT_TIME)
	epicsTimeStamp now;
#endif

	if (rb_switch_trigger == 1) {
		rb_switch_trigger = 0;
		RB_SWAP;
		rtems_semaphore_release(rtems_stats_sem);
	}

#if defined(WITH_INT_TIME)
	if (epicsTimeGetCurrentInt(&now) == epicsTimeOK) {
	   epicsTimeToTimespec(&evt->stamp, &now);
	}
	else {
		evt->stamp.tv_sec = 0;
		evt->stamp.tv_nsec = 0;
	}
#else
	evt->ticks = rtems_clock_get_ticks_since_boot();
#endif
	index = rb_active->num_events % MAX_EVENTS;
	memcpy(&rb_active->thread_activations[index], evt, sizeof(RTEMS_STATS_EVENT));
	rb_active->num_events++;
	if (index == rb_active->head)
		INCR_RB_POINTER(rb_active->head);

	if (rtems_taking_snapshot) {
		rtems_snapshot_count--;
		if ((rb_active->num_events >= MAX_EVENTS) || (rtems_snapshot_count < 1)) {
			rtems_taking_snapshot = 0;
			RB_SWAP;
			rtems_semaphore_release(rtems_stats_sem);
		}
	}
}

void rtems_stats_switching_context(rtems_tcb *active, rtems_tcb *heir) {
	RTEMS_STATS_EVENT evt = {
		.misc = EVENT_SET_MISC(SWITCH, heir->current_priority, heir->real_priority),
		.state = active->current_state,
		.obj_id  = heir->Object.id,
		.wait_id = active->Wait.id
	};

	SET_ACTIVE_TASK(rb_active, heir->Object.id);
	rtems_stats_add_event(&evt);
}

void rtems_stats_task_begins(rtems_tcb *task) {
	RTEMS_STATS_EVENT evt = {
		.misc = EVENT_SET_MISC(BEGIN, task->current_priority, task->real_priority),
		.obj_id = task->Object.id
	};

	SET_ACTIVE_TASK(rb_active, task->Object.id);
	rtems_stats_add_event(&evt);
}

void rtems_stats_task_exits(rtems_tcb *task) {
	RTEMS_STATS_EVENT evt = {
		.misc = EVENT_SET_MISC(EXIT, task->current_priority, task->real_priority),
		.obj_id = task->Object.id
	};

	SET_ACTIVE_TASK(rb_active, task->Object.id);
	rtems_stats_add_event(&evt);
}

#define NUM_CHUNKS 6
#define MAX_LONGS_IN_CHUNK 4000

static void rtems_stats_export_init(aSubRecord *prec) {
	int i;
	void **pval;
	void **povl;

	void *val, *ovl;

	for (i = 0,
	     pval = &prec->valf,
	     povl = &prec->ovlf;
	     i < NUM_CHUNKS;
	     i++,
	     pval++,
	     povl++) {
		free(*pval);
		free(*povl);
	}

	// We allocate taking into account the largest of the two, to match the DB definition
	val = callocMustSucceed(MAX_EVENTS, sizeof(rtems_stats_event_with_timestamp), "rtems_stats_export_init -> pval");
	ovl = callocMustSucceed(MAX_EVENTS, sizeof(rtems_stats_event_with_timestamp), "rtems_stats_export_init -> povl");

	for (i = 0, pval = &prec->valf, povl = &prec->ovlf; i < NUM_CHUNKS; i++, pval++, povl++) {
		*pval = &((uint32_t *)val)[MAX_LONGS_IN_CHUNK * i];
		*povl = &((uint32_t *)ovl)[MAX_LONGS_IN_CHUNK * i];
	}
}

/*+
 *   Function name:
 *   rtems_stats_export_support
 *
 *   Purpose:
 *
 *   EPICS outputs:
 *
 *   vala => ticks per second
 *   valb => seconds at the beginning of the capture
 *   valc => nanoseconds at the beginning of the capture
 *   vald => number of events
 *   vale => index of the first event
 *   valf => array chunk #1
 *   valg => array chunk #2
 *   valh => array chunk #3
 *   vali => array chunk #4
 *   valj => array chunk #5
 *   valk => array chunk #6
 *   vall => array chunk #7
 *   valr => array: IDs for the captured tasks
 *   vals => array: (known) names for the tasks
 *   valt => ticks at the beginning of the capture
 *   valu => record size as multiple of LONG
 */

const unsigned sizeinlongs = sizeof(RTEMS_STATS_EVENT) / sizeof(unsigned long);

static long rtems_stats_export_support(aSubRecord *prec) {
	unsigned nevents = 0;
	unsigned total_longs = 0;
	int i;
	epicsUInt32 *nev;

	*(epicsUInt32 *)prec->vala = rtems_clock_get_ticks_per_second();
	*(unsigned long *)prec->valu = sizeinlongs;

	if (rtems_stats_enabled() == RTEMS_SUCCESSFUL) {
		rtems_stats_ring_buffer *export = rtems_stats_switch_rb();
		unsigned nids = 0;

		if (export == NULL) {
			errlogMessage("RTEMS STATS: Error trying to switch ring buffers");
			return 1;
		}

		nevents = export->num_events;

		memcpy(prec->valf, export->thread_activations, MAX_EVENTS * sizeof(RTEMS_STATS_EVENT));
		*(epicsUInt32 *)prec->valb = export->stamp.tv_sec;
		*(epicsUInt32 *)prec->valc = export->stamp.tv_nsec;
		*(epicsUInt32 *)prec->vale = export->head;
		*(epicsUInt32 *)prec->valt = export->ticks;

		for (i = 0; i < ARRAY_IDS_SIZE; i++) {
			if (export->ids[i] != 0) {
				int j;
				uint32_t tidbase;
				char tname[MAX_STRING_SIZE];

				tidbase = 0xa010000 + (i * 32);
				for (j = 0; j < 32; j++) {
					if ((1 << j) & export->ids[i]) {
						((epicsUInt32 *)prec->valr)[nids] = tidbase + j;
						epicsThreadGetName((epicsThreadId)(tidbase + j), tname, MAX_STRING_SIZE);
						if (strlen(tname) != 0)
							strcpy(&((char *)prec->vals)[nids * MAX_STRING_SIZE], tname);
						else
							strcpy(&((char *)prec->vals)[nids * MAX_STRING_SIZE], "UNKNOWN");
						nids ++;
					}
				}
			}
		}

		// TODO: It's unlikely that we have an only event, but if nids would be 1, this won't do...
		prec->nevr = nids;
		prec->nevs = nids;
	}

	*(epicsUInt32 *)prec->vald = nevents;
	total_longs = nevents * sizeinlongs;

	for (i = 0, nev = &prec->nevf; i < NUM_CHUNKS; i++, nev++) {
		if (total_longs >= MAX_LONGS_IN_CHUNK) {
			*nev = MAX_LONGS_IN_CHUNK;
			total_longs -= MAX_LONGS_IN_CHUNK;
		}
		else {
			*nev = total_longs > 0 ? total_longs : 1;
			total_longs = 0;
		}
	}

	return 0;
}

static void rtems_stats_control_init(aSubRecord *prec) {
	*(short *)prec->vala = 1;
	strcpy((char *)prec->valb, "UNKNOWN");
}

enum rtems_stats_control_command {
	INFO,
	ENABLE,
	DISABLE,
	UNKNOWN
};

#define RTEMS_STATS_PRECISE_TIMING 0x01
#define RTEMS_STATS_IS_ENABLED     0x02

static long rtems_stats_control_support(aSubRecord *prec) {
	char *cmds = (char*)prec->a;
	char *results = "UNKNOWN";
	enum rtems_stats_control_command cmd = UNKNOWN;
	unsigned ret = 1;
	unsigned short *vala = (unsigned short *)prec->vala;
	unsigned *valc = (unsigned *)prec->valc;

	if (!strncmp(cmds, "INFO", MAX_STRING_SIZE)) {
		cmd = INFO;
	}
	else if (!strncmp(cmds, "ENABLE", MAX_STRING_SIZE)) {
		cmd = ENABLE;
	}
	else if (!strncmp(cmds, "DISABLE", MAX_STRING_SIZE)) {
		cmd = DISABLE;
	}
	else {
		errlogMessage("rtems_stats_control_support: Received garbage\n");
	}


	switch (cmd) {
		case INFO:
			results = "ACCEPT";

			*valc = 0;
#if defined(WITH_INT_TIME)
			*valc |= RTEMS_STATS_PRECISE_TIMING;
#endif
			if (rtems_stats_enabled() == RTEMS_SUCCESSFUL) {
				*valc |= RTEMS_STATS_IS_ENABLED;
			}
			ret = 0;
			break;
		case ENABLE:
			if (rtems_stats_enable() == RTEMS_SUCCESSFUL) {
				rtems_stats_switch_rb();
				*vala = 0;
				results = "ACCEPT";
			}
			else {
				results = "REJECT";
				*vala = 1;
			}
			ret = 0;
			break;
		case DISABLE:
			rtems_stats_disable();
			*vala = 1;
			results = "ACCEPT";
			ret = 0;
			break;
		default:
			break;
	}
	strcpy((char *)prec->valb, results);

	return ret;
}

/*+
 *   Function name:
 *   rtemsStatsReport
 *
 *   Purpose:
 *   Report on statistics related to RTEMS task execution
 *
 *-
 */

static void rtemsStatsReport() {
	rtems_cpu_usage_report();
}


/*+
 *   Function name:
 *   rtemsStatsReset
 *
 *   Purpose:
 *   Reset RTEMS statistics
 *
 *-
 */
static void rtemsStatsReset() {
	rtems_cpu_usage_reset();
}

static const iocshFuncDef rtemsStatsReportFuncDef = {"rtemsStatsReport", 0, NULL};
static const iocshFuncDef rtemsStatsResetFuncDef = {"rtemsStatsReset", 0, NULL};
// New style
static const iocshArg rtemsStatsCountArg = {"count", iocshArgInt};
static const iocshArg *const rtemsStatsSnapArgs[] = {&rtemsStatsCountArg};
static const iocshFuncDef rtemsStatsSnapFuncDef = {"rtemsStatsSnap", 1, rtemsStatsSnapArgs};
static const iocshFuncDef rtemsStatsEnableFuncDef = {"rtemsStatsEnable", 0, NULL};
static const iocshFuncDef rtemsStatsDisableFuncDef = {"rtemsStatsDisable", 0, NULL};

static void rtemsStatsReportCallFunc(const iocshArgBuf *args)
{
	rtemsStatsReport();
}

static void rtemsStatsResetCallFunc(const iocshArgBuf *args)
{
	rtemsStatsReset();
}

static void rtemsStatsSnapCallFunc(const iocshArgBuf *args)
{
	rtems_stats_snapshot(args[0].ival);
}

static void rtemsStatsEnableCallFunc(const iocshArgBuf *args)
{
	rtems_stats_enable();
}

static void rtemsStatsDisableCallFunc(const iocshArgBuf *args)
{
	rtems_stats_disable();
}

static void rtemsStatsRegister() {
	iocshRegister(&rtemsStatsReportFuncDef, rtemsStatsReportCallFunc);
	iocshRegister(&rtemsStatsResetFuncDef, rtemsStatsResetCallFunc);
	iocshRegister(&rtemsStatsSnapFuncDef, rtemsStatsSnapCallFunc);
	iocshRegister(&rtemsStatsEnableFuncDef, rtemsStatsEnableCallFunc);
	iocshRegister(&rtemsStatsDisableFuncDef, rtemsStatsDisableCallFunc);
}

epicsExportRegistrar(rtemsStatsRegister);
epicsRegisterFunction(rtems_stats_export_init);
epicsRegisterFunction(rtems_stats_export_support);
epicsRegisterFunction(rtems_stats_control_init);
epicsRegisterFunction(rtems_stats_control_support);
