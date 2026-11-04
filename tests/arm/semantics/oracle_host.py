#!/usr/bin/env python3
"""
The Python side of the host oracle (host_arm64.c): builds the runner on demand with clang, keeps
one process per worker and runs the states through it. The runner dies with the process when an
instruction breaks it (a signal it can't handle): the wrapper restarts it and reports the line
as an OracleError.

  host = HostOracle()                 # builds out/host_arm64 if needed
  result = host.run(code, state)      # a result dict (armstate.py); OracleError if not runnable
"""
import os
import subprocess
import sys

import armstate as S

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
SOURCE = os.path.join(HERE, 'host_arm64.c')
BINARY = os.path.join(REPO, 'out', 'host_arm64')
# The registers the runner reserves: x28 is the pointer of its state block, x18 the platform
# register. They are not compared against the host
HOST_IGNORE = {'x18', 'x28'}


class OracleError(Exception):
    pass


def available():
    """True on an arm64 macOS host."""
    return sys.platform == 'darwin' and os.uname().machine == 'arm64'


def build():
    """Compile host_arm64.c if the binary is missing or older than the source."""
    if not available():
        raise OracleError('the host oracle needs an arm64 macOS host')
    if not os.path.exists(BINARY) or os.path.getmtime(BINARY) < os.path.getmtime(SOURCE):
        os.makedirs(os.path.dirname(BINARY), exist_ok=True)
        subprocess.check_call(['clang', '-O1', '-Wall', '-o', BINARY, SOURCE])
    return BINARY


class HostOracle(object):
    def __init__(self):
        self.binary = build()
        self.proc = None

    def _start(self):
        self.proc = subprocess.Popen([self.binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)

    def run_line(self, line):
        """One input line, one output line."""
        if self.proc is None or self.proc.poll() is not None:
            self._start()
        try:
            self.proc.stdin.write(line.rstrip('\n') + '\n')
            self.proc.stdin.flush()
            out = self.proc.stdout.readline()
        except (BrokenPipeError, OSError):
            out = ''
        if not out:
            code = self.proc.wait()
            self.proc = None
            return 'ERR host runner died (status %d)' % code
        return out.rstrip('\n')

    def run(self, code, state, arch='a64'):
        line = S.format_state(arch, code, state)
        out = self.run_line(line)
        if state.get('sm'):
            # Observed on the M4 under load (several host processes): now and then (about one
            # line in a thousand) the result of a streaming run has FPSR = state | 0x9f, for any
            # instruction (uzp and umlall included), and the same state run again gives the right
            # result. 0x0800009F is the FPSR a change of the streaming mode sets, so the cause is
            # probably the kernel re-entering the streaming mode of the process when it is
            # rescheduled in the middle of a run (not verified). The result of a state is
            # therefore taken from two identical runs (more runs decide a disagreement)
            seen = {out: 1}
            for _ in range(5):
                again = self.run_line(line)
                seen[again] = seen.get(again, 0) + 1
                best = max(seen.values())
                if best >= 2 and len([v for v in seen.values() if v == best]) == 1:
                    out = [k for k, v in seen.items() if v == best][0]
                    break
        out = out
        res = S.parse_result(arch, out)
        if 'err' in res:
            raise OracleError(res['err'])
        return res

    def close(self):
        if self.proc is not None:
            self.proc.stdin.close()
            self.proc.wait()
            self.proc = None


_host = None


def get_host():
    global _host
    if _host is None:
        _host = HostOracle()
    return _host
