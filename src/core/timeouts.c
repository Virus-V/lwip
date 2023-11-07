/**
 * @file
 * Stack-internal timers implementation.
 * This file includes timer callbacks for stack-internal timers as well as
 * functions to set up or stop timers and check for expired timers.
 *
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *         Simon Goldschmidt
 *
 */

#include "lwip/opt.h"

#include "lwip/timeouts.h"
#include "lwip/priv/tcp_priv.h"

#include "lwip/def.h"
#include "lwip/memp.h"
#include "lwip/priv/tcpip_priv.h"

#include "lwip/ip4_frag.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/autoip.h"
#include "lwip/igmp.h"
#include "lwip/dns.h"
#include "lwip/nd6.h"
#include "lwip/ip6_frag.h"
#include "lwip/mld6.h"
#include "lwip/dhcp6.h"
#include "lwip/sys.h"
#include "lwip/pbuf.h"

#if LWIP_DEBUG_TIMERNAMES
#define HANDLER(x) x, #x
#else /* LWIP_DEBUG_TIMERNAMES */
#define HANDLER(x) x
#endif /* LWIP_DEBUG_TIMERNAMES */

#define LWIP_MAX_TIMEOUT  0x7fffffff

/* Check if timer's expiry time is greater than time and care about u32_t wraparounds */
#define TIME_LESS_THAN(t, compare_to) ( (((u32_t)((t)-(compare_to))) > LWIP_MAX_TIMEOUT) ? 1 : 0 )

#if LWIP_TCP
static char tcpip_slow_timer_pending = 0;
static char tcpip_fast_timer_pending = 0;
static void tcpip_tcp_slow_timer(void);
static void tcpip_tcp_fast_timer(void);
static bool tcp_slow_timer_calculate_next_wake(u32_t * next_wake_ms);
static bool tcp_fast_timer_calculate_next_wake(u32_t * next_wake_ms);
#endif

/** This array contains all stack-internal cyclic timers. To get the number of
 * timers, use LWIP_ARRAYSIZE() */
struct lwip_cyclic_timer lwip_cyclic_timers[] = {
#if LWIP_TCP
  /* The TCP timer is a special case: it does not have to run always and
     is triggered to start from TCP using tcp_timer_needed() */
  {LWIP_TIMER_STATUS_RUNNING, TCP_SLOW_INTERVAL, HANDLER(tcpip_tcp_slow_timer)},
  {LWIP_TIMER_STATUS_RUNNING, TCP_TMR_INTERVAL, HANDLER(tcpip_tcp_fast_timer)},
#endif /* LWIP_TCP */
#if LWIP_IPV4
#if IP_REASSEMBLY
  {LWIP_TIMER_STATUS_RUNNING, IP_TMR_INTERVAL, HANDLER(ip_reass_tmr)},
#endif /* IP_REASSEMBLY */
#if LWIP_ARP
  {LWIP_TIMER_STATUS_RUNNING, ARP_TMR_INTERVAL, HANDLER(etharp_tmr)},
#endif /* LWIP_ARP */
#if LWIP_DHCP
  {LWIP_TIMER_STATUS_RUNNING, DHCP_COARSE_TIMER_MSECS, HANDLER(dhcp_coarse_tmr)},
  {LWIP_TIMER_STATUS_RUNNING, DHCP_FINE_TIMER_MSECS, HANDLER(dhcp_fine_tmr)},
#endif /* LWIP_DHCP */
#if LWIP_AUTOIP
  {LWIP_TIMER_STATUS_RUNNING, AUTOIP_TMR_INTERVAL, HANDLER(autoip_tmr)},
#endif /* LWIP_AUTOIP */
#if LWIP_IGMP
  {LWIP_TIMER_STATUS_RUNNING, IGMP_TMR_INTERVAL, HANDLER(igmp_tmr)},
#endif /* LWIP_IGMP */
#endif /* LWIP_IPV4 */
#if LWIP_DNS
  {LWIP_TIMER_STATUS_RUNNING, DNS_TMR_INTERVAL, HANDLER(dns_tmr)},
#endif /* LWIP_DNS */
#if LWIP_IPV6
  {LWIP_TIMER_STATUS_IDLE, ND6_TMR_INTERVAL, HANDLER(nd6_tmr)},
#if LWIP_IPV6_REASS
  {LWIP_TIMER_STATUS_IDLE, IP6_REASS_TMR_INTERVAL, HANDLER(ip6_reass_tmr)},
#endif /* LWIP_IPV6_REASS */
#if LWIP_IPV6_MLD
  {LWIP_TIMER_STATUS_IDLE, MLD6_TMR_INTERVAL, HANDLER(mld6_tmr)},
#endif /* LWIP_IPV6_MLD */
#if LWIP_IPV6_DHCP6
  {LWIP_TIMER_STATUS_IDLE, DHCP6_TIMER_MSECS, HANDLER(dhcp6_tmr)},
#endif /* LWIP_IPV6_DHCP6 */
#endif /* LWIP_IPV6 */
};
const int lwip_num_cyclic_timers = LWIP_ARRAYSIZE(lwip_cyclic_timers);

