/* w32-pth.c - GNU Pth emulation for W32 (MS Windows).
 * Copyright (c) 1999-2003 Ralf S. Engelschall <rse@engelschall.com>
 * Copyright (C) 2004 g10 Code GmbH
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 * ------------------------------------------------------------------
 * This code is based on Ralf Engelschall's GNU Pth, a non-preemptive
 * thread scheduling library which can be found at
 * http://www.gnu.org/software/pth/.  MS Windows (W32) specific code
 * written by Timo Schulz, g10 Code.
 */

#include <config.h>
#ifdef HAVE_W32_SYSTEM
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <signal.h>

/* We don't want to have any Windows specific code in the header, thus
   we use a macro which defaults to a compatible type in w32-pth.h. */
#define W32_PTH_HANDLE_INTERNAL  HANDLE
#include "w32-pth.h"


static int pth_initialized = 0;

/* Variables to support event handling. */
static int pth_signo = 0;
static HANDLE pth_signo_ev = NULL;

/* Mutex to make sure only one thread is running. */
static CRITICAL_SECTION pth_shd;


#define implicit_init() do { if (!pth_initialized) pth_init(); } while (0)



struct pth_event_s
{
  struct pth_event_s * next;
  struct pth_event_s * prev;
  HANDLE hd;
  union
  {
    struct sigset_s * sig;
    int               fd;
    struct timeval    tv;
    pth_mutex_t     * mx;
  } u;
  int * val;
  int u_type;
  int flags;
};


struct pth_attr_s 
{
  unsigned int flags;
  unsigned int stack_size;
  char * name;
};


struct _pth_priv_hd_s
{
    void *(*thread)(void *);
    void * arg;
};


/* Prototypes.  */
static pth_event_t do_pth_event (unsigned long spec, ...);
static unsigned int do_pth_waitpid (unsigned pid, int * status, int options);
static int do_pth_wait (pth_event_t ev);
static int do_pth_event_status (pth_event_t ev);
static void * helper_thread (void * ctx);




int
pth_init (void)
{
  SECURITY_ATTRIBUTES sa;
  WSADATA wsadat;
  
  fprintf (stderr, "pth_init: called.\n");
  pth_initialized = 1;
  if (WSAStartup (0x202, &wsadat))
    abort ();
  pth_signo = 0;
  InitializeCriticalSection (&pth_shd);
  if (pth_signo_ev)
    CloseHandle (pth_signo_ev);
  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;
  pth_signo_ev = CreateEvent (&sa, TRUE, FALSE, NULL);
  if (!pth_signo_ev)
    abort ();
  EnterCriticalSection (&pth_shd);
  return 0;
}


int
pth_kill (void)
{
  pth_signo = 0;
  if (pth_signo_ev)
    CloseHandle (pth_signo_ev);
  DeleteCriticalSection (&pth_shd);
  WSACleanup ();
  pth_initialized = 0;
  return 0;
}


static void
enter_pth (const char *function)
{
  /* Fixme: I am not sure whether the same thread my enter a critical
     section twice.  */
  fprintf (stderr, "enter_pth (%s)\n", function? function:"");
  LeaveCriticalSection (&pth_shd);
}


static void
leave_pth (const char *function)
{
  EnterCriticalSection (&pth_shd);
  fprintf (stderr, "leave_pth (%s)\n", function? function:"");
}


long 
pth_ctrl (unsigned long query, ...)
{
  implicit_init ();

  switch (query)
    {
    case PTH_CTRL_GETAVLOAD:
    case PTH_CTRL_GETPRIO:
    case PTH_CTRL_GETNAME:
    case PTH_CTRL_GETTHREADS_NEW:
    case PTH_CTRL_GETTHREADS_READY:
    case PTH_CTRL_GETTHREADS_RUNNING:
    case PTH_CTRL_GETTHREADS_WAITING:
    case PTH_CTRL_GETTHREADS_SUSPENDED:
    case PTH_CTRL_GETTHREADS_DEAD:
    case PTH_CTRL_GETTHREADS:
    default:
      return -1;
    }
  return 0;
}



