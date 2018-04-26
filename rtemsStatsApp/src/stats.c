#include <epicsStdio.h>
#include <registryFunction.h>
#include <epicsExport.h>
#include <iocsh.h>
#include <initHooks.h>

#include <rtems.h>
#include <cpuuse/cpuuse.h>

#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 1024

/*
typedef struct rtems_stats_period {
	rtems_id task_id;
};

typedef struct rtems_stats_block {
	unsigned num_events;
	rtems_stats_period thread_activations[MAX_EVENTS];
};
*/

// Just in case we've got more threads than configured...
static unsigned rtems_stats_additional_activations = 0;

static void rtems_stats_initialize(void) {

	// REGISTER THE EXTENSION
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

static void rtemsStatsReset() {
	rtems_cpu_usage_reset();
}

static const iocshFuncDef rtemsStatsReportFuncDef = {"rtemsStatsReport", 0, NULL};
static const iocshFuncDef rtemsStatsResetFuncDef = {"rtemsStatsReset", 0, NULL};

static void rtemsStatsReportCallFunc(const iocshArgBuf *args)
{
	rtemsStatsReport();
}

static void rtemsStatsResetCallFunc(const iocshArgBuf *args)
{
	rtemsStatsReset();
}

static void rtemsStatsRegister() {
	iocshRegister(&rtemsStatsReportFuncDef, rtemsStatsReportCallFunc);
	iocshRegister(&rtemsStatsResetFuncDef, rtemsStatsResetCallFunc);
}

epicsExportRegistrar(rtemsStatsRegister);
