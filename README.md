# rtemsStats

EPICS support module to get thread info from RTEMS

Compatible with RTEMS = 4.10.

## Integration into your Project

Add the module to your `configure/RELEASE`, as usual. Additionally, you will
want to add to your `App/src/Makefile` the following lines:

```
app_DBD += rtemsStats.dbd
app_LIBS += rtemsStats
```

If you plan to use the client interface, add to your `App/Db/Makefile`:

```
DB_INSTALLS += rtemsStats.db
```

and to the startup script:

```
dbLoadRecords("db/rtemsStats.db", "IOC=<the_prefix>")
```

## IOC Shell Use

The module exports a number of iocsh calls, most of them for debugging use. Of
interest:

```
iocsh> rtemsStatsSnap <numsamples>
```

will temporarily enable the stats module, capture the number of specified
samples (<= max samples, check the stats.c for this), and print them out.

## Client Interface

We provide a sample Python script to import information from the IOC and
dump it out to a client machine. At the moment the script has been written
without 2/3 compatibility in mind, only Python 2 with `ctypes` included is
guaranteed to work. Additionally, it depends on *NumPy* and *PyEpics*. Find it
under the `clients` directory:

```
$ clients/monitor.py PREFIX
```

The output will be sent to standard output and looks like this:

```
Creating the SessionTracker for PV: tc1:rtems:stats
ENABLING
Dumping dataset with timestamp 1527851613.82, reported start at 2018-06-01 11:13:33.701438
#events: 1164; ticks per second: 10000
2018-06-01T11:13:33.701438: 0xa010053 -> 0xa010054 ---/180 (WAITING FOR MUTEX)
2018-06-01T11:13:33.701438: 0xa010054 -> 0xa010053 ---/179 (READY)
2018-06-01T11:13:33.701538: 0xa010004 -> 0xa010054 ---/010 (READY)
2018-06-01T11:13:33.701538: 0xa01001e -> 0xa010004 ---/126 (WAITING FOR EVENT)
2018-06-01T11:13:33.701538: 0xa010005 -> 0xa01001e ---/010 (READY)
2018-06-01T11:13:33.701538: 0xa01001e -> 0xa010005 ---/126 (WAITING FOR EVENT)
...
```

The format is:

```
UTC Timestamp: TASK_A_ID -> TASK_B_ID CURRENT_PRIORITY/REAL_PRIORITY (STATE OF TASK A)
```

Where TASK A is the one leaving the CPU and TASK B the one acquiring it.
If `CURRENT_PRIORITY` is `---`, it means that the task is using its real
priority.