pth_time_t
pth_timeout (long sec, long usec)
{
  pth_time_t tvd;

  tvd.tv_sec  = sec;
  tvd.tv_usec = usec;    
  return tvd;
}


int
pth_read_ev (int fd, void *buffer, size_t size, pth_event_t ev)
{
  implicit_init ();
  return 0;
}


int
pth_read (int fd,  void * buffer, size_t size)
{
  int n;

  implicit_init ();
  enter_pth (__FUNCTION__);

  n = recv (fd, buffer, size, 0);
  if (n == -1 && WSAGetLastError () == WSAENOTSOCK)
    {
      DWORD nread = 0;
      n = ReadFile ((HANDLE)fd, buffer, size, &nread, NULL);
      if (!n)
        n = -1;
      else
        n = (int)nread;
    }
  leave_pth (__FUNCTION__);
  return n;
}


int
pth_write_ev (int fd, const void *buffer, size_t size, pth_event_t ev)
{
  implicit_init ();
  return 0;
}


int
pth_write (int fd, const void * buffer, size_t size)
{
  int n;

  implicit_init ();
  enter_pth (__FUNCTION__);
  n = send (fd, buffer, size, 0);
  if (n == -1 && WSAGetLastError () == WSAENOTSOCK)
    {
      char strerr[256];
  
      FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM, NULL, (int)GetLastError (),
                     MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
                     strerr, sizeof (strerr)-1, NULL);
      fprintf (stderr, "pth_write(%d) failed in send: %s\n", fd, strerr);

 
      DWORD nwrite;
      n = WriteFile ((HANDLE)fd, buffer, size, &nwrite, NULL);
      if (!n)
        {
          FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM, NULL,(int)GetLastError (),
                         MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
                         strerr, sizeof (strerr)-1, NULL);
          fprintf (stderr, "pth_write(%d) failed in write: %s\n", fd, strerr);
          n = -1;
        }
      else
        n = (int)nwrite;
    }
  leave_pth (__FUNCTION__);
  return n;
}


int
pth_select (int nfds, fd_set * rfds, fd_set * wfds, fd_set * efds,
	    const struct timeval * timeout)
{
  int n;

  implicit_init ();
  enter_pth (__FUNCTION__);
  n = select (nfds, rfds, wfds, efds, timeout);
  leave_pth (__FUNCTION__);
  return n;
}


int
pth_fdmode (int fd, int mode)
{
  unsigned long val;
  int ret = PTH_FDMODE_BLOCK;

  implicit_init ();
  /* Note: we don't do the eter/leave pth here because this is for one
     a fast fucntion and secondly already called from inside such a
     block.  */
  /* XXX: figure out original fd mode */
  switch (mode)
    {
    case PTH_FDMODE_NONBLOCK:
      val = 1;
      if (ioctlsocket (fd, FIONBIO, &val) == SOCKET_ERROR)
        ret = PTH_FDMODE_ERROR;
      break;

    case PTH_FDMODE_BLOCK:
      val = 0;
      if (ioctlsocket (fd, FIONBIO, &val) == SOCKET_ERROR)
        ret = PTH_FDMODE_ERROR;
      break;
    }
  return ret;
}


int
pth_accept (int fd, struct sockaddr *addr, int *addrlen)
{
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);
  rc = accept (fd, addr, addrlen);
  leave_pth (__FUNCTION__);
  return rc;
}


int
pth_accept_ev (int fd, struct sockaddr *addr, int *addrlen,
               pth_event_t ev_extra)
{
  pth_key_t ev_key;
  pth_event_t ev;
  int rv;
  int fdmode;

  implicit_init ();
  enter_pth (__FUNCTION__);

  fdmode = pth_fdmode (fd, PTH_FDMODE_NONBLOCK);
  if (fdmode == PTH_FDMODE_ERROR)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }

  ev = NULL;
  while ((rv = accept (fd, addr, addrlen)) == -1 && 
         (WSAGetLastError () == WSAEINPROGRESS || 
          WSAGetLastError () == WSAEWOULDBLOCK))
    {
      if (ev == NULL) {
        ev = do_pth_event (PTH_EVENT_FD|PTH_UNTIL_FD_READABLE|PTH_MODE_STATIC,
                           &ev_key, fd);
        if (ev == NULL)
          {
            leave_pth (__FUNCTION__);
            return -1;
          }
        if (ev_extra != NULL)
          pth_event_concat (ev, ev_extra, NULL);
      }
      /* Wait until accept has a chance. */
      do_pth_wait (ev);
      if (ev_extra != NULL)
        {
          pth_event_isolate (ev);
          if (do_pth_event_status (ev) != PTH_STATUS_OCCURRED)
            {
              pth_fdmode (fd, fdmode);
              leave_pth (__FUNCTION__);
              return -1;
            }
        }
    }

  pth_fdmode (fd, fdmode);
  leave_pth (__FUNCTION__);
  return rv;   
}


