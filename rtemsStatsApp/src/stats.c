#include <epicsStdio.h>
#include <registryFunction.h>
#include <epicsExport.h>
#include <iocsh.h>
#include <initHooks.h>
#include <epicsPrint.h>

#include <rtems.h>
#include <rtems/extension.h>
#include <cpuuse/cpuuse.h>

#include <stdlib.h>
#include <string.h>

static void rtems_stats_switching_context(rtems_tcb *, rtems_tcb *);
static void rtems_stats_task_begins(rtems_tcb *);
static void rtems_stats_task_exits(rtems_tcb *);
static void rtems_stats_enable(void);
static void rtems_stats_disable(void);
static void rtems_stats_show(void);

#define MAX_EVENTS 256

typedef enum {
	SWITCH,
	BEGIN,
	EXIT
} rtems_stats_event_type;

typedef struct {
	rtems_stats_event_type type;
	rtems_id begin_id;
	rtems_id end_id;
	struct timeval begin;
	struct timeval end;
} rtems_stats_event;

#define INCR_RB_POINTER(x) (x = (x + 1) % MAX_EVENTS)
typedef struct {
	unsigned num_events;
	unsigned head;
	rtems_stats_event thread_activations[MAX_EVENTS];
} rtems_stats_ring_buffer;

static rtems_stats_ring_buffer rb[2];
static rtems_stats_ring_buffer *rb_current = &rb[0];

static void rtems_stats_clear_rb(rtems_stats_ring_buffer *);
static rtems_stats_ring_buffer *rtems_stats_switch_rb(void);

static rtems_extensions_table rtems_stats_extension_table = {
	.thread_switch  = rtems_stats_switching_context,
	.thread_begin   = rtems_stats_task_begins,
	.thread_exitted = rtems_stats_task_exits,
};

static rtems_id rtems_stats_extension_table_id;
static rtems_name rtems_stats_table_name = rtems_build_name('R', 'T', 'S', 'T');
static rtems_id rtems_stats_sem;

static int rtems_stats_enabled(void) {
	rtems_id id;

	return rtems_extension_ident(rtems_stats_table_name, &id);
}

void rtems_stats_enable(void) {
	// REGISTER THE EXTENSION
	rtems_status_code ret;
	char name[10], *res;

	if (rtems_stats_enabled() == RTEMS_SUCCESSFUL)
		return;

	if(rtems_semaphore_create(rtems_build_name('S', 'T', 'S', 'M'), 1,
			       RTEMS_SIMPLE_BINARY_SEMAPHORE, 0, &rtems_stats_sem) != RTEMS_SUCCESSFUL)
	{
		errlogMessage("Cannot create a semaphore for the stats module");
		return;
	}

	ret = rtems_extension_create(rtems_stats_table_name,
				    &rtems_stats_extension_table,
				    &rtems_stats_extension_table_id);

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
		case RTEMS_SUCCESSFUL:
		default:
			errlogMessage("rtemsStats enabled");
			break;
	}
}

void rtems_stats_disable(void) {
	if (rtems_extension_delete(rtems_stats_extension_table_id) == RTEMS_SUCCESSFUL) {
		rtems_stats_extension_table_id = 0;
		errlogMessage("rtemsStats disabled");
	}
	else {
		errlogMessage("rtemsStats doesn't seem to be enabled\n");
	}
}

void rtems_stats_show(void) {
	rtems_stats_ring_buffer *local_rb;
	unsigned current_event, tail;

	if (rtems_stats_enabled() != RTEMS_SUCCESSFUL) {
		errlogMessage("Stats are not enabled");
		return;
	}

	local_rb = rtems_stats_switch_rb();
	tail = local_rb->num_events % MAX_EVENTS;
	for (current_event = local_rb->head; current_event != tail; INCR_RB_POINTER(current_event))
	{
		// TODO
	}
}

