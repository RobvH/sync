dnl $Id$
dnl config.m4 for extension sync

PHP_ARG_ENABLE(sync, whether to enable synchronization object support (--enable-sync),
  [  --enable-sync           Enable synchronization object support])

if test "$PHP_SYNC" != "no"; then
  dnl # Check for sem_open() support.
  AC_MSG_CHECKING([for sem_open in -pthread -lrt])

  SAVED_LIBS="$LIBS"

  dnl on OS X this does not need to be linked against the rt lib
  case $build_os in
    darwin1*.*.*)
      LIBS="$LIBS -pthread"
      ;;
    *)
      LIBS="$LIBS -pthread -lrt"
      PHP_ADD_LIBRARY(rt,,SYNC_SHARED_LIBADD)
      ;;
  esac

  AC_TRY_LINK([
    #include <fcntl.h>
    #include <semaphore.h>
  ], [
    sem_t *MxSemMutex = sem_open("", O_CREAT, 0666, 1);
  ], [
    have_sem_open=yes
    AC_MSG_RESULT([yes])
  ], [
    AC_MSG_ERROR([sem_open() is not available on this platform])
  ])

  PHP_SUBST(SYNC_SHARED_LIBADD)

  dnl # Finish defining the basic extension support.
  AC_DEFINE(HAVE_SYNC, 1, [Whether you have synchronization object support])
  PHP_NEW_EXTENSION(sync, sync.c, $ext_shared)
fi
