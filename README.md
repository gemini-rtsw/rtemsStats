# rtemsStats

EPICS support module to get thread info from RTEMS

Compatible with RTEMS = 4.10 (and possibly 4.11), and EPICS using the OSI layer (tested under 3.14.12.7)

## Building

The module should be straightforward to build. Just check `configure/RELEASE`
and correct as needed.  The files under `configure` include some
Gemini-specific includes, but they should not make your build fail.

The following instructions make reference to Makefiles according to our
standard code layout. Please make changes as needed.

### Using epicsTimeGetCurrentInt for timestamps

Given that this is an EPICS support module, it relies on EPICS timestamps:
we've seen irregular behavior when trying to use pure RTEMS calls to grab the
time (eg. time going backwards...), probably when EPICS udpates the OS clock
based on other time provider(s).

The event capture runs on RTEMS core, though, and we cannot rely on
`epicsTimeGetCurrent`, but only on `epicsTimeGetCurrentInt`. As this
functionality may not be registered for your time provider, the default for
rtemsStats is to rely on RTEMS ticks instead (which are guaranteed to be
monotonically increasing): at the beginning of each sample (every 0.2 seconds),
we grab the current time and system tick, and then use those and the tick at
the time of an event to extrapolate timestamps.

The problem with this is that the time resolution for our event capture will be
only as good as RTEMS' tick rate. This may be enough for your purposes, but in
many cases it will be too coarse.

If this is the case **and** your time provider is registering a callback for
`epicsTimeGetCurrentInt`, then you can edit `configure/CONFIG_SITE.local` to
enable support. Please remember to clean your build after you make changes
here.

## Integration into your Project

Add the module to your `configure/RELEASE` as usual. Additionally, you will
want to add `App/src/Makefile` the following lines:

```
app_DBD += rtemsStats.dbd
app_LIBS += rtemsStats
```

To be able to use the client interface , add to your `App/Db/Makefile`:

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
guaranteed to work. Additionally, it depends on `NumPy` and `PyEpics`. Find it
under the `clients` directory:

```
$ clients/monitor.py -h
usage: monitor.py [-h] [-v] [-r] [--format {console,csv}] top

RTEMS/EPICS Monitor

positional arguments:
  top                   Top of the database, as in {top}:rtems:stats

  optional arguments:
    -h, --help            show this help message and exit
    -v                    More verbose output
    -r                    Display RTEMS priorities (default is to show EPICS
                          ones). Does not affect all output types
    --format {console,csv}
                          Output format
```

**NB**: the CSV output format is still not implemented...

### Console output

The console output will be sent to standard output and looks like this:

```
$ clients/monitor.py tc1 
2018-06-08T23:22:51.658503: CAS-event            -> CAS-client            ---/ 020 (READY, 0x1a013981)
2018-06-08T23:22:51.658503: CAS-client           -> CAS-event             ---/ 019 (WAITING FOR EVENT, 0x1a013982)
2018-06-08T23:22:51.658503: CAS-event            -> FECd                  ---/*010 (READY, 0x1a013981)
2018-06-08T23:22:51.658503: FECd                 -> CAS-event             ---/ 019 (WAITING FOR EVENT, 0x1a010021)
2018-06-08T23:22:51.658503: CAS-event            -> errlog                ---/ 010 (WAITING FOR MUTEX, 0x1a013981)
2018-06-08T23:22:51.658503: errlog               -> IDLE                  ---/*255 (WAITING FOR MUTEX, 0x1a010018)
2018-06-08T23:22:51.668503: IDLE                 -> scan0.01              ---/ 068 (READY)
2018-06-08T23:22:51.668503: scan0.01             -> scan0.05              ---/ 067 (WAITING FOR MUTEX, 0x1a013699)
2018-06-08T23:22:51.668503: scan0.05             -> IDLE                  ---/*255 (WAITING FOR MUTEX, 0x1a013697)
...
2018-06-08T23:22:51.730243: timerQueue           -> CAC-event             ---/ 051 (WAITING FOR MUTEX, 0x1a0136cf)
2018-06-08T23:22:51.730243: CAC-event            -> scanOnce              ---/ 070 (READY, 0x1a0136df)
2018-06-08T23:22:51.730243: scanOnce             -> CAC-event             070/ 051 (WAITING FOR MUTEX, 0x1a012d0d)
2018-06-08T23:22:51.730243: CAC-event            -> scanOnce              ---/ 070 (READY, 0x1a0136df)
2018-06-08T23:22:51.730243: scanOnce             -> CAC-event             ---/ 051 (WAITING FOR MUTEX, 0x1a013687)
2018-06-08T23:22:51.730243: CAC-event            -> CAS-event             ---/ 019 (WAITING FOR MUTEX, 0x1a0136df)
...
```

The format is:

```
UTC Timestamp: TASK_A_ID -> TASK_B_ID   CURRENT_PRIORITY/REAL_PRIORITY (STATE OF TASK A[, object A is waiting on])
```

With the following details:

* Task A is the one leaving the CPU
* Task B is the one acquiring it.
* Both priorities are for Task B. When the "current priority" is the same as
  the real one, `---` is displayed instead of an number.
* EPICS priorities (0-99) are shown for EPICS tasks. When a priority does
  not map to the EPICS range, the RTEMS priority (255-0) will be shown, with
  an asterisk preceding it (see `IDLE` and `FECd` in our example).
* The task states are taken straight from the RTEMS Task Control Block, and
  they're not not EPICS-aware, of course.

This last point may make for some confusing traces. For example, in the
`scan0.01 -> scan0.05` transition above, `scan0.01` has actually finished
scanning its list and it's actually waiting for its next activation. Future
versions of this module will (hopefully) be more EPICS-aware.
