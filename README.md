# rtemsStats

EPICS support module to get thread stats from RTEMS

Compatible with RTEMS >= 4.10.

## Integration into your Project

Add the module to your `configure/RELEASE`, as usual. Additionally, you will
want to add to your `App/src/Makefile` the following lines:

```
app_DBD += rtemsStats.dbd
app_LIBS += rtemsStats
```

## Use

The module exports two iocsh calls:

* rtemsStatsReport
* rtemsStatsReset

These calls give you access to the corresponding RTEMS functions
(`rtems_cpus_usage_report`, `rtems_cpus_usage_reset`), documented under
the [CPU Usage Statistics](https://ftp.rtems.org/pub/rtems/people/chrisj/doc-online/4.11/c-user/cpu_usage_statistics.html)
section of the C User Manual.

