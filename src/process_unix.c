/*
   Copyright (C) Cfengine AS

   This file is part of Cfengine 3 - written and maintained by Cfengine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of Cfengine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include "cf3.defs.h"
#include "process_lib.h"
#include "process_unix_priv.h"
#include "misc_lib.h"

#define SLEEP_POLL_TIMEOUT_NS 10000

/*
 * Wait until process specified by #pid is stopped due to SIGSTOP signal.
 *
 * @returns true if process has come to stop during #timeout_ns nanoseconds,
 * false if the process cannot be found or failed to stop during #timeout_ns
 * nanoseconds.
 *
 * FIXME: Only timeouts < 1s are supported
 */
static bool ProcessWaitUntilStopped(pid_t pid, long timeout_ns)
{
    while (timeout_ns > 0)
    {
        switch (GetProcessState(pid))
        {
        case PROCESS_STATE_RUNNING:
            break;
        case PROCESS_STATE_STOPPED:
            return true;
        case PROCESS_STATE_DOES_NOT_EXIST:
            return false;
        default:
            CfOut(cf_error, "", "Unexpected value returned from GetProcessState\n");
            exit(42);
        }

        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = MIN(SLEEP_POLL_TIMEOUT_NS, timeout_ns),
        };

        while (nanosleep(&ts, &ts) < 0)
        {
            if (errno != EINTR)
            {
                CfOut(cf_error, "", "Invalid timeout for nanosleep\n");
                exit(42);
            }
        }

        timeout_ns = MAX(0, timeout_ns - SLEEP_POLL_TIMEOUT_NS);
    }

    return false;
}

/*
 * FIXME: Only timeouts < 1s are supported
 */
static bool ProcessWaitUntilExited(pid_t pid, long timeout_ns)
{
    while (timeout_ns > 0)
    {
        if (kill(pid, 0) < 0 && errno == ESRCH)
        {
            return true;
        }

        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = MIN(SLEEP_POLL_TIMEOUT_NS, timeout_ns),
        };

        while (nanosleep(&ts, &ts) < 0)
        {
            if (errno != EINTR)
            {
                CfOut(cf_error, "", "Invalid timeout for nanosleep\n");
                exit(42);
            }
        }

        timeout_ns = MAX(0, timeout_ns - SLEEP_POLL_TIMEOUT_NS);
    }

    return false;
}

/*
 * A timeout to wait for process to become stopped
 */
#define STOP_WAIT_TIMEOUT 999999999L

/*
 * Safely kill process by checking that the process is the right one by matching
 * process start time.
 *
 * The algorithm:
 *
 *   1. Check that the process has the same start time as stored in lock. If it
 *      is not, return, as we know for sure this is a wrong process. (This step
 *      is an optimization to avoid sending SIGSTOP/SIGCONT to wrong processes).
 *
 *   2. Send SIGSTOP to the process.
 *
 *   3. Poll process state until it is stopped.
 *
 *   Now the process is stopped, so we may examine it and not be afraid that it
 *   will exit and another one with the same PID will appear.
 *
 *   4. Check that the process has the same start time as provided.
 *      If it is, send the signal to the process.
 *
 *   5. Send SIGCONT to the process, so it may continue.
 *
 *
 * Returns 0 on success, -1 on error. Error code is signalled through errno.
 *
 * ERRORS
 *
 *  EINVAL An invalid signal was specified.
 *  EPERM The process does not have permission to send the signal.
 *  ESRCH The pid does not exist or its start time does not match expected one.
 */
static int SafeKill(pid_t pid, time_t expected_start_time, int signal)
{
    /* Preliminary check: in case process start time is different already, we
     * are sure we don't want to STOP it or kill it. */

    time_t pid_start_time = GetProcessStartTime(pid);

    if (pid_start_time != expected_start_time)
    {
        errno = ESRCH;
        return -1;
    }

    /* Now to avoid race conditions we need to stop process so it won't exit
     * voluntarily while we are working on it */

    if (kill(pid, SIGSTOP) < 0)
    {
        return -1;
    }

    if (!ProcessWaitUntilStopped(pid, STOP_WAIT_TIMEOUT))
    {
        /* Ensure the process is started again in case of timeout or error, so
         * we don't leave SIGSTOP'ed processes around on overloaded or
         * misconfigured machine */
        kill(pid, SIGCONT);
        errno = ESRCH;
        return -1;
    }

    /* Here process has stopped, so we may interrogate it without race conditions */

    pid_start_time = GetProcessStartTime(pid);

    if (pid_start_time != expected_start_time)
    {
        /* This is a wrong process, let it continue */
        kill(pid, SIGCONT);
        errno = ESRCH;
        return -1;
    }

    /* We've got a right process, signal it and let it continue */

    int ret = kill(pid, signal);
    int saved_errno = errno;

    /*
     * We don't check return value of SIGCONT, as the proces may have been
     * terminated already by previous kill. Moreover, what would we do with the
     * return code?
     */
    kill(pid, SIGCONT);

    errno = saved_errno;
    return ret;
}

static int Kill(pid_t pid, time_t process_start_time, int signal)
{
    if (process_start_time == PROCESS_START_TIME_UNKNOWN)
    {
        /* We don't know when the process has started, do a plain kill(2) */
        return kill(pid, signal);
    }
    else
    {
        return SafeKill(pid, process_start_time, signal);
    }
}

int GracefulTerminate(pid_t pid, time_t process_start_time)
{
    if (Kill(pid, process_start_time, SIGINT) < 0)
    {
        return errno == ESRCH;
    }

    if (ProcessWaitUntilExited(pid, 999999999L))
    {
        return true;
    }

    if (Kill(pid, process_start_time, SIGTERM) < 0)
    {
        return errno == ESRCH;
    }

    if (ProcessWaitUntilExited(pid, 999999999L))
    {
        return true;
    }

    if (Kill(pid, process_start_time, SIGKILL) < 0)
    {
        return errno == ESRCH;
    }

    return true;
}