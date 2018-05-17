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
static void rtems_stats_snapshot(void);

#define MAX_EVENTS 4096

typedef enum {
	SWITCH,
	BEGIN,
	EXIT
} rtems_stats_event_type;

typedef struct {
	rtems_stats_event_type type;
	States_Control state;
	rtems_id begin_id;
	rtems_id end_id;
	rtems_interval ticks;
} rtems_stats_event;

#define INCR_RB_POINTER(x) (x = (x + 1) % MAX_EVENTS)
typedef struct {
	double   stamp;
	unsigned num_events;
	unsigned head;
	rtems_stats_event thread_activations[MAX_EVENTS];
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
static int rb_switch_trigger = 0;

static rtems_id rtems_stats_extension_table_id;
static rtems_name rtems_stats_table_name = rtems_build_name('R', 'T', 'S', 'T');
static rtems_id rtems_stats_sem;

static Timestamp_Control last_stamp;

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

	_TOD_Get_uptime(&last_stamp);
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
	unsigned current_event;
	unsigned count;

	errlogMessage("Displaying stats...\n");
	errlogPrintf("Ticks per second: %lu\n", rtems_clock_get_ticks_per_second());
	errlogPrintf("Total events: %d\n", tgt_rb->num_events);
	for (count = 0, current_event = tgt_rb->head; count < tgt_rb->num_events; INCR_RB_POINTER(current_event), count++)
	{
		rtems_stats_event *ce = &tgt_rb->thread_activations[current_event];
		switch(ce->type) {
			case SWITCH:
				errlogPrintf("S | %x | %x | %lu | ", (unsigned int)ce->end_id,
								  (unsigned int)ce->begin_id,
								  ce->ticks);
				rtems_stats_print_state(ce->state, 1);
				break;
			case BEGIN:
				errlogPrintf("E |         | %x | %lu | ", (unsigned int)ce->begin_id,
										ce->ticks);
				rtems_stats_print_state(ce->state, 1);
				break;
			case EXIT:
				errlogPrintf("B | %x |         | %lu | ", (unsigned int)ce->end_id,
										ce->ticks);
				rtems_stats_print_state(ce->state, 1);
				break;
			default:
				errlogPrintf("U | ****\n");
				break;
		}
	}
}