int
pth_connect (int fd, struct sockaddr *name, int namelen)
{
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);
  rc = connect (fd, name, namelen);
  leave_pth (__FUNCTION__);
  return rc;
}


int
pth_mutex_release (pth_mutex_t *hd)
{
  if (!hd)
    return -1;
  implicit_init ();
  enter_pth (__FUNCTION__);
  if (hd->mx)
    {
      CloseHandle (hd->mx);
      hd->mx = NULL;
    }
  free (hd);
  leave_pth (__FUNCTION__);
  return 0;
}


int
pth_mutex_acquire (pth_mutex_t *hd, int tryonly, pth_event_t ev_extra)
{
  implicit_init ();
  enter_pth (__FUNCTION__);

  if (!hd || !hd->mx)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }
    
#if 0
  /* still not locked, so simply acquire mutex? */
  if (!(mutex->mx_state & PTH_MUTEX_LOCKED))
    {
      mutex->mx_state |= PTH_MUTEX_LOCKED;
      mutex->mx_count = 1;
      pth_ring_append(&(pth_current->mutexring), &(mutex->mx_node));
      pth_debug1("pth_mutex_acquire: immediately locking mutex");
      return 0;
    }

  /* already locked by caller? */
  if (mutex->mx_count >= 1 && mutex->mx_owner == pth_current)
    {
      /* recursive lock */
      mutex->mx_count++;
      pth_debug1("pth_mutex_acquire: recursive locking");
      return 0;
    }

  if (tryonly)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }

  for (;;)
    {
      ev = pth_event(PTH_EVENT_MUTEX|PTH_MODE_STATIC, &ev_key, mutex);
      if (ev_extra != NULL)
        pth_event_concat (ev, ev_extra, NULL);
      pth_wait (ev);
      if (ev_extra != NULL) {
        pth_event_isolate (ev);
        if (do_pth_event_status(ev) == PTH_STATUS_PENDING)
          {
            leave_pth (__FUNCTION__);
            return -1;
          }
      }
      if (!(mutex->mx_state & PTH_MUTEX_LOCKED))
        break;
    }
#endif

  hd->mx_state |= PTH_MUTEX_LOCKED;
  leave_pth (__FUNCTION__);
  return 0;
}


int
pth_mutex_init (pth_mutex_t *hd)
{
  SECURITY_ATTRIBUTES sa;
  
  implicit_init ();
  enter_pth (__FUNCTION__);

  if (hd->mx)
    {
      ReleaseMutex (hd->mx);
      CloseHandle (hd->mx);
    }
  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;
  hd->mx = CreateMutex (&sa, FALSE, NULL);
  hd->mx_state = PTH_MUTEX_INITIALIZED;

  leave_pth (__FUNCTION__);
  return 0;
}


pth_attr_t
pth_attr_new (void)
{
  pth_attr_t hd;

  implicit_init ();
  hd = calloc (1, sizeof *hd);
  return hd;
}


int
pth_attr_destroy (pth_attr_t hd)
{
  if (!hd)
    return -1;
  implicit_init ();
  if (hd->name)
    free (hd->name);
  free (hd);
  return 0;
}


int
pth_attr_set (pth_attr_t hd, int field, ...)
{    
  va_list args;
  char * str;
  int val;
  int rc = 0;

  implicit_init ();

  va_start (args, field);
  switch (field)
    {
    case PTH_ATTR_JOINABLE:
      val = va_arg (args, int);
      if (val)
        {
          hd->flags |= PTH_ATTR_JOINABLE;
          fprintf (stderr, "pth_attr_set: PTH_ATTR_JOINABLE\n");
        }
      break;

    case PTH_ATTR_STACK_SIZE:
      val = va_arg (args, int);
      if (val)
        {
          hd->flags |= PTH_ATTR_STACK_SIZE;
          hd->stack_size = val;
          fprintf (stderr, "pth_attr_set: PTH_ATTR_STACK_SIZE %d\n", val);
        }
      break;

    case PTH_ATTR_NAME:
      str = va_arg (args, char*);
      if (hd->name)
        free (hd->name);
      if (str)
        {
          hd->name = strdup (str);
          if (!hd->name)
            return -1;
          hd->flags |= PTH_ATTR_NAME;
          fprintf (stderr, "pth_attr_set: PTH_ATTR_NAME %s\n", hd->name);
        }
      break;

    default:
      rc = -1;
      break;
    }
  va_end (args);
  return rc;
}


static pth_t
do_pth_spawn (pth_attr_t hd, void *(*func)(void *), void *arg)
{
  SECURITY_ATTRIBUTES sa;
  DWORD tid;
  HANDLE th;
  struct _pth_priv_hd_s * ctx;

  if (!hd)
    return NULL;

  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;

  ctx = calloc (1, sizeof * ctx);
  if (!ctx)
    return NULL;
  ctx->thread = func;
  ctx->arg = arg;

  /* XXX: we don't use all thread attributes. */

  fprintf (stderr, "do_pth_spawn creating thread ...\n");
  th = CreateThread (&sa, hd->stack_size,
                     (LPTHREAD_START_ROUTINE)helper_thread,
                     ctx, 0, &tid);
  fprintf (stderr, "do_pth_spawn created thread %p\n", th);

  return th;
}

pth_t
pth_spawn (pth_attr_t hd, void *(*func)(void *), void *arg)
{
  HANDLE th;

  if (!hd)
    return NULL;

  implicit_init ();
  enter_pth (__FUNCTION__);
  th = do_pth_spawn (hd, func, arg);
  leave_pth (__FUNCTION__);
  return th;
}


int
pth_join (pth_t hd, void **value)
{
  return 0;
}


/* friendly */
int
pth_cancel (pth_t hd)
{
  if (!hd)
    return -1;
  implicit_init ();
  enter_pth (__FUNCTION__);
  WaitForSingleObject (hd, 1000);
  TerminateThread (hd, 0);
  leave_pth (__FUNCTION__);
  return 0;
}


/* cruel */
int
pth_abort (pth_t hd)
{
  if (!hd)
    return -1;
  implicit_init ();
  enter_pth (__FUNCTION__);
  TerminateThread (hd, 0);
  leave_pth (__FUNCTION__);
  return 0;
}


void
pth_exit (void *value)
{
  implicit_init ();
  enter_pth (__FUNCTION__);
  pth_kill ();
  leave_pth (__FUNCTION__);
  exit ((int)(long)value);
}


static unsigned int
do_pth_waitpid (unsigned pid, int * status, int options)
{
#if 0
  pth_event_t ev;
  static pth_key_t ev_key = PTH_KEY_INIT;
  pid_t pid;

  pth_debug2("pth_waitpid: called from thread \"%s\"", pth_current->name);

  for (;;)
    {
      /* do a non-blocking poll for the pid */
      while (   (pid = pth_sc(waitpid)(wpid, status, options|WNOHANG)) < 0
                && errno == EINTR)
        ;

      /* if pid was found or caller requested a polling return immediately */
      if (pid == -1 || pid > 0 || (pid == 0 && (options & WNOHANG)))
        break;

      /* else wait a little bit */
      ev = pth_event(PTH_EVENT_TIME|PTH_MODE_STATIC, &ev_key,
                     pth_timeout (0,250000));
      pth_wait(ev);
    }

  pth_debug2("pth_waitpid: leave to thread \"%s\"", pth_current->name);
#endif
  return 0;
}


