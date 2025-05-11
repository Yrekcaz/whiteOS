#!/usr/bin/env bash
set -eu

# Where is this script?
SCRIPTDIR=$(dirname $([ -L $0 ] && echo "$(dirname $0)/$(readlink -n $0)" || echo $0))           

# What do we want to profile?
: ${ANKI_PROFILE_PROCNAME:="vic-engine"}

# How long do we capture? (in seconds)
: ${ANKI_PROFILE_DURATION:="10"}

# How often do we sample? (in usec)
: ${ANKI_PROFILE_FREQUENCY:="4000"}

# Where is symbol cache?
: ${ANKI_PROFILE_SYMBOLCACHE:="${SCRIPTDIR}/symbol_cache"}

# Where is perf.data?
: ${ANKI_PROFILE_PERFDATA:="${SCRIPTDIR}/${ANKI_PROFILE_PROCNAME}/perf.data"}
mkdir -p ${SCRIPTDIR}/${ANKI_PROFILE_PROCNAME}

# Where is top level?
: ${TOPLEVEL:="`git rev-parse --show-toplevel`"}

# Where is simpleperf?
: ${SIMPLEPERF:="${TOPLEVEL}/lib/util/tools/simpleperf"}

# Where is adb?
: ${ADB:="adb"}

#
# Create symbol cache
#
if [ ! -d ${ANKI_PROFILE_SYMBOLCACHE} ] ; then
  bash ${SCRIPTDIR}/make_symbol_cache.sh ${ANKI_PROFILE_SYMBOLCACHE}
fi

#
# Remount /data to allow execution
#
${ADB} shell "mount /data -o remount,exec"

#
# Run app_profiler.py to start profiling.
# When it finishes it will pull a `perf.data` file off the robot.
#
# Use '-nc' because we don't need to recompile JNI.
# Use '-nb' because we use the symbol cache instead of binaries from the device.
# Use '-np' and '-r' to set collection parameters.
# Use '-lib' to fetch symbols from cache.
#
PROFILER=${SIMPLEPERF}/app_profiler.py

python ${PROFILER} -nc -nb \
  -np ${ANKI_PROFILE_PROCNAME} \
  -r "-e cpu-cycles:u -f ${ANKI_PROFILE_FREQUENCY} --duration ${ANKI_PROFILE_DURATION} --call-graph dwarf" \
  -lib ${ANKI_PROFILE_SYMBOLCACHE} \
  -o ${ANKI_PROFILE_PERFDATA}

#
# Remount /data without execution
#
${ADB} shell "mount /data -o remount,noexec"

#
# To view perf.data, run 
#  simpleperf report --symfs symbol_cache
# which will print performance stuff to console. 
#

export PATH=${SIMPLEPERF}/bin/darwin/x86_64:${PATH}
simpleperf report \
  -i ${ANKI_PROFILE_PERFDATA} \
  --symfs ${ANKI_PROFILE_SYMBOLCACHE} $@


