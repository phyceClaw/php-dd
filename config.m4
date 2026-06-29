PHP_ARG_ENABLE([dd],
  [whether to enable dd support],
  [AS_HELP_STRING([--enable-dd],
    [Enable native dd()/dump() (Laravel-style dump-and-die)])],
  [no])

if test "$PHP_DD" != "no"; then
  AC_DEFINE(HAVE_DD, 1, [ Have dd support ])
  PHP_NEW_EXTENSION(dd, dd.c, $ext_shared)
fi
