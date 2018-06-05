#!/usr/bin/env python

# vim: ai:sw=4:sts=4:expandtab

from contextlib import contextmanager
from collections import defaultdict, namedtuple
from time import sleep
import ctypes

import epics
from epics import PV
import numpy as np
try:
    from numpy import datetime64, timedelta64
    def getdt(sec, nsec):
        return np.datetime64('{0}.{1}'.format(sec, nsec), 'ns')
except ImportError:
    from datetime import datetime, timedelta
    def getdt(sec, nsec):
        return datetime.utcfromtimestamp(sec + nsec / 1000000000.)
    def getdelta(mdelta):
        return timedelta(microseconds=mdelta)
    def isodt(dt):
        return dt.isoformat()

import numpy as np

MONITORED_OUTPUTS = {
    'VALA': 'Ticks per second',
    'VALB': 'Timestamp: Seconds',
    'VALC': 'Timestamp: Nanoseconds',
    'VALD': 'Number of events',
    'VALE': 'Index of first event',
    'VALF': 'Chunk #1',
    'VALG': 'Chunk #2',
    'VALH': 'Chunk #3',
    'VALI': 'Chunk #4',
    'VALJ': 'Chunk #5',
    'VALK': 'Chunk #6',
    'VALL': 'Chunk #7',
    'VALM': 'Chunk #8',
    'VALR': 'List of IDs',
    'VALS': 'List of names',
    'VALT': 'Ticks at the time of timestamp',
    'VALU': 'Record size (in uint32_t)'
    }

CHUNKSUFFS = "FGHIJKLM"
CHUNKS = set('VAL{0}'.format(x) for x in CHUNKSUFFS)

epics.ca.HAS_NUMPY = False

MAX_EVENTS = 4096

# NOTE: This list is valid for RTEMS 4.10. It may change across versions
# There's also STATES_READY = 0x0000, but we treat that one in a special way
RtemsState = namedtuple("RtemsState", "mask text")
RTEMS_STATES = [
    RtemsState(0x00001, "DORMANT"),
    RtemsState(0x00002, "SUSPENDED"),
    RtemsState(0x00004, "TRANSIENT"),
    RtemsState(0x00008, "DELAYING"),
    RtemsState(0x00010, "WAITING FOR TIME"),
    RtemsState(0x00020, "WAITING FOR BUFFER"),
    RtemsState(0x00040, "WAITING FOR SEGMENT"),
    RtemsState(0x00080, "WAITING FOR MESSAGE"),
    RtemsState(0x00100, "WAITING FOR EVENT"),
    RtemsState(0x00200, "WAITING FOR SEMAPHORE"),
    RtemsState(0x00400, "WAITING FOR MUTEX"),
    RtemsState(0x00800, "WAITING FOR CONDITION VARIABLE"),
    RtemsState(0x01000, "WAITING FOR JOIN AT EXIT"),
    RtemsState(0x02000, "WAITING FOR RPC REPLY"),
    RtemsState(0x04000, "WAITING FOR PERIOD"),
    RtemsState(0x08000, "WAITING FOR SIGNAL"),
    RtemsState(0x10000, "WAITING FOR BARRIER"),
    RtemsState(0x20000, "WAITING FOR RW LOCK")
]
rtems_states_map = dict(RTEMS_STATES)
status_masks = np.array([state.mask for state in RTEMS_STATES], dtype=np.uint32)

class RtemsStatsEvent(ctypes.Structure):
    _fields_ = [("misc", ctypes.c_uint32),
                ("state", ctypes.c_uint32),
                ("obj_id", ctypes.c_uint32),
                ("wait_id", ctypes.c_uint32),
                ("ticks", ctypes.c_uint32)]

    @property
    def ev_type(self):
        return self.misc & 0xFF

    @property
    def prio_current(self):
        return (self.misc & 0xFF00) >> 8

    @property
    def prio_real(self):
        return (self.misc & 0xFF0000) >> 16

    def status_text(self):
        bits = status_masks[status_masks & self.state != 0]
        return "READY" if len(bits) == 0 else (', '.join(rtems_states_map[mask] for mask in bits))