unsigned int
pth_waitpid (unsigned pid, int * status, int options)
{
  unsigned int n;

  implicit_init ();
  enter_pth (__FUNCTION__);
  n = do_pth_waitpid (pid, status, options);
  leave_pth (__FUNCTION__);
  return n;
}


static BOOL WINAPI
sig_handler (DWORD signo)
{
  switch (signo)
    {
    case CTRL_C_EVENT:     pth_signo = SIGINT; break;
    case CTRL_BREAK_EVENT: pth_signo = SIGTERM; break;
    }
  SetEvent (pth_signo_ev);
  fprintf (stderr, "sig_handler=%d\n", pth_signo);
  return TRUE;
}


static pth_event_t
do_pth_event_body (unsigned long spec, va_list arg)
{
  SECURITY_ATTRIBUTES sa;
  pth_event_t ev;
  int rc;

  fprintf (stderr, "pth_event spec=%lu\n", spec);
  ev = calloc (1, sizeof *ev);
  if (!ev)
    return NULL;
  if (spec == 0)
    ;
  else if (spec & PTH_EVENT_SIGS)
    {
      ev->u.sig = va_arg (arg, struct sigset_s *);
      ev->u_type = PTH_EVENT_SIGS;
      ev->val = va_arg (arg, int *);	
      rc = SetConsoleCtrlHandler (sig_handler, TRUE);
      fprintf (stderr, "pth_event: sigs rc=%d\n", rc);
    }
  else if (spec & PTH_EVENT_FD)
    {
      if (spec & PTH_UNTIL_FD_READABLE)
        ev->flags |= PTH_UNTIL_FD_READABLE;
      if (spec & PTH_MODE_STATIC)
        ev->flags |= PTH_MODE_STATIC;
      ev->u_type = PTH_EVENT_FD;
      va_arg (arg, pth_key_t);
      ev->u.fd = va_arg (arg, int);
      fprintf (stderr, "pth_event: fd=%d\n", ev->u.fd);
    }
  else if (spec & PTH_EVENT_TIME)
    {
      pth_time_t t;
      if (spec & PTH_MODE_STATIC)
        ev->flags |= PTH_MODE_STATIC;
      va_arg (arg, pth_key_t);
      t = va_arg (arg, pth_time_t);
      ev->u_type = PTH_EVENT_TIME;
      ev->u.tv.tv_sec =  t.tv_sec;
      ev->u.tv.tv_usec = t.tv_usec;
    }
  else if (spec & PTH_EVENT_MUTEX)
    {
      va_arg (arg, pth_key_t);
      ev->u_type = PTH_EVENT_MUTEX;
      ev->u.mx = va_arg (arg, pth_mutex_t*);
    }
    
  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;
  ev->hd = CreateEvent (&sa, FALSE, FALSE, NULL);
  if (!ev->hd)
    {
      free (ev);
      return NULL;
    }

  return ev;
}

static pth_event_t
do_pth_event (unsigned long spec, ...)
{
  va_list arg;
  pth_event_t ev;

  va_start (arg, spec);
  ev = do_pth_event_body (spec, arg);
  va_end (arg);
    
  return ev;
}

pth_event_t
pth_event (unsigned long spec, ...)
{
  va_list arg;
  pth_event_t ev;

  implicit_init ();
  enter_pth (__FUNCTION__);
  
  va_start (arg, spec);
  ev = do_pth_event_body (spec, arg);
  va_end (arg);
    
  leave_pth (__FUNCTION__);
  return ev;
}


static void
pth_event_add (pth_event_t root, pth_event_t node)
{
  pth_event_t n;

  for (n=root; n->next; n = n->next)
    ;
  n->next = node;
}


pth_event_t
pth_event_concat (pth_event_t evf, ...)
{
  pth_event_t evn;
  va_list ap;

  if (!evf)
    return NULL;

  implicit_init ();

  va_start (ap, evf);
  while ((evn = va_arg(ap, pth_event_t)) != NULL)
    pth_event_add (evf, evn);
  va_end (ap);

  return evf;
}


