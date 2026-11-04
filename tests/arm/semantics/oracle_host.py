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
        out = self.run_line(S.format_state(arch, code, state))
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
