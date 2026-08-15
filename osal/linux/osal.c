/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */
#include <osal.h>
#include <stdlib.h>
#include <string.h>
#ifdef USE_XENOMAI_EVL
#include <unistd.h>
#include <sys/syscall.h>
#endif

#ifdef USE_XENOMAI_EVL
typedef struct
{
   void *(*real_func)(void *);
   void *real_arg;
} evl_rt_trampoline_argt;
#endif

/* Returns time from some unspecified moment in past,
 * strictly increasing, used for time intervals measurement. */
void osal_get_monotonic_time(ec_timet *ts)
{
#ifdef USE_XENOMAI_EVL
   evl_read_clock(EVL_CLOCK_MONOTONIC, ts);
#else
   /* Use clock_gettime to prevent possible live-lock.
    * Gettimeofday uses CLOCK_REALTIME that can get NTP timeadjust.
    * If this function preempts timeadjust and it uses vpage it live-locks.
    * Also when using XENOMAI, only clock_gettime is RT safe */
   clock_gettime(CLOCK_MONOTONIC, ts);
#endif
}

ec_timet osal_current_time(void)
{
   struct timespec ts;

#ifdef USE_XENOMAI_EVL
   evl_read_clock(EVL_CLOCK_REALTIME, &ts);
#else
   clock_gettime(CLOCK_REALTIME, &ts);
#endif
   return ts;
}

void osal_time_diff(ec_timet *start, ec_timet *end, ec_timet *diff)
{
   osal_timespecsub(end, start, diff);
}

void osal_timer_start(osal_timert *self, uint32 timeout_usec)
{
   struct timespec start_time;
   struct timespec timeout;

   osal_get_monotonic_time(&start_time);
   osal_timespec_from_usec(timeout_usec, &timeout);
   osal_timespecadd(&start_time, &timeout, &self->stop_time);
}

boolean osal_timer_is_expired(osal_timert *self)
{
   struct timespec current_time;
   int is_not_yet_expired;

   osal_get_monotonic_time(&current_time);
   is_not_yet_expired = osal_timespeccmp(&current_time, &self->stop_time, <);

   return is_not_yet_expired == FALSE;
}

int osal_usleep(uint32 usec)
{
   int result;

#ifdef USE_XENOMAI_EVL
   while (usec > 1000000u)
   {
      result = evl_usleep(1000000u);
      if (result != 0)
      {
         return -1;
      }
      usec -= 1000000u;
   }
   result = evl_usleep((useconds_t)usec);
#else
   struct timespec ts;
   osal_timespec_from_usec(usec, &ts);
   result = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
#endif

   return result == 0 ? 0 : -1;
}

int osal_monotonic_sleep(ec_timet *ts)
{
   int result;
#ifdef USE_XENOMAI_EVL
   result = evl_sleep_until(EVL_CLOCK_MONOTONIC, ts);
#else
   result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ts, NULL);
#endif
   return result == 0 ? 0 : -1;
}

void *osal_malloc(size_t size)
{
   return malloc(size);
}

void osal_free(void *ptr)
{
   free(ptr);
}

int osal_thread_create(void *thandle, int stacksize, void *func, void *param)
{
   int ret;
   pthread_attr_t attr;
   pthread_t *threadp;

   threadp = thandle;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, stacksize);
   ret = pthread_create(threadp, &attr, func, param);
   if (ret < 0)
   {
      return 0;
   }
   return 1;
}

#ifdef USE_XENOMAI_EVL
static void *evl_rt_trampoline(void *arg)
{
   evl_rt_trampoline_argt targ = *(evl_rt_trampoline_argt *)arg;
   long tid;
   int efd;

   osal_free(arg);

   tid = syscall(SYS_gettid);
   efd = evl_attach_self("soem-rt-%ld", tid);
   if (efd < 0)
   {
      EC_PRINT("osal_thread_create_rt: evl_attach_self() failed: %d -- "
               "running in-band, DC sync jitter guarantees are void.\n",
               efd);
   }

   return targ.real_func(targ.real_arg);
}
#endif

int osal_thread_create_rt(void *thandle, int stacksize, void *func, void *param)
{
#ifdef USE_XENOMAI_EVL
   int ret;
   pthread_attr_t attr;
   struct sched_param schparam;
   pthread_t *threadp;
   evl_rt_trampoline_argt *targ;

   targ = (evl_rt_trampoline_argt *)osal_malloc(sizeof(*targ));
   if (!targ)
   {
      return 0;
   }
   targ->real_func = (void *(*)(void *))func;
   targ->real_arg = param;

   threadp = thandle;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, stacksize);
   pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
   pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
   memset(&schparam, 0, sizeof(schparam));
   schparam.sched_priority = 40;
   pthread_attr_setschedparam(&attr, &schparam);

   ret = pthread_create(threadp, &attr, evl_rt_trampoline, targ);
   pthread_attr_destroy(&attr);
   if (ret < 0)
   {
      osal_free(targ);
      return 0;
   }
#else
   int ret;
   pthread_attr_t attr;
   struct sched_param schparam;
   pthread_t *threadp;

   threadp = thandle;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, stacksize);
   ret = pthread_create(threadp, &attr, func, param);
   pthread_attr_destroy(&attr);
   if (ret < 0)
   {
      return 0;
   }
   memset(&schparam, 0, sizeof(schparam));
   schparam.sched_priority = 40;
   ret = pthread_setschedparam(*threadp, SCHED_FIFO, &schparam);
   if (ret < 0)
   {
      return 0;
   }
#endif

   return 1;
}

void *osal_mutex_create(void)
{
#ifdef USE_XENOMAI_EVL
   osal_mutext *mutex;
   int fd;
   mutex = (osal_mutext *)osal_malloc(sizeof(osal_mutext));
   if (mutex)
   {
      fd = evl_new_mutex(mutex, "soem-mtx-%d-%p", (int)getpid(), (void *)mutex);
      if (fd < 0)
      {
         EC_PRINT("osal_mutex_create: evl_new_mutex() failed: %d\n", fd);
         osal_free(mutex);
         return NULL;
      }
   }
#else
   pthread_mutexattr_t mutexattr;
   osal_mutext *mutex;
   mutex = (osal_mutext *)osal_malloc(sizeof(osal_mutext));
   if (mutex)
   {
      pthread_mutexattr_init(&mutexattr);
      pthread_mutexattr_setprotocol(&mutexattr, PTHREAD_PRIO_INHERIT);
      pthread_mutex_init(mutex, &mutexattr);
   }
#endif

   return (void *)mutex;
}

void osal_mutex_destroy(void *mutex)
{
#ifdef USE_XENOMAI_EVL
   evl_close_mutex((osal_mutext *)mutex);
#else
   pthread_mutex_destroy((osal_mutext *)mutex);
#endif
   osal_free(mutex);
}

void osal_mutex_lock(void *mutex)
{
#ifdef USE_XENOMAI_EVL
   evl_lock_mutex((osal_mutext *)mutex);
#else
   pthread_mutex_lock((osal_mutext *)mutex);
#endif
}

void osal_mutex_unlock(void *mutex)
{
#ifdef USE_XENOMAI_EVL
   evl_unlock_mutex((osal_mutext *)mutex);
#else
   pthread_mutex_unlock((osal_mutext *)mutex);
#endif
}