class EventPrinter(object):
    def __init__(self):
        self.trate = None
        self.last_timestamp = None
        self.tstamp_ticks = None
        self.last_tick = None
        self.sub_tick = 0
        self.prev_id = None

    def set_trate(self, ticks_per_second):
        # Microseconds per tick
        self.trate = getdelta(1000000.0 / ticks_per_second)

    def set_timestamp(self, tstamp, ticks):
        self.last_timestamp = tstamp
        self.tstamp_ticks = ticks

    def print_ev(self, event, t_mapping):
        if self.prev_id is None:
            self.prev_id = event.obj_id
            return

        ticks_since = event.ticks
        tstamp = self.last_timestamp + self.trate * (ticks_since - self.tstamp_ticks)

        if self.last_tick == ticks_since:
            self.sub_tick += 1
        else:
            self.sub_tick = 0

        self.last_tick = ticks_since
        st = event.status_text()
        state = st if event.wait_id == 0 else "{0}, {1:#08x}".format(st, event.wait_id)
        print "{stamp}: {name_a:20s} -> {name_b:20s} {pcur:3}/{preal:03} ({state})".format(
                id_a  = self.prev_id,
                id_b  = event.obj_id,
                name_a = t_mapping.get(self.prev_id, 'UNKNOWN'),
                name_b = t_mapping.get(event.obj_id, 'UNKNOWN'),
                stamp = isodt(tstamp),
                state = state,
                pcur  = ("---" if event.prio_current == event.prio_real else event.prio_current),
                preal = event.prio_real
                )
        self.prev_id = event.obj_id

class Buffer(object):
    def __init__(self):
        self.seq_no = None
        self.attributes = dict((x, None) for x in MONITORED_OUTPUTS)
        self._set_attributes = set()
        self.invalid = False

    def set_data(self, key, value):
        output = key.split('.')[-1]
        self.attributes[output] = value
        self._set_attributes.add(output)

    def done(self):
        return self._set_attributes == set(MONITORED_OUTPUTS)

    @property
    def timestamp(self):
        # Needs support from NumPy... only >= 1.7
        return getdt(self.attributes['VALB'], self.attributes['VALC'])

    @property
    def ticks_per_second(self):
        return self.attributes['VALA']

    @property
    def ticks_at_timestamp(self):
        return self.attributes['VALT']

    @property
    def number_of_events(self):
        return self.attributes['VALD']

    @property
    def longs_per_entry(self):
        return self.attributes['VALU']

    def dump(self, printer):
        events = self.number_of_events
        copyevents = self.number_of_events
        thread_map = dict((i, n if n != 'UNKNOWN' else "{0:#08x}".format(i)) for (i, n) in zip(self.attributes['VALR'], self.attributes['VALS']))
        thread_map[0x9010001] = 'IDLE'
        printer.set_trate(self.ticks_per_second)
        if events > 0:
            print "#events: {0}".format(events)
            # Size of each structure in bytes
            sizeof = self.longs_per_entry * 4
            data = (RtemsStatsEvent * events)()
            for offset, source in ((n * (512 * sizeof), self.attributes['VAL{0}'.format(x)]) for (n, x) in enumerate(CHUNKSUFFS)):
                block_size = (events if events < 512 else 512) * sizeof
                ctypes.memmove(ctypes.byref(data, offset), source, block_size)
                events -= 512
                if events < 1:
                    break
            printer.set_timestamp(self.timestamp, self.ticks_at_timestamp)
            for n in range(copyevents):
                printer.print_ev(data[n], thread_map)

class SessionTracker(object):
    def __init__(self, pvprefix, printer):
        self.prefix = pvprefix
        self.control = "{0}:control".format(pvprefix)
        self.main    = PV("{0}:export".format(pvprefix))
        self.outputs = [PV('{0}:export.{1}'.format(pvprefix, var), auto_monitor=epics.dbr.DBE_VALUE, callback=self.callback)
                        for var in MONITORED_OUTPUTS]
        self.buffers = defaultdict(Buffer)
        self.printer = printer

    def callback(self, pvname, value, count, status, timestamp, **kw):
        if status != 0:
            return
        try:
            buff = self.buffers[timestamp]
            buff.set_data(pvname, value)
            if buff.done():
                print "Dumping dataset with timestamp {0}, reported start at {1}".format(timestamp, buff.timestamp)
                buff.dump(self.printer)
        except KeyError:
            print "Setting {0} as invalid".format(timestamp)
            buff.invalid = True

    def enable(self, en):
        print "ENABLING" if en else "DISABLING"
        epics.PV("{0}.A".format(self.control)).put('ENABLE' if en else 'DISABLE')
        epics.PV("{0}.PROC".format(self.control)).put(1)

@contextmanager
def monitor_session(monitored):
    print "Creating the SessionTracker for PV: {0}".format(monitored)
    mon = SessionTracker(monitored, EventPrinter())
    try:
        yield mon
    finally:
        mon.enable(False)

def main(top):
    try:
        with monitor_session("{top}:rtems:stats".format(top=top)) as mon:
            mon.enable(True)
            while True:
                sleep(1)
    except KeyboardInterrupt:
        pass

if __name__=='__main__':
    import sys
    try:
        TOP = sys.argv[1]
    except IndexError:
        print "Pass the value for ${top}"
        sys.exit(1)

    main(TOP)