#if LWIP_TIMERS && !LWIP_TIMERS_CUSTOM

/** The one and only timeout list */
static struct sys_timeo *next_timeout;

static u32_t cur_mbox_fetch_sleeptime;
extern u32_t tcp_ticks;

static u32_t current_timeout_due_time;

#if LWIP_TESTMODE
struct sys_timeo**
sys_timeouts_get_next_timeout(void)
{
  return &next_timeout;
}
#endif

#if LWIP_TCP
void tcpip_tmr_compensate_tick(void)
{
  static u32_t tmr_first_run_time = 0;
  u32_t now_time = sys_now();

  if (tmr_first_run_time != 0) {
    tcp_ticks = (now_time - tmr_first_run_time) / TCP_SLOW_INTERVAL;
    LWIP_DEBUGF(TCP_DEBUG, ("%s: set tcp_ticks to %d\n", __func__, tcp_ticks));
  } else {
    LWIP_DEBUGF(TCP_DEBUG, ("%s: set tcp_ticks to 0\n", __func__));
    tcp_ticks = 0;
    tmr_first_run_time = now_time;
  }
}

/**
 * Timer callback function that calls tcp_tmr() and reschedules itself.
 *
 * @param arg unused argument
 */
static void
tcpip_tcp_slow_timer(void)
{
  u32_t sleep_duration = 0;

  /* compensate tcp_ticks */
  tcpip_tmr_compensate_tick();

  /* call TCP timer handler */
  tcp_slowtmr();
  tcpip_slow_timer_pending = 0;

  /* timer still needed? */
  if ((tcp_active_pcbs || tcp_tw_pcbs) && !tcp_slow_timer_calculate_next_wake(&sleep_duration)) {
    LWIP_DEBUGF(TCP_DEBUG, ("%s: will sleep %dms\n", __func__, sleep_duration));

    /* restart timer */
    sys_untimeout((sys_timeout_handler)tcpip_tcp_slow_timer, NULL);
    sys_timeout(sleep_duration, (sys_timeout_handler)tcpip_tcp_slow_timer, NULL);
  } else {
    LWIP_DEBUGF(TCP_DEBUG, ("%s: do nothing\n", __func__));
  }
}

static void
tcpip_tcp_fast_timer(void)
{
  u32_t sleep_duration = 0;

  /* compensate tcp_ticks */
  tcpip_tmr_compensate_tick();

  /* call TCP timer handler */
  tcp_fasttmr();
  tcpip_fast_timer_pending = 0;

  /* timer still needed? */
  if ((tcp_active_pcbs) && !tcp_fast_timer_calculate_next_wake(&sleep_duration)) {
    LWIP_DEBUGF(TCP_DEBUG, ("%s: will sleep %dms\n", __func__, sleep_duration));

    /* restart timer */
    sys_untimeout((sys_timeout_handler)tcpip_tcp_fast_timer, NULL);
    sys_timeout(sleep_duration, (sys_timeout_handler)tcpip_tcp_fast_timer, NULL);
  } else {
    LWIP_DEBUGF(TCP_DEBUG, ("%s: do nothing\n", __func__));
  }
}

