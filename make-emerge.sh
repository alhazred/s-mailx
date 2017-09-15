#!/bin/sh -
#@ Out-of-tree compilation support, à la
#@    $ cd /tmp && mkdir build && cd build &&
#@       ~/src/nail.git/make-emerge.sh && make tangerine DESTDIR=.ddir
# Public Domain

# See make-config.sh, the source of all this!

( set -o noglob ) >/dev/null 2>&1 && noglob_shell=1 || unset noglob_shell

config_exit() {
   exit ${1}
}

msg() {
   fmt=${1}
   shift
   printf >&2 -- "${fmt}\\n" "${@}"
}

# which(1) not standardized, command(1) -v may return non-executable: unroll!
acmd_test() { __acmd "${1}" 1 0 0; }
acmd_test_fail() { __acmd "${1}" 1 1 0; }
acmd_set() { __acmd "${2}" 0 0 0 "${1}"; }
acmd_set_fail() { __acmd "${2}" 0 1 0 "${1}"; }
acmd_testandset() { __acmd "${2}" 1 0 0 "${1}"; }
acmd_testandset_fail() { __acmd "${2}" 1 1 0 "${1}"; }
thecmd_set() { __acmd "${2}" 0 0 1 "${1}"; }
thecmd_set_fail() { __acmd "${2}" 0 1 1 "${1}"; }
thecmd_testandset() { __acmd "${2}" 1 0 1 "${1}"; }
thecmd_testandset_fail() { __acmd "${2}" 1 1 1 "${1}"; }
__acmd() {
   pname=${1} dotest=${2} dofail=${3} verbok=${4} varname=${5}

   if [ "${dotest}" -ne 0 ]; then
      eval dotest=\$${varname}
      if [ -n "${dotest}" ]; then
         [ -n "${VERBOSE}" ] && [ ${verbok} -ne 0 ] &&
            msg ' . ${%s} ... %s' "${pname}" "${dotest}"
         return 0
      fi
   fi

   oifs=${IFS} IFS=:
   [ -n "${noglob_shell}" ] && set -o noglob
   set -- ${PATH}
   [ -n "${noglob_shell}" ] && set +o noglob
   IFS=${oifs}
   for path
   do
      if [ -z "${path}" ] || [ "${path}" = . ]; then
         if [ -d "${PWD}" ]; then
            path=${PWD}
         else
            path=.
         fi
      fi
      if [ -f "${path}/${pname}" ] && [ -x "${path}/${pname}" ]; then
         [ -n "${VERBOSE}" ] && [ ${verbok} -ne 0 ] &&
            msg ' . ${%s} ... %s' "${pname}" "${path}/${pname}"
         [ -n "${varname}" ] && eval ${varname}="${path}/${pname}"
         return 0
      fi
   done

   # We may have no builtin string functions, we yet have no programs we can
   # use, try to access once from the root, assuming it is an absolute path if
   # that finds the executable
   if ( cd && [ -f "${pname}" ] && [ -x "${pname}" ] ); then
     [ -n "${VERBOSE}" ] && [ ${verbok} -ne 0 ] &&
            msg ' . ${%s} ... %s' "${pname}" "${pname}"
      [ -n "${varname}" ] && eval ${varname}="${pname}"
      return 0
   fi

   [ ${dofail} -eq 0 ] && return 1
   msg 'ERROR: no trace of utility '"${pname}"
   exit 1
}

syno() {
   if [ ${#} -gt 0 ]; then
      echo >&2 "ERROR: ${*}"
      echo >&2
   fi
   echo >&2 'Synopsis: SOURCEDIR/make-emerge.sh [from within target directory]'
   exit 1
}

[ ${#} -eq 0 ] || syno

# Rude simple, we should test for Solaris, but who runs this script?
if [ -d /usr/xpg4 ]; then
   PATH=/usr/xpg4/bin:${PATH}
fi

thecmd_testandset_fail awk awk
thecmd_testandset_fail cp cp
thecmd_testandset_fail dirname dirname
thecmd_testandset_fail pwd pwd

srcdir=`${dirname} ${0}`
blddir=`${pwd}`
if [ "${srcdir}" = . ]; then
   msg 'This is not out of tree?!'
   config_exit 1
fi
echo 'Initializing out-of-tree build.'
echo 'Source directory: '"${srcdir}"
echo 'Build directory : '"${blddir}"
srcdir="${srcdir}"/

${awk} -v srcdir="${srcdir}" -v blddir="${blddir}" '
   {
      gsub(/^SRCDIR=\.\/$/, "SRCDIR=" srcdir)
      print
   }
   ' < "${srcdir}"makefile > ./makefile

${cp} "${srcdir}"config.h ./
${cp} "${srcdir}"gen-version.h ./
${cp} "${srcdir}"make.rc ./

echo 'You should now be able to proceed as normal (e.g., "$ make all")'

# s-sh-mode