void rtems_stats_clear_rb(rtems_stats_ring_buffer *local_rb) {
	memset(local_rb, 0, sizeof(rtems_stats_ring_buffer));
}

// Returns the ring buffer that was being used at the moment of being called
rtems_stats_ring_buffer *rtems_stats_switch_rb(void) {
	rtems_stats_ring_buffer *current = rb;
	rtems_stats_ring_buffer *next;
	rtems_status_code sem_ret;

	next = (rb_current == &rb[0]) ? &rb[1] : &rb[0];
	sem_ret = rtems_semaphore_obtain(rtems_stats_sem, RTEMS_WAIT, 20);
	if (sem_ret == RTEMS_TIMEOUT) {
		errlogMessage("Timeout trying to acquire the stats semaphore");
		return NULL;
	} else if (sem_ret != RTEMS_SUCCESSFUL) {
		errlogMessage("Can't acquire the stats semaphore");
		return NULL;
	}
	rtems_stats_clear_rb(next);
	rb_current = next;
	rtems_semaphore_release(rtems_stats_sem);

	return current;
}

static void rtems_stats_add_event(rtems_stats_event *evt) {
	unsigned index;
	int got_it = 0;
	unsigned count = 0;

	while(!got_it) {
		switch(rtems_semaphore_obtain(rtems_stats_sem, RTEMS_WAIT, 1)) {
			case RTEMS_OBJECT_WAS_DELETED:
			case RTEMS_INVALID_ID:
				// Should never happen, but make sure we don't get stuck here
				return;
				break;
			case RTEMS_TIMEOUT:
			case RTEMS_UNSATISFIED:
				if ((count += 1) > 1) {
					// Taking too long!
					return;
				}
			case RTEMS_SUCCESSFUL:
			default:
				got_it = 1;
				break;
		}
	}
	index = rb_current->num_events % MAX_EVENTS;
	memcpy(&rb_current->thread_activations[index], evt, sizeof(rtems_stats_event));
	rb_current->num_events++;
	if (index == rb_current->head)
		INCR_RB_POINTER(rb_current->head);
	rtems_semaphore_release(rtems_stats_sem);
}

void rtems_stats_switching_context(rtems_tcb *current, rtems_tcb *hier) {
	rtems_stats_event evt = {
		.type = SWITCH,
		.begin_id = hier->Object.id,
		.end_id = current->Object.id
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
static const iocshFuncDef rtemsStatsEnableFuncDef = {"rtemsStatsEnable", 0, NULL};
static const iocshFuncDef rtemsStatsDisableFuncDef = {"rtemsStatsDisable", 0, NULL};
static const iocshFuncDef rtemsStatsShowFuncDef = {"rtemsStatsShow", 0, NULL};

static void rtemsStatsReportCallFunc(const iocshArgBuf *args)
{
	rtemsStatsReport();
}

static void rtemsStatsResetCallFunc(const iocshArgBuf *args)
{
	rtemsStatsReset();
}

static void rtemsStatsEnableCallFunc(const iocshArgBuf *args)
{
	rtems_stats_enable();
}

static void rtemsStatsDisableCallFunc(const iocshArgBuf *args)
{
	rtems_stats_disable();
}

static void rtemsStatsShowCallFunc(const iocshArgBuf *args)
{
	rtems_stats_show();
}

static void rtemsStatsRegister() {
	iocshRegister(&rtemsStatsReportFuncDef, rtemsStatsReportCallFunc);
	iocshRegister(&rtemsStatsResetFuncDef, rtemsStatsResetCallFunc);
	iocshRegister(&rtemsStatsEnableFuncDef, rtemsStatsEnableCallFunc);
	iocshRegister(&rtemsStatsDisableFuncDef, rtemsStatsDisableCallFunc);
	iocshRegister(&rtemsStatsShowFuncDef, rtemsStatsShowCallFunc);
}

epicsExportRegistrar(rtemsStatsRegister);