/**
 * Called from TCP_REG when registering a new PCB:
 * the reason is to have the TCP timer only running when
 * there are active (or time-wait) PCBs.
 */
 void
 tcp_timer_needed(void)
 {
   LWIP_ASSERT_CORE_LOCKED();
   u32_t sleep_duration = 0;

   /* timer is off but needed again? */
   if ((tcp_active_pcbs) && !tcp_fast_timer_calculate_next_wake(&sleep_duration)) {
     LWIP_DEBUGF(TCP_DEBUG, ("tcp_timer_needed: start tcp fast timer, next:%dms\n", sleep_duration));

     if (tcpip_fast_timer_pending == 0) {
       /* (re)start timer */
       sys_untimeout((sys_timeout_handler)tcpip_tcp_fast_timer, NULL);
       sys_timeout(sleep_duration, (sys_timeout_handler)tcpip_tcp_fast_timer, NULL);
       tcpip_fast_timer_pending = 1;
     }
   } else {
     sys_untimeout((sys_timeout_handler)tcpip_tcp_fast_timer, NULL);
   }

   if ((tcp_active_pcbs || tcp_tw_pcbs) && !tcp_slow_timer_calculate_next_wake(&sleep_duration)) {
     LWIP_DEBUGF(TCP_DEBUG, ("tcp_timer_needed: start tcp slow timer, next:%dms\n", sleep_duration));

     if (tcpip_slow_timer_pending == 0) {
       /* (re)start timer */
       sys_untimeout((sys_timeout_handler)tcpip_tcp_slow_timer, NULL);
       sys_timeout(sleep_duration, (sys_timeout_handler)tcpip_tcp_slow_timer, NULL);
       tcpip_slow_timer_pending = 1;
     }
   } else {
     sys_untimeout((sys_timeout_handler)tcpip_tcp_slow_timer, NULL);
   }
 }
#endif /* LWIP_TCP */

static void
#if LWIP_DEBUG_TIMERNAMES
sys_timeout_abs(u32_t abs_time, sys_timeout_handler handler, void *arg, const char *handler_name)
#else /* LWIP_DEBUG_TIMERNAMES */
sys_timeout_abs(u32_t abs_time, sys_timeout_handler handler, void *arg)
#endif
{
  struct sys_timeo *timeout, *t;

  timeout = (struct sys_timeo *)memp_malloc(MEMP_SYS_TIMEOUT);
  if (timeout == NULL) {
    LWIP_ASSERT("sys_timeout: timeout != NULL, pool MEMP_SYS_TIMEOUT is empty", timeout != NULL);
    return;
  }

  timeout->next = NULL;
  timeout->h = handler;
  timeout->arg = arg;
  timeout->time = abs_time;

#if LWIP_DEBUG_TIMERNAMES
  timeout->handler_name = handler_name;
  LWIP_DEBUGF(TIMERS_DEBUG, ("sys_timeout: %p abs_time=%"U32_F" handler=%s arg=%p\n",
                             (void *)timeout, abs_time, handler_name, (void *)arg));
#endif /* LWIP_DEBUG_TIMERNAMES */

  if (next_timeout == NULL) {
    next_timeout = timeout;
    return;
  }
  if (TIME_LESS_THAN(timeout->time, next_timeout->time)) {
    timeout->next = next_timeout;
    next_timeout = timeout;
  } else {
    for (t = next_timeout; t != NULL; t = t->next) {
      if ((t->next == NULL) || TIME_LESS_THAN(timeout->time, t->next->time)) {
        timeout->next = t->next;
        t->next = timeout;
        break;
      }
    }
  }
}

/**
 * Timer callback function that calls cyclic->handler() and reschedules itself.
 *
 * @param arg unused argument
 */
