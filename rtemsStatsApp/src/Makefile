TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE

#=============================
# Build a support module called <APPNAME>

LIBRARY_IOC_RTEMS += rtemsStats

# <APPNAME>.dbd will be created and installed
DBD += rtemsStats.dbd

# rtemsStats.dbd will be made up from these files:
#rtemsStats_DBD += base.dbd
#rtemsStats_DBD += menuScan.dbd
#rtemsStats_DBD += genSubRecord.dbd
rtemsStats_DBD += rtemsStatsCore.dbd

# specify all source files to be compiled
# including sequencer (.st) source files
rtemsStats_SRCS += stats.c
# rtemsStats_SRCS += rtems_config.c

#=============================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE
