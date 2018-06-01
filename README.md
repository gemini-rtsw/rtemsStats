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
guaranteed to work. Additionally, it depends on NumPy and PyEpics. Find it
under the `clients` directory:

```
$ clients/monitor.py PREFIX
```

The output will be sent to standard output and looks like this:

```
Creating the SessionTracker for PV: tc1:rtems:stats
ENABLING
Dumping dataset with timestamp 1527831854.6, reported start at 2018-06-01 05:44:14.435835
#events: 1700
2018-06-01T05:44:14.435835: 0xa01004d -> 0xa01004e (WAITING FOR MUTEX)
2018-06-01T05:44:14.435835: 0xa01004e -> 0xa01004d (READY)
2018-06-01T05:44:14.435835: 0xa01004d -> 0xa01004e (WAITING FOR EVENT)
2018-06-01T05:44:14.435935: 0xa01001d -> 0xa01004d (READY)
2018-06-01T05:44:14.435935: 0xa01004d -> 0xa01001d (WAITING FOR MUTEX)
2018-06-01T05:44:14.435935: 0xa010005 -> 0xa01004d (READY)
2018-06-01T05:44:14.435935: 0xa01004d -> 0xa010005 (WAITING FOR EVENT)
2018-06-01T05:44:14.436035: 0xa01001f -> 0xa01004d (WAITING FOR MUTEX)
2018-06-01T05:44:14.436035: 0xa010005 -> 0xa01001f (READY)
2018-06-01T05:44:14.436035: 0xa010004 -> 0xa010005 (WAITING FOR EVENT)
2018-06-01T05:44:14.436035: 0xa01001f -> 0xa010004 (WAITING FOR EVENT)
2018-06-01T05:44:14.436035: 0xa010003 -> 0xa01001f (WAITING FOR MUTEX)
2018-06-01T05:44:14.436035: 0x9010001 -> 0xa010003 (WAITING FOR MUTEX)
2018-06-01T05:44:14.436235: 0xa01001e -> 0x9010001 (READY)
...
```

The format is:

```
UTC Timestamp: TASK A ID -> TASK B ID (STATE OF TASK A)
```

Where TASK A is the one leaving the CPU and TASK B the one acquiring it.