#if !LWIP_TESTMODE
static
#endif
void
lwip_cyclic_timer(void *arg)
{
  u32_t now;
  u32_t next_timeout_time;
  struct lwip_cyclic_timer *cyclic = (struct lwip_cyclic_timer *)arg;

#if LWIP_DEBUG_TIMERNAMES
  LWIP_DEBUGF(TIMERS_DEBUG, ("tcpip: %s()\n", cyclic->handler_name));
#endif
  /*
   * Add to support enable/disable timer dynamically
   **/
  if (cyclic->status == LWIP_TIMER_STATUS_RUNNING) {
    cyclic->handler();
  }

  /*
   * Add to support enable/disable timer dynamically
   *
   * Do not merge the if block into the above one,
   * because the cyclic->handler may change the cyclic->status value
   */
  if (LWIP_TIMER_STATUS_RUNNING == cyclic->status) {
      now = sys_now();
      next_timeout_time = (u32_t)(current_timeout_due_time + cyclic->interval_ms);  /* overflow handled by TIME_LESS_THAN macro */
      if (TIME_LESS_THAN(next_timeout_time, now)) {
        /* timer would immediately expire again -> "overload" -> restart without any correction */
#if LWIP_DEBUG_TIMERNAMES
        sys_timeout_abs((u32_t)(now + cyclic->interval_ms), lwip_cyclic_timer, arg, cyclic->handler_name);
#else
        sys_timeout_abs((u32_t)(now + cyclic->interval_ms), lwip_cyclic_timer, arg);
#endif
      } else {
        /* correct cyclic interval with handler execution delay and sys_check_timeouts jitter */
#if LWIP_DEBUG_TIMERNAMES
        sys_timeout_abs(next_timeout_time, lwip_cyclic_timer, arg, cyclic->handler_name);
#else
        sys_timeout_abs(next_timeout_time, lwip_cyclic_timer, arg);
#endif
      }
  } else if (LWIP_TIMER_STATUS_STOPPING == cyclic->status) {
    cyclic->status = LWIP_TIMER_STATUS_IDLE;
  }
}

/** Initialize this module */
void sys_timeouts_init(void)
{
  size_t i;
  /* tcp_tmr() at index 0 is started on demand */
  for (i = (LWIP_TCP ? 2 : 0); i < LWIP_ARRAYSIZE(lwip_cyclic_timers); i++) {
    /* we have to cast via size_t to get rid of const warning
      (this is OK as cyclic_timer() casts back to const* */

    if (LWIP_TIMER_STATUS_RUNNING == lwip_cyclic_timers[i].status) {
      sys_timeout(lwip_cyclic_timers[i].interval_ms, lwip_cyclic_timer, LWIP_CONST_CAST(void*, &lwip_cyclic_timers[i]));
    }
  }
}

static void sys_timer_callback_for_add_timer(void *ctx) {
  LWIP_DEBUGF(TIMERS_DEBUG, ("sys_timer_callback_for_add_timer\n"));
}

/**
 * Create a one-shot timer (aka timeout). Timeouts are processed in the
 * following cases:
 * - while waiting for a message using sys_timeouts_mbox_fetch()
 * - by calling sys_check_timeouts() (NO_SYS==1 only)
 *
 * @param msecs time in milliseconds after that the timer should expire
 * @param handler callback function to call when msecs have elapsed
 * @param arg argument to pass to the callback function
 */
#if LWIP_DEBUG_TIMERNAMES
void
sys_timeout_debug(u32_t msecs, sys_timeout_handler handler, void *arg, const char *handler_name)
#else /* LWIP_DEBUG_TIMERNAMES */
void
sys_timeout(u32_t msecs, sys_timeout_handler handler, void *arg)
#endif /* LWIP_DEBUG_TIMERNAMES */
{
  u32_t next_timeout_time;

  LWIP_ASSERT_CORE_LOCKED();

  LWIP_ASSERT("Timeout time too long, max is LWIP_UINT32_MAX/4 msecs", msecs <= (LWIP_UINT32_MAX / 4));

  next_timeout_time = (u32_t)(sys_now() + msecs); /* overflow handled by TIME_LESS_THAN macro */

#if LWIP_DEBUG_TIMERNAMES
  sys_timeout_abs(next_timeout_time, handler, arg, handler_name);
#else
  sys_timeout_abs(next_timeout_time, handler, arg);
#endif

  if (cur_mbox_fetch_sleeptime != 0 && msecs < cur_mbox_fetch_sleeptime) {
    tcpip_callback_with_block(sys_timer_callback_for_add_timer, NULL, 0);
  }
}

void sys_timeouts_set_timer_enable(bool enable, lwip_cyclic_timer_handler handler)
{
   size_t i;

  for (i = (LWIP_TCP ? 1 : 0); i < LWIP_ARRAYSIZE(lwip_cyclic_timers); i++) {
    if (lwip_cyclic_timers[i].handler == handler) {
      if (LWIP_TIMER_STATUS_RUNNING == lwip_cyclic_timers[i].status && !enable)
      {
        lwip_cyclic_timers[i].status = LWIP_TIMER_STATUS_STOPPING;
      }
      else if (enable)
      {
        if (LWIP_TIMER_STATUS_IDLE == lwip_cyclic_timers[i].status)
        {
          sys_timeout(lwip_cyclic_timers[i].interval_ms, lwip_cyclic_timer,
              LWIP_CONST_CAST(void*, &lwip_cyclic_timers[i]));
        }
        lwip_cyclic_timers[i].status = LWIP_TIMER_STATUS_RUNNING;
      }
      break;
    }
  }
}

/**
 * Go through timeout list (for this task only) and remove the first matching
 * entry (subsequent entries remain untouched), even though the timeout has not
 * triggered yet.
 *
 * @param handler callback function that would be called by the timeout
 * @param arg callback argument that would be passed to handler
*/
void
sys_untimeout(sys_timeout_handler handler, void *arg)
{
  struct sys_timeo *prev_t, *t;

  LWIP_ASSERT_CORE_LOCKED();

  if (next_timeout == NULL) {
    return;
  }

  for (t = next_timeout, prev_t = NULL; t != NULL; prev_t = t, t = t->next) {
    if ((t->h == handler) && (t->arg == arg)) {
      /* We have a match */
      /* Unlink from previous in list */
      if (prev_t == NULL) {
        next_timeout = t->next;
      } else {
        prev_t->next = t->next;
      }
      memp_free(MEMP_SYS_TIMEOUT, t);
      return;
    }
  }
  return;
}

/**
 * @ingroup lwip_nosys
 * Handle timeouts for NO_SYS==1 (i.e. without using
 * tcpip_thread/sys_timeouts_mbox_fetch(). Uses sys_now() to call timeout
 * handler functions when timeouts expire.
 *
 * Must be called periodically from your main loop.
 */
void
sys_check_timeouts(void)
{
  u32_t now;

  LWIP_ASSERT_CORE_LOCKED();

  /* Process only timers expired at the start of the function. */
  now = sys_now();

  do {
    struct sys_timeo *tmptimeout;
    sys_timeout_handler handler;
    void *arg;

    PBUF_CHECK_FREE_OOSEQ();

    tmptimeout = next_timeout;
    if (tmptimeout == NULL) {
      return;
    }

    if (TIME_LESS_THAN(now, tmptimeout->time)) {
      return;
    }

    /* Timeout has expired */
    next_timeout = tmptimeout->next;
    handler = tmptimeout->h;
    arg = tmptimeout->arg;
    current_timeout_due_time = tmptimeout->time;
#if LWIP_DEBUG_TIMERNAMES
    if (handler != NULL) {
      LWIP_DEBUGF(TIMERS_DEBUG, ("sct calling h=%s t=%"U32_F" arg=%p\n",
                                 tmptimeout->handler_name, sys_now() - tmptimeout->time, arg));
    }
#endif /* LWIP_DEBUG_TIMERNAMES */
    memp_free(MEMP_SYS_TIMEOUT, tmptimeout);
    if (handler != NULL) {
      handler(arg);
    }
    LWIP_TCPIP_THREAD_ALIVE();

    /* Repeat until all expired timers have been called */
  } while (1);
}

/** Rebase the timeout times to the current time.
 * This is necessary if sys_check_timeouts() hasn't been called for a long
 * time (e.g. while saving energy) to prevent all timer functions of that
 * period being called.
 */
