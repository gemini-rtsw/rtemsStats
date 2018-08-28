#!/usr/bin/env python

# vim: ai:sw=4:sts=4:expandtab

import ctypes
import argparse
import os
import sys
from contextlib import contextmanager
from collections import defaultdict, namedtuple
from functools import partial
from time import sleep

import epics
from epics import PV
import numpy as np
from datetime import datetime, timedelta
try:
    from numpy import datetime64, timedelta64
    def getdt(sec, nsec):
        return datetime64(datetime.utcfromtimestamp(sec), 'ns') + timedelta64(nsec, 'ns')
    def getdelta(mdelta):
        return timedelta64(mdelta, 'ms')
    def isodt(dt):
        return str(dt)
except ImportError:
    def getdt(sec, nsec):
        return datetime.utcfromtimestamp(sec + nsec / 1000000000.)
    def getdelta(mdelta):
        return timedelta(microseconds=mdelta)
    def isodt(dt):
        return dt.isoformat()

DEBUG_LEVEL = 0

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
    'VALR': 'List of IDs',
    'VALS': 'List of names',
    'VALT': 'Ticks at the time of timestamp',
    'VALU': 'Record size (in uint32_t)',
    }

CHUNKSUFFS = "FGHIJK"
CHUNKS = set('VAL{0}'.format(x) for x in CHUNKSUFFS)

epics.ca.HAS_NUMPY = False

COLORS = {
    'red': '31',
    'bright_red': '31;1',
    'green': '32',
    'bright_green': '32;1',
    'yellow': '33',
    'bright_yellow': '33;1',
}

def colorize(text, color):
    return "\x1b[{0}m{1}\x1b[0m".format(COLORS[color], text)

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

# This class is abstract. Do not use
class RtemsStatsEvent(object):
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

class RtemsStatsEventTicks(ctypes.Structure, RtemsStatsEvent):
    _fields_ = [("misc", ctypes.c_uint32),
                ("state", ctypes.c_uint32),
                ("obj_id", ctypes.c_uint32),
                ("wait_id", ctypes.c_uint32),
                ("ticks", ctypes.c_uint32)]

class RtemsStatsEventTimestamp(ctypes.Structure, RtemsStatsEvent):
    _fields_ = [("misc", ctypes.c_uint32),
                ("state", ctypes.c_uint32),
                ("obj_id", ctypes.c_uint32),
                ("wait_id", ctypes.c_uint32),
                ("seconds", ctypes.c_uint32),
                ("nanoseconds", ctypes.c_uint32)]

    @property
    def timestamp(self):
        return getdt(self.seconds, self.nanoseconds)


class EventPrinter(object):
    def __init__(self, args, stamp_translator):
        self.prev_id = None
        self.args = args
        self.stampt = stamp_translator

    def set_trate(self, ticks_per_second):
        self.stampt.set_trate(ticks_per_second)

    def set_timestamp(self, *args):
        self.stampt.set_timestamp(*args)

    def print_ev(self, event, t_mapping):
        raise NotImplementedError("Please use a derivative class that implements this method...")


EPICS_PRIO_MAX = 99
EPICS_PRIO_MIN = 0

class ConsoleEventPrinter(EventPrinter):
    def __init__(self, *args, **kw):
        super(ConsoleEventPrinter, self).__init__(*args, **kw)
        self.is_terminal = os.isatty(sys.stdout.fileno())

    def get_prio(self, prio):
        if self.args.rtems_prio:
            return ' {0:03d}'.format(prio)

        epics_prio = 199 - prio
        if EPICS_PRIO_MIN <= epics_prio <= EPICS_PRIO_MAX:
            return ' {0:03d}'.format(epics_prio)

        ret = '*{0:03d}'.format(prio)
        return ret if not self.is_terminal else colorize(ret, 'yellow')

    def print_ev(self, event, t_mapping):
        if self.prev_id is None:
            self.prev_id = event.obj_id
            return

        tstamp = self.stampt.get_timestamp(event)

        st = event.status_text()
        state = st if event.wait_id == 0 else "{0}, {1:#08x}".format(st, event.wait_id)
        prio_current, prio_real = self.get_prio(event.prio_current), self.get_prio(event.prio_real)
        print "{stamp}: {name_a:20s} -> {name_b:20s} {pcur}/{preal} ({state})".format(
                id_a  = self.prev_id,
                id_b  = event.obj_id,
                name_a = t_mapping.get(self.prev_id, 'UNKNOWN'),
                name_b = t_mapping.get(event.obj_id, 'UNKNOWN'),
                stamp = isodt(tstamp),
                state = state,
                pcur  = (" ---" if prio_current == prio_real else prio_current),
                preal = prio_real
                )
        self.prev_id = event.obj_id

class CsvEventPrinter(EventPrinter):
    pass

class TimestampTranslator(object):
    def set_trate(self, *args):
        pass

    def set_timestamp(self, *args):
        pass

    def get_timestamp(self, event):
        return event.timestamp