static int
wait_for_fd (int fd, int is_read, int nwait)
{
  struct timeval tv;
  fd_set r;
  fd_set w;
  int n;

  FD_ZERO (&r);
  FD_ZERO (&w);    
  FD_SET (fd, is_read ? &r : &w);

  tv.tv_sec = nwait;
  tv.tv_usec = 0;

  while (1)
    {
      n = select (fd+1, &r, &w, NULL, &tv);
      fprintf (stderr, "wait_for_fd=%d fd %d (ec=%d)\n", n, fd,(int)WSAGetLastError ());
      if (n == -1)
        break;
      if (!n)
        continue;
      if (n == 1)
        {
          if (is_read && FD_ISSET (fd, &r))
            break;
          else if (FD_ISSET (fd, &w))
            break;
        }
    }
  return 0;
}


static void *
helper_thread (void * ctx)
{
  struct _pth_priv_hd_s * c = ctx;
  
  leave_pth (__FUNCTION__);
  c->thread (c->arg);
  enter_pth (__FUNCTION__);
  free (c);
  ExitThread (0);
  return NULL;
}

/* void */
/* sigemptyset (struct sigset_s * ss) */
/* { */
/*     if (ss) { */
/* 	memset (ss->sigs, 0, sizeof ss->sigs); */
/* 	ss->idx = 0; */
/*     } */
/* } */


/* int */
/* sigaddset (struct sigset_s * ss, int signo) */
/* { */
/*     if (!ss) */
/* 	return -1; */
/*     if (ss->idx + 1 > 64) */
/* 	return -1; */
/*     ss->sigs[ss->idx] = signo; */
/*     ss->idx++; */
/*     return 0; */
/* }  */


static int
sigpresent (struct sigset_s * ss, int signo)
{
/*     int i; */
/*     for (i=0; i < ss->idx; i++) { */
/* 	if (ss->sigs[i] == signo) */
/* 	    return 1; */
/*     } */
/* FIXME: See how to implement it.  */
    return 0;
}


static int
do_pth_event_occurred (pth_event_t ev)
{
  int ret;

  if (!ev)
    return 0;

  ret = 0;
  switch (ev->u_type)
    {
    case 0:
      if (WaitForSingleObject (ev->hd, 0) == WAIT_OBJECT_0)
        ret = 1;
      break;

    case PTH_EVENT_SIGS:
      if (sigpresent (ev->u.sig, pth_signo) &&
          WaitForSingleObject (pth_signo_ev, 0) == WAIT_OBJECT_0)
        {
          fprintf (stderr, "pth_event_occurred: sig signaled.\n");
          (*ev->val) = pth_signo;
          ret = 1;
        }
      break;

    case PTH_EVENT_FD:
      if (WaitForSingleObject (ev->hd, 0) == WAIT_OBJECT_0)
        ret = 1;
      break;
    }

  return ret;
}


int
pth_event_occurred (pth_event_t ev)
{
  int ret;

  implicit_init ();
  enter_pth (__FUNCTION__);
  ret = do_pth_event_occurred (ev);
  leave_pth (__FUNCTION__);
  return ret;
}


static int
do_pth_event_status (pth_event_t ev)
{
  if (!ev)
    return 0;
  if (do_pth_event_occurred (ev))
    return PTH_STATUS_OCCURRED;
  return 0;
}

int
pth_event_status (pth_event_t ev)
{
  if (!ev)
    return 0;
  if (pth_event_occurred (ev))
    return PTH_STATUS_OCCURRED;
  return 0;
}


static int
do_pth_event_free (pth_event_t ev, int mode)
{
  pth_event_t n;

  if (mode == PTH_FREE_ALL)
    {
      while (ev)
        {
          n = ev->next;
          CloseHandle (ev->hd); ev->hd = NULL;
          free (ev);
          ev = n;
        }
    }
  else if (mode == PTH_FREE_THIS)
    {
      ev->prev->next = ev->next;
      ev->next->prev = ev->prev;
      CloseHandle (ev->hd); ev->hd = NULL;	    
      free (ev);
	
    }

  return 0;
}

int
pth_event_free (pth_event_t ev, int mode)
{
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);
  rc = do_pth_event_free (ev, mode);
  leave_pth (__FUNCTION__);
  return rc;
}


