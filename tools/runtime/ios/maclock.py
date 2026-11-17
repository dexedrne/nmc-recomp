#!/usr/bin/env python3
"""maclock.py : run ONE command on the MacBook Air while holding the shared build lock ~/Developer/.mac-build.lock.

UNRUN as a whole: this wrapper has never run on that Mac (the machine was withdrawn before the first build step).  What WAS
measured there is the lock behaviour described below; and the wrapper's own logic (busy cases, the line in the file, release on
exit and on a signal) was exercised on Linux against a scratch lock file (KHNMC_MACLOCK_FILE, tools/ios/test_maclock.sh).

  python3 maclock.py [--what TEXT] -- COMMAND [ARG ...]

Runs ON THE MAC (Python 3.9, standard library).  Every build and every simulator step of tools/ios/*.sh goes through it.

Two conventions are in use for that file on this Mac, and both are honoured:
  * an fcntl lock on the file.  macOS keeps POSIX (fcntl / lockf) and BSD (flock) locks in ONE list per file: measured there on
    2026-11-17 with a scratch file, a process that holds lockf(LOCK_EX) makes BOTH lockf and flock of another process fail with
    EAGAIN (and its own flock on the same descriptor too, so the two cannot be taken together).  One lockf(LOCK_EX|LOCK_NB) for the
    duration of the command therefore excludes both kinds of user;
  * a description written INTO the file means busy.  Content that is not ours: stop.  While the command runs the file holds
    one line `khnmc iOS build, started <UTC time>` (plus what the step is).
Before taking it: `pgrep -x xcodebuild`; anything printed means someone is building: stop.
"Stop" is exit status 75 with a line that says why.  Nothing waits and nothing is retried here: the caller decides.
On exit, also on SIGHUP / SIGTERM / SIGINT (an ssh session that drops): the command's process group gets SIGTERM (its own traps
run: the simulator scripts delete their simulator there), then the file is truncated and removed, as the Mac's rules ask.
An EMPTY existing file is the leftover of an fcntl-style user and means free.
Exit status: the command's; 75 = busy, not started; 2 = usage.
"""
import errno
import fcntl
import os
import signal
import subprocess
import sys
import time

LOCK = os.environ.get('KHNMC_MACLOCK_FILE') or os.path.expanduser('~/Developer/.mac-build.lock')   # the variable: tests only
MARK = 'khnmc iOS build, started '
EX_BUSY = 75


def busy(msg):
    sys.stderr.write('maclock: BUSY, not started: %s\n' % msg)
    sys.exit(EX_BUSY)


def main():
    argv = sys.argv[1:]
    what = ''
    if len(argv) >= 2 and argv[0] == '--what':
        what = argv[1]
        argv = argv[2:]
    if not argv or argv[0] != '--' or len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    cmd = argv[1:]

    pg = subprocess.run(['pgrep', '-x', 'xcodebuild'], stdout=subprocess.PIPE, universal_newlines=True)
    if pg.stdout.strip():
        busy('xcodebuild is running (pid %s): someone is building' % ' '.join(pg.stdout.split()))

    if not os.path.isdir(os.path.dirname(LOCK)):
        sys.stderr.write('maclock: %s does not exist\n' % os.path.dirname(LOCK))
        return 2
    fd = os.open(LOCK, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as e:
        if e.errno in (errno.EAGAIN, errno.EACCES):
            os.close(fd)
            busy('%s is locked by another process (fcntl or flock)' % LOCK)
        raise
    os.lseek(fd, 0, os.SEEK_SET)
    content = os.read(fd, 4096).decode('utf-8', 'replace').strip()
    if content and not content.startswith(MARK):
        os.close(fd)                                    # closing drops our lock; the file and its content are not touched
        busy('%s holds a description that is not ours: %r' % (LOCK, content[:200]))
    if content:
        sys.stderr.write('maclock: note: the lock file held OUR line from a step that did not clean up (%r); no process holds its lock: taking over\n' % content[:120])
    line = '%s%s%s\n' % (MARK, time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), (' (%s)' % what) if what else '')
    os.ftruncate(fd, 0)
    os.lseek(fd, 0, os.SEEK_SET)
    os.write(fd, line.encode())
    os.fsync(fd)

    child = None
    state = {'sig': 0}

    def release():
        try:
            os.ftruncate(fd, 0)
            st, here = os.fstat(fd), None
            try:
                here = os.stat(LOCK)
            except OSError:
                pass
            if here is not None and (here.st_ino, here.st_dev) == (st.st_ino, st.st_dev):
                os.unlink(LOCK)                         # only OUR file: somebody may have replaced the path meanwhile
        finally:
            os.close(fd)

    def on_signal(signum, _frame):
        state['sig'] = signum
        if child is not None and child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGTERM)    # the step's own traps run (delete the simulator, ...)
            except OSError:
                pass

    for s in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT):
        signal.signal(s, on_signal)
    rc = 1
    try:
        child = subprocess.Popen(cmd, start_new_session=True)
        while True:
            try:
                rc = child.wait(timeout=1)
                break
            except subprocess.TimeoutExpired:
                if state['sig']:
                    try:
                        rc = child.wait(timeout=120)    # time for the step's cleanup
                    except subprocess.TimeoutExpired:
                        sys.stderr.write('maclock: the step did not end within 120 s of signal %d: killing its process group\n' % state['sig'])
                        try:
                            os.killpg(child.pid, signal.SIGKILL)
                        except OSError:
                            pass
                        rc = child.wait()
                    break
    except OSError as e:
        sys.stderr.write('maclock: cannot run %r: %s\n' % (cmd[0], e))
        rc = 127
    finally:
        release()
    if state['sig']:
        sys.stderr.write('maclock: ended by signal %d; lock released\n' % state['sig'])
        return 128 + state['sig']
    return rc if rc >= 0 else 128 - rc


if __name__ == '__main__':
    sys.exit(main())
