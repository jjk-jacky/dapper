#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([dapper], [0.1.2], [i.am.jack.mail@gmail.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_SRCDIR([main.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([stdlib.h string.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_CHECK_FUNCS([memmove memset strchr strdup strstr])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

echo "
        ${PACKAGE} version ${PACKAGE_VERSION}

 Build information:
   source code location         : ${srcdir}
   prefix                       : ${prefix}

 Install paths:
   binaries                     : $(eval echo $(eval echo ${bindir}))
   documentation                : $(eval echo $(eval echo ${docdir}))
   man pages                    : $(eval echo $(eval echo ${mandir}))

"