pth_event_t
pth_event_isolate (pth_event_t ev)
{
  pth_event_t ring = NULL;

  if (!ev)
    return NULL;
  return ring;    
}


static int
pth_event_count (pth_event_t ev)
{
  pth_event_t p;
  int cnt=0;

  if (!ev)
    return 0;
  for (p=ev; p; p = p->next)
    cnt++;    
  return cnt;
}



static pth_t
spawn_helper_thread (pth_attr_t hd, void *(*func)(void *), void *arg)
{
  SECURITY_ATTRIBUTES sa;
  DWORD tid;
  HANDLE th;

  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;

  fprintf (stderr, "spawn_helper_thread creating thread ...\n");
  th = CreateThread (&sa, hd->stack_size,
                     (LPTHREAD_START_ROUTINE)func,
                     arg, 0, &tid);
  fprintf (stderr, "spawn_helper_thread created thread %p\n", th);

  return th;
}


static void 
free_helper_threads (HANDLE *waitbuf, int *hdidx, int n)
{
  int i;

  for (i=0; i < n; i++)
    CloseHandle (waitbuf[hdidx[i]]);
}


static void *
wait_fd_thread (void * ctx)
{
  pth_event_t ev = ctx;

  wait_for_fd (ev->u.fd, ev->flags & PTH_UNTIL_FD_READABLE, 3600);
  fprintf (stderr, "wait_fd_thread: exit.\n");
  SetEvent (ev->hd);
  ExitThread (0);
  return NULL;
}


static void *
wait_timer_thread (void * ctx)
{
  pth_event_t ev = ctx;
  int n = ev->u.tv.tv_sec*1000;
  Sleep (n);
  SetEvent (ev->hd);
  fprintf (stderr, "wait_timer_thread: exit.\n");
  ExitThread (0);
  return NULL;
}


static int
do_pth_wait (pth_event_t ev)
{
  HANDLE waitbuf[MAXIMUM_WAIT_OBJECTS/2];
  int    hdidx[MAXIMUM_WAIT_OBJECTS/2];
  DWORD n = 0;
  pth_attr_t attr;
  pth_event_t tmp;
  int pos=0, i=0;

  if (!ev)
    return 0;

  attr = pth_attr_new ();
  pth_attr_set (attr, PTH_ATTR_JOINABLE, 1);
  pth_attr_set (attr, PTH_ATTR_STACK_SIZE, 4096);
    
  fprintf (stderr, "pth_wait: cnt %d\n", pth_event_count (ev));
  for (tmp = ev; tmp; tmp = tmp->next)
    {
      if (pos+1 > MAXIMUM_WAIT_OBJECTS/2)
        {
          free_helper_threads (waitbuf, hdidx, i);
          pth_attr_destroy (attr);
          return -1;
        }
      switch (tmp->u_type)
        {
        case 0:
          waitbuf[pos++] = tmp->hd;
          break;

        case PTH_EVENT_SIGS:
          waitbuf[pos++] = pth_signo_ev;
          fprintf (stderr, "pth_wait: add signal event.\n");
          break;

        case PTH_EVENT_FD:
          fprintf (stderr, "pth_wait: spawn event wait thread.\n");
          hdidx[i++] = pos;
          waitbuf[pos++] = spawn_helper_thread (attr, wait_fd_thread, tmp);
          break;

        case PTH_EVENT_TIME:
          fprintf (stderr, "pth_wait: spawn event timer thread.\n");
          hdidx[i++] = pos;
          waitbuf[pos++] = spawn_helper_thread (attr, wait_timer_thread, tmp);
          break;

        case PTH_EVENT_MUTEX:
          fprintf (stderr, "pth_wait: add mutex event.\n");
          hdidx[i++] = pos;
          waitbuf[pos++] = tmp->u.mx->mx;
          /* XXX: Use SetEvent(hd->ev) */
          break;
        }
    }
  fprintf (stderr, "pth_wait: set %d\n", pos);
  n = WaitForMultipleObjects (pos, waitbuf, FALSE, INFINITE);
  free_helper_threads (waitbuf, hdidx, i);
  pth_attr_destroy (attr);
  fprintf (stderr, "pth_wait: n %ld\n", n);

  if (n != WAIT_TIMEOUT)
    return 1;
    
  return 0;
}

