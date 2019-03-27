#!/bin/sh -
#@ Out-of-tree compilation support, à la
#@    $ cd /tmp && mkdir build && cd build &&
#@       ~/src/nail.git/make-emerge.sh && make tangerine DESTDIR=.ddir
# Public Domain

## Upon interest see mk/make-config.sh, the source of all this!

# For heaven's sake auto-redirect on SunOS/Solaris
if [ "x${SHELL}" = x ] || [ "${SHELL}" = /bin/sh ] && \
      [ -f /usr/xpg4/bin/sh ] && [ -x /usr/xpg4/bin/sh ]; then
   SHELL=/usr/xpg4/bin/sh
   export SHELL
   exec /usr/xpg4/bin/sh "${0}" "${@}"
fi
[ -n "${SHELL}" ] || SHELL=/bin/sh
export SHELL


config_exit() {
   exit ${1}
}

syno() {
   if [ ${#} -gt 0 ]; then
      echo >&2 "ERROR: ${*}"
      echo >&2
   fi
   echo >&2 'Synopsis: SOURCEDIR/make-emerge.sh [from within target directory]'
   exit 1
}

oneslash() {
   </dev/null ${awk} -v X="${1}" '
      BEGIN{
         i = match(X, "/+$")
         if(RSTART != 0)
            X = substr(X, 1, RSTART - 1)
         X = X "/"
         print X
      }
   '
}

[ ${#} -eq 0 ] || syno

SU_FIND_COMMAND_INCLUSION=1 . ${0%/*}/mk/su-find-command.sh

# Rude simple, we should test for Solaris, but who runs this script?
if [ -d /usr/xpg4 ]; then
   PATH=/usr/xpg4/bin:${PATH}
fi

thecmd_testandset_fail awk awk
thecmd_testandset_fail cp cp
thecmd_testandset_fail dirname dirname
thecmd_testandset_fail mkdir mkdir
thecmd_testandset_fail pwd pwd

topdir=`${dirname} ${0}`
if [ "${topdir}" = . ]; then
   msg 'This is not out of tree?!'
   config_exit 1
fi
topdir=`cd ${topdir}; oneslash "\`${pwd}\`"`
blddir=`oneslash "\`${pwd}\`"`
echo 'Initializing out-of-tree build.'
echo 'Source directory: '"${topdir}"
echo 'Build directory : '"${blddir}"

set -e
${mkdir} -p include/mx
${cp} "${topdir}"mx-config.h ./
${awk} -v topdir="${topdir}" -v blddir="${blddir}" '
   /^CWDDIR=.*$/{ print "CWDDIR=" blddir; next}
   /^TOPDIR=.*$/{ print "TOPDIR=" topdir; next}
   /^OBJDIR=.*$/{ print "OBJDIR=" blddir ".obj"; next}
   {print}
   ' < "${topdir}"makefile > ./makefile
${cp} "${topdir}"make.rc ./
${cp} "${topdir}"mime.types ./
${cp} "${topdir}"include/mx/gen-version.h include/mx/
set +e

echo 'You should now be able to proceed as normal (e.g., "$ make all")'

# s-sh-mode