void
sys_restart_timeouts(void)
{
  u32_t now;
  u32_t base;
  struct sys_timeo *t;

  if (next_timeout == NULL) {
    return;
  }

  now = sys_now();
  base = next_timeout->time;

  for (t = next_timeout; t != NULL; t = t->next) {
    t->time = (t->time - base) + now;
  }
}

/** Return the time left before the next timeout is due. If no timeouts are
 * enqueued, returns 0xffffffff
 */
u32_t
sys_timeouts_sleeptime(void)
{
  u32_t now;

  LWIP_ASSERT_CORE_LOCKED();

  if (next_timeout == NULL) {
    cur_mbox_fetch_sleeptime = SYS_TIMEOUTS_SLEEPTIME_INFINITE;
    return SYS_TIMEOUTS_SLEEPTIME_INFINITE;
  }
  now = sys_now();
  if (TIME_LESS_THAN(next_timeout->time, now)) {
    cur_mbox_fetch_sleeptime = 0;
    return 0;
  } else {
    u32_t ret = (u32_t)(next_timeout->time - now);
    cur_mbox_fetch_sleeptime = ret;
    LWIP_ASSERT("invalid sleeptime", ret <= LWIP_MAX_TIMEOUT);
    return ret;
  }
}

// tcp_tmr stop_condition 1. (Removed)only run MAX_TCP_ONCE_RUNNING_TIME 2min
//                        2. all active pcb must not have unsent segment
//                        3. all active pcb must not have unacked segment
//                        4. all active pcb must not have persist_timer
//                        5. all active pcb must be ESTABLISHED state, no data send/receive
//                        6. (fast timer)all active pcb must not refused data
//                        7. all active pcb must not ooseq
//                        8. (fast timer)all active pcb's flags must not TF_ACK_DELAY or TF_CLOSEPEND
//                        9. no time_wait pcb