int
pth_wait (pth_event_t ev)
{
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);
  rc = do_pth_wait (ev);
  leave_pth (__FUNCTION__);
  return rc;
}


int
pth_sleep (int sec)
{
  static pth_key_t ev_key = PTH_KEY_INIT;
  pth_event_t ev;

  implicit_init ();
  enter_pth (__FUNCTION__);

  if (sec == 0)
    {
      leave_pth (__FUNCTION__);
      return 0;
    }

  ev = do_pth_event (PTH_EVENT_TIME|PTH_MODE_STATIC, &ev_key,
                     pth_timeout (sec, 0));
  if (ev == NULL)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }
  do_pth_wait (ev);
  do_pth_event_free (ev, PTH_FREE_ALL);

  leave_pth (__FUNCTION__);
  return 0;
}





/* 
   Some simple tests.  
 */
#ifdef TEST
#include <stdio.h>

void * thread (void * c)
{

  Sleep (2000);
  SetEvent (((pth_event_t)c)->hd);
  fprintf (stderr, "\n\nhallo!.\n");
  pth_exit (NULL);
  return NULL;
}


int main_1 (int argc, char ** argv)
{
  pth_attr_t t;
  pth_t hd;
  pth_event_t ev;

  pth_init ();
  ev = pth_event (0, NULL);
  t = pth_attr_new ();
  pth_attr_set (t, PTH_ATTR_JOINABLE, 1);
  pth_attr_set (t, PTH_ATTR_STACK_SIZE, 4096);
  pth_attr_set (t, PTH_ATTR_NAME, "hello");
  hd = pth_spawn (t, thread, ev);

  pth_wait (ev);
  pth_attr_destroy (t);
  pth_event_free (ev, 0);
  pth_kill ();

  return 0;
}


static pth_event_t 
setup_signals (struct sigset_s *sigs, int *signo)
{
  pth_event_t ev;

  sigemptyset (sigs);
  sigaddset (sigs, SIGINT);
  sigaddset (sigs, SIGTERM);

  ev = pth_event (PTH_EVENT_SIGS, sigs, signo);
  return ev;
}

int
main_2 (int argc, char ** argv)
{
  pth_event_t ev;
  struct sigset_s sigs;
  int signo = 0;

  pth_init ();
  ev = setup_signals (&sigs, &signo);
  pth_wait (ev);
  if (pth_event_occured (ev) && signo)
    fprintf (stderr, "signal caught! signo %d\n", signo);

  pth_event_free (ev, PTH_FREE_ALL);
  pth_kill ();
  return 0;
}

int
main_3 (int argc, char ** argv)
{
  struct sockaddr_in addr, rem;
  int fd, n = 0, infd;
  int signo = 0;
  struct sigset_s sigs;
  pth_event_t ev;

  pth_init ();
  fd = socket (AF_INET, SOCK_STREAM, 0);

  memset (&addr, 0, sizeof addr);
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons (5050);
  addr.sin_family = AF_INET;
  bind (fd, (struct sockaddr*)&addr, sizeof addr);
  listen (fd, 5);

  ev = setup_signals (&sigs, &signo);
  n = sizeof addr;
  infd = pth_accept_ev (fd, (struct sockaddr *)&rem, &n, ev);
  fprintf (stderr, "infd %d: %s:%d\n", infd, inet_ntoa (rem.sin_addr),
          htons (rem.sin_port));

  closesocket (infd);
  pth_event_free (ev, PTH_FREE_ALL);
  pth_kill ();
  return 0;
}

int
main (int argc, char ** argv)
{
  pth_event_t ev;
  pth_key_t ev_key;

  pth_init ();
  /*ev = pth_event (PTH_EVENT_TIME, &ev_key, pth_timeout (5, 0));
    pth_wait (ev);
    pth_event_free (ev, PTH_FREE_ALL);*/
  pth_sleep (5);
  pth_kill ();
  return 0;
}
#endif

#endif /*HAVE_W32_SYSTEM*/