class TicksTranslator(object):
    def __init__(self):
        self.trate = None
        self.last_timestamp = None
        self.tstamp_ticks = None
        self.last_tick = None
        self.sub_tick = 0

    def set_trate(self, ticks_per_second):
        # Microseconds per tick
        self.trate = getdelta(1000000.0 / ticks_per_second)

    def set_timestamp(self, tstamp, ticks):
        self.last_timestamp = tstamp
        self.tstamp_ticks = ticks

    def get_timestamp(self, event):
        ticks_since = event.ticks
        tstamp = self.last_timestamp + self.trate * (ticks_since - self.tstamp_ticks)

        if self.last_tick == ticks_since:
            self.sub_tick += 1
        else:
            self.sub_tick = 0
        self.last_tick = ticks_since

        return tstamp

format_dict = {
    'console': ConsoleEventPrinter,
    'csv': CsvEventPrinter
}

def printerFactory(args, info):
    stamp_translator_class = TimestampTranslator if (info & 0x01) else TicksTranslator

    try:
        return format_dict[args.fmt](args, stamp_translator_class())
    except KeyError:
        raise ValueError("Unknown format: {0}".format(args.gmt))

class Buffer(object):
    def __init__(self, event_class):
        self.seq_no = None
        self.attributes = dict((x, None) for x in MONITORED_OUTPUTS)
        self._set_attributes = set()
        self.invalid = False
        self.evCls = event_class

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
            if DEBUG_LEVEL > 0:
                print "#events: {0}".format(events)
            # Size of each structure in bytes
            sizeof = self.longs_per_entry * 4
            max_chunk_size = 16000             # 4000 * 4
            total_bytes = events * sizeof
            data = (self.evCls * events)()
            for offset, source in (((n * max_chunk_size), self.attributes['VAL{0}'.format(x)]) for (n, x) in enumerate(CHUNKSUFFS)):
                block_size = max_chunk_size if total_bytes > max_chunk_size else total_bytes
                ctypes.memmove(ctypes.byref(data, offset), source, block_size)
                total_bytes -= max_chunk_size
                if total_bytes < 1:
                    break
            printer.set_timestamp(self.timestamp, self.ticks_at_timestamp)
            for n in range(copyevents):
                printer.print_ev(data[n], thread_map)

class SessionTracker(object):
    def __init__(self, pvprefix):
        self.buffers = None
        self.printer = None
        self.prefix = pvprefix
        self.control = "{0}:control".format(pvprefix)
        self.main    = PV("{0}:export".format(pvprefix))
        self.outputs = [PV('{0}:export.{1}'.format(pvprefix, var), auto_monitor=epics.dbr.DBE_VALUE, callback=self.callback)
                        for var in MONITORED_OUTPUTS]

    def _get_pv_var(self, var_name):
        return epics.PV("{0}.{1}".format(self.control, var_name))

    def _process_pv(self):
        self._get_pv_var('PROC').put(1)

    def get_info(self):
        self._send_control('INFO')
        return self._get_pv_var('VALC').get()

    def _send_control(self, command):
        self._get_pv_var('A').put(command)
        self._process_pv()

    def callback(self, pvname, value, count, status, timestamp, **kw):
        if status != 0 or self.buffers is None or self.printer is None:
            return
        try:
            buff = self.buffers[timestamp]
            buff.set_data(pvname, value)
            if buff.done():
                if DEBUG_LEVEL > 0:
                    print "Dumping dataset with timestamp {0}, reported start at {1}".format(timestamp, buff.timestamp)
                buff.dump(self.printer)
        except KeyError:
            print "Setting {0} as invalid".format(timestamp)
            buff.invalid = True

    def set_buffer_class(self, cls):
        self.buffers = defaultdict(partial(Buffer, cls))

    def enable(self, en):
        if DEBUG_LEVEL > 0:
            print "ENABLING" if en else "DISABLING"
        self._send_control('ENABLE' if en else 'DISABLE')

@contextmanager
def monitor_session(args, monitored):
    if DEBUG_LEVEL > 0:
        print "Creating the SessionTracker for PV: {0}".format(monitored)
    mon = SessionTracker(monitored)
    info = mon.get_info()
    mon.set_buffer_class(RtemsStatsEventTimestamp if (info & 0x01) else RtemsStatsEventTicks)
    mon.printer = printerFactory(args, info)

    def _get_evt_classes(self):
        self._send_control('INFO')
        mask = self._get_pv_var('VALC').get()
        if (mask & 0x01) == 1:
            return RtemsStatsEventTimestamp, EventPrinterTimestamp
        else:
            return RtemsStatsEventTicks, EventPrinterTicks
    try:
        yield mon
    finally:
        mon.enable(False)

def main(args):
    try:
        with monitor_session(args, "{top}:rtems:stats".format(top=args.top)) as mon:
            mon.enable(True)
            while True:
                sleep(1)
    except KeyboardInterrupt:
        pass

def parse_args():
    parser = argparse.ArgumentParser(description = "RTEMS/EPICS Monitor")
    parser.add_argument('-v', dest='verbose', action='store_true',
                        help='More verbose output')
    parser.add_argument('-r', dest='rtems_prio', action='store_true',
                        help='Display RTEMS priorities (default is to show EPICS ones). Does not affect all output types')
    parser.add_argument('--format', dest='fmt', default='console', choices=['console', 'csv'],
                        help='Output format')
    parser.add_argument('top', help='Top of the database, as in {top}:rtems:stats')

    return parser.parse_args()

if __name__=='__main__':

    args = parse_args()
    if args.verbose:
        DEBUG_LEVEL = 10
    main(args)