/* return true indicate stop tcp timer */
static bool tcp_slow_timer_calculate_next_wake(u32_t * next_wake_ms)
{
  s32_t min_wake_time = LWIP_UINT32_MAX/4;

  struct tcp_pcb *pcb = tcp_active_pcbs;
  while (pcb != NULL) {
    if (pcb->unacked != NULL) {
      /* calculate retransmission timeouts */
      if ((tcp_ticks - pcb->rtime) <= pcb->rto) {
        LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: retransmission timeout %ldms\n", (pcb->rto - (tcp_ticks - pcb->rtime)) * TCP_SLOW_INTERVAL));
        min_wake_time = LWIP_MIN((pcb->rto - (tcp_ticks - pcb->rtime)) * TCP_SLOW_INTERVAL, min_wake_time);
      } else {
        LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: retransmission tcp_ticks:%d, pcb->rtime:%d, pcb->rto:%d, please check.\n", tcp_ticks, pcb->rtime, pcb->rto));
        min_wake_time = TCP_SLOW_INTERVAL;
      }
    }

    if (pcb->persist_backoff > 0) {
      u8_t backoff_cnt = tcp_persist_backoff[pcb->persist_backoff - 1];
      LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: persist time timeout %ldms\n", (backoff_cnt - (tcp_ticks - pcb->persist_last)) * TCP_SLOW_INTERVAL));

      min_wake_time = LWIP_MIN((backoff_cnt - (tcp_ticks - pcb->persist_last)) * TCP_SLOW_INTERVAL, min_wake_time);
    }

    if (pcb->state == FIN_WAIT_1) {
      /* calculate FIN WAIT 1 timeouts */
      LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: fin wait 1 timeout %ldms\n", TCP_FIN_WAIT_TIMEOUT - (tcp_ticks - pcb->fin_wait1_tmr) * TCP_SLOW_INTERVAL));

      min_wake_time = LWIP_MIN(TCP_FIN_WAIT_TIMEOUT - (tcp_ticks - pcb->fin_wait1_tmr) * TCP_SLOW_INTERVAL, min_wake_time);
    }

    if (pcb->state == FIN_WAIT_2 && (pcb->flags & TF_RXCLOSED)) {
      /* calculate FIN WAIT 2 timeouts */
      LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: fin wait 2 timeout %ldms\n", TCP_FIN_WAIT_TIMEOUT - (tcp_ticks - pcb->tmr) * TCP_SLOW_INTERVAL));

      min_wake_time = LWIP_MIN(TCP_FIN_WAIT_TIMEOUT - (tcp_ticks - pcb->tmr) * TCP_SLOW_INTERVAL, min_wake_time);
    }

#if TCP_QUEUE_OOSEQ
    if (pcb->ooseq != NULL) {
      /* calculate ooseq timeouts */
      LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: free ooseq timeout %ldms\n", (pcb->rto * TCP_OOSEQ_TIMEOUT - (tcp_ticks - pcb->tmr)) * TCP_SLOW_INTERVAL));

      min_wake_time = LWIP_MIN((pcb->rto * TCP_OOSEQ_TIMEOUT - (tcp_ticks - pcb->tmr)) * TCP_SLOW_INTERVAL, min_wake_time);
    }
#endif

    if (pcb->state == SYN_RCVD) {
      LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: tcp SYN reset timeout %ldms\n", TCP_SYN_RCVD_TIMEOUT - (tcp_ticks - pcb->tmr) * TCP_SLOW_INTERVAL));
      min_wake_time = LWIP_MIN(TCP_SYN_RCVD_TIMEOUT - (tcp_ticks - pcb->tmr) * TCP_SLOW_INTERVAL, min_wake_time);
    }

    if (pcb->state == LAST_ACK) {
      LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: LAST_ACK timeout %ldms\n", 2 * TCP_MSL - (tcp_ticks - pcb->tmr) * TCP_SLOW_INTERVAL));
      min_wake_time = LWIP_MIN(2 * TCP_MSL - (tcp_ticks - pcb->tmr) * TCP_SLOW_INTERVAL, min_wake_time);
    }

    pcb = pcb->next;
  }

  pcb = tcp_tw_pcbs;
  while (pcb != NULL) {
    LWIP_ASSERT("calculate_next_wake: TIME-WAIT pcb->state == TIME-WAIT", pcb->state == TIME_WAIT);
    LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: TIME-WAIT timeout %ldms\n", 2 * TCP_MSL - (tcp_ticks - pcb->tmr) * TCP_SLOW_INTERVAL));
    min_wake_time = LWIP_MIN(2 * TCP_MSL - (tcp_ticks - pcb->tmr) * TCP_SLOW_INTERVAL, min_wake_time);

    pcb = pcb->next;
  }

  if (min_wake_time != LWIP_UINT32_MAX/4) {
    if (min_wake_time > TCP_MAXIDLE) {
      LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: WARNING! min_wake_time = %ldms, fix to TCP_SLOW_INTERVAL\n", min_wake_time));
      min_wake_time = TCP_SLOW_INTERVAL;
    }
    LWIP_ASSERT("calculate_next_wake: Invalid min_wake_time!\n", min_wake_time >= 0);

    LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: min_wake_time = %ldms\n", min_wake_time));
    *next_wake_ms = (u32_t)min_wake_time;

    return false;
  } else {
    *next_wake_ms = 0;

    return true;
  }
}

static bool tcp_fast_timer_calculate_next_wake(u32_t * next_wake_ms)
{
  struct tcp_pcb *pcb = tcp_active_pcbs;
  while (pcb != NULL) {
    if (((pcb->flags & TF_ACK_DELAY) || (pcb->flags & TF_CLOSEPEND)) || pcb->refused_data != NULL) {
      LWIP_DEBUGF(TCP_DEBUG, ("calculate_next_wake: TCP_FAST_INTERVAL\n"));
      *next_wake_ms = TCP_FAST_INTERVAL;
      return false;
    }

    pcb = pcb->next;
  }

  *next_wake_ms = 0;
  return true;
}
#else /* LWIP_TIMERS && !LWIP_TIMERS_CUSTOM */
/* Satisfy the TCP code which calls this function */
void
tcp_timer_needed(void)
{
}
#endif /* LWIP_TIMERS && !LWIP_TIMERS_CUSTOM */