void rtems_stats_snapshot(void) {
	rtems_stats_ring_buffer *local_rb = rb_active;

	if (rtems_stats_enabled() == RTEMS_SUCCESSFUL) {
		errlogMessage("rtemsStats is enabled at this time. Not taking snapshot");
		return;
	}

	rtems_taking_snapshot = 1;
	errlogPrintf("Size of the rtems event struct: %d\n", sizeof(rtems_stats_event));
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

void rtems_stats_reset_rb(rtems_stats_ring_buffer *local_rb) {
	local_rb->stamp = 0;
	local_rb->num_events = 0;
	local_rb->head = 0;
}

#define NEXT_ACTIVE_RB ((rb_active == &rb[0]) ? &rb[1] : &rb[0])
#define RB_SWAP       { rtems_stats_ring_buffer *next = NEXT_ACTIVE_RB; rb_export = rb_active; rb_active = next; }
#define CLEAR_NEXT_RB { rtems_stats_reset_rb(NEXT_ACTIVE_RB); }

// Returns the ring buffer that was being used at the moment of being called
rtems_stats_ring_buffer *rtems_stats_switch_rb(void) {
	rtems_stats_ring_buffer *active = rb;

	if (rtems_stats_enabled() != RTEMS_SUCCESSFUL) {
		return NULL;
	}

	CLEAR_NEXT_RB;
	rb_switch_trigger = 1;
	if (rtems_semaphore_obtain(rtems_stats_sem, RTEMS_WAIT, 1000) != RTEMS_SUCCESSFUL) {
		return NULL;
	}

	return active;
}

static void rtems_stats_add_event(rtems_stats_event *evt) {
	unsigned index;

	if (rb_switch_trigger == 1) {
		rb_switch_trigger = 0;
		RB_SWAP;
		rtems_semaphore_release(rtems_stats_sem);
	}

	evt->ticks = rtems_clock_get_ticks_since_boot();
	index = rb_active->num_events % MAX_EVENTS;
	memcpy(&rb_active->thread_activations[index], evt, sizeof(rtems_stats_event));
	rb_active->num_events++;
	if (index == rb_active->head)
		INCR_RB_POINTER(rb_active->head);

	if (rtems_taking_snapshot && (rb_active->num_events >= MAX_EVENTS)) {
		rtems_taking_snapshot = 0;
		RB_SWAP;
		rtems_semaphore_release(rtems_stats_sem);
	}
}

void rtems_stats_switching_context(rtems_tcb *active, rtems_tcb *hier) {
	rtems_stats_event evt = {
		.type = SWITCH,
		.state = active->current_state,
		.begin_id = hier->Object.id,
		.end_id = active->Object.id
	};

	rtems_stats_add_event(&evt);
}

void rtems_stats_task_begins(rtems_tcb *task) {
	rtems_stats_event evt = {
		.type = BEGIN,
		.begin_id = task->Object.id
	};

	rtems_stats_add_event(&evt);
}

void rtems_stats_task_exits(rtems_tcb *task) {
	rtems_stats_event evt = {
		.type = EXIT,
		.end_id = task->Object.id
	};

	rtems_stats_add_event(&evt);
}

#define NUM_ARGS 16

static void rtems_stats_export_init(aSubRecord *prec) {
	int i;
	void **pval;
	void **povl;

	void *vala, *ovla;

	for (i = 0,
	     pval = &prec->vala,
	     povl = &prec->ovla;
	     i < NUM_ARGS;
	     i++,
	     pval++,
	     povl++) {
		free(*pval);
		free(*povl);
	}

	vala = callocMustSucceed(MAX_EVENTS, sizeof(rtems_stats_event), "rtems_stats_export_init -> vala");
	ovla = callocMustSucceed(MAX_EVENTS, sizeof(rtems_stats_event), "rtems_stats_export_init -> povl");
	printf("Allocated two blocks of %d * %d = %d bytes\n", MAX_EVENTS, sizeof(rtems_stats_event), MAX_EVENTS * sizeof(rtems_stats_event));

	for (i = 0, pval = &prec->vala, povl = &prec->ovla; i < NUM_ARGS; i++, pval++, povl++) {
		*pval = &((rtems_stats_event *)vala)[512 * i];
		*povl = &((rtems_stats_event *)ovla)[512 * i];
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
 *   valb => timestamp at the beginning of the capture
 *   valc => number of events
 *   vald => index of the first event
 *   vale => array chunk #1
 *   valf => array chunk #2
 *   valg => array chunk #3
 *   valh => array chunk #4
 *   vali => array chunk #5
 *   valj => array chunk #6
 *   valk => array chunk #7
 *   vall => array chunk #8
 *   valm => array chunk #9
 *   valn => array chunk #10
 *   valo => array chunk #11
 *   valp => array chunk #12
 *   valq => array chunk #13
 *   valr => array chunk #14
 *   vals => array chunk #15
 *   valt => array chunk #16
 */

static long rtems_stats_export_support(aSubRecord *prec) {
	rtems_stats_ring_buffer *rb = rtems_stats_switch_rb();

	if (rb == NULL) {
		return 1;
	}

	*(uint32_t *)prec->vala = (uint32_t)rtems_clock_get_ticks_per_second();
	*(double *)prec->valb = rb->stamp;
	*(uint32_t *)prec->valc = (uint32_t)rb->num_events;
	*(uint32_t *)prec->vald = (uint32_t)rb->head;
	// memcpy(prec->vala, rb->thread_activations, sizeof(rb->thread_activations));
	/*
	memcpy(prec->vale, &rb->thread_activations[  0   ], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valf, &rb->thread_activations[512   ], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valg, &rb->thread_activations[512* 2], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valh, &rb->thread_activations[512* 3], sizeof(rtems_stats_event) * 512);
	memcpy(prec->vali, &rb->thread_activations[512* 4], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valj, &rb->thread_activations[512* 5], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valk, &rb->thread_activations[512* 6], sizeof(rtems_stats_event) * 512);
	memcpy(prec->vall, &rb->thread_activations[512* 7], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valm, &rb->thread_activations[512* 8], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valn, &rb->thread_activations[512* 9], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valo, &rb->thread_activations[512*10], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valp, &rb->thread_activations[512*11], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valq, &rb->thread_activations[512*12], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valr, &rb->thread_activations[512*13], sizeof(rtems_stats_event) * 512);
	memcpy(prec->vals, &rb->thread_activations[512*14], sizeof(rtems_stats_event) * 512);
	memcpy(prec->valt, &rb->thread_activations[512*15], sizeof(rtems_stats_event) * 512);
	*/

	return 0;
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
static const iocshFuncDef rtemsStatsSnapFuncDef = {"rtemsStatsSnap", 0, NULL};
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
	rtems_stats_snapshot();
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
