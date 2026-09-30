# SPDX-License-Identifier: GPL-2.0-only
"""Disposable PTY screen-state relay for the native terminal transfer tests.

It drops the native byte-stream probe and maps title updates into the shape
observed through Mosh. No network, user session, or installed binary is used.
"""
import fcntl
import os
import pty
import select
import signal
import subprocess
import sys
import termios
import tty


class OutputFilter:
    def __init__(self):
        self.pending = bytearray()
        self.dropped = False
        self.duplicated = False
        self.drop_first = bool(os.getenv("SCREEN_FIXTURE_DROP_FIRST"))
        self.drop_all = bool(os.getenv("SCREEN_FIXTURE_DROP_ALL"))
        self.duplicate = bool(os.getenv("SCREEN_FIXTURE_DUPLICATE"))
        self.replay = bool(os.getenv("SCREEN_FIXTURE_REPLAY"))
        self.first_frame = None
        self.replayed = False
        self.replay_hello = bool(os.getenv("SCREEN_FIXTURE_REPLAY_HELLO"))
        self.hello = None
        self.hello_replayed = False

    def hello_replay(self, result, phase):
        result.extend(self.hello)
        marker = os.getenv("SCREEN_FIXTURE_HELLO_REPLAYED")
        if marker:
            with open(marker, "ab") as replayed:
                replayed.write(phase + b"\n")

    def mark_dropped(self):
        marker = os.getenv("SCREEN_FIXTURE_DROPPED")
        if marker:
            with open(marker, "wb") as dropped:
                dropped.write(b"1")

    def output(self, data):
        result = bytearray()
        probe = b"\x1b[?9001;"
        for byte in data:
            if not self.pending:
                if byte == 27:
                    self.pending.append(byte)
                else:
                    result.append(byte)
                continue
            self.pending.append(byte)
            current = bytes(self.pending)
            if current.startswith(b"\x1b]2;"):
                if byte == 7 or current.endswith(b"\x1b\\"):
                    title = current[4:-1] if byte == 7 else current[4:-2]
                    if title.startswith(b"SNAJPAGENT-SCREEN/1:"):
                        title = b"[mosh] " + title[:245]
                        framed = b"\x1b]2;" + title + b"\x07"
                        data_frame = b":DATA:" in title
                        if b":HELLO:" in title:
                            self.hello = framed
                        if (self.replay_hello and self.hello and data_frame
                                and not self.hello_replayed):
                            self.hello_replay(result, b"active")
                            self.hello_replayed = True
                        if self.drop_all and data_frame:
                            if not self.dropped:
                                self.dropped = True
                                self.mark_dropped()
                        elif self.drop_first and data_frame and not self.dropped:
                            self.dropped = True
                            self.mark_dropped()
                        else:
                            if (self.replay and self.first_frame and not self.replayed
                                    and b":00000002:" in title):
                                result.extend(self.first_frame)
                                self.replayed = True
                                marker = os.getenv("SCREEN_FIXTURE_REPLAYED")
                                if marker:
                                    with open(marker, "wb") as replayed:
                                        replayed.write(b"1")
                            result.extend(framed)
                            if data_frame and self.first_frame is None:
                                self.first_frame = framed
                            if self.duplicate and data_frame and not self.duplicated:
                                result.extend(framed)
                                self.duplicated = True
                        if self.replay_hello and self.hello and b":END:" in title:
                            self.hello_replay(result, b"ended")
                    else:
                        result.extend(current)
                    self.pending.clear()
                elif len(current) > 512:
                    result.extend(current)
                    self.pending.clear()
            elif probe.startswith(current):
                continue
            elif current.startswith(probe) and len(current) <= 30:
                if byte == ord("n"):
                    self.pending.clear()
                elif not (ord("0") <= byte <= ord("9")):
                    result.extend(current)
                    self.pending.clear()
            elif b"\x1b]2;".startswith(current):
                continue
            else:
                result.extend(current)
                self.pending.clear()
        return result


def main():
    original = termios.tcgetattr(0)
    tty.setraw(0)
    master, slave = pty.openpty()

    def resize(_number=None, _frame=None):
        try:
            size = fcntl.ioctl(0, termios.TIOCGWINSZ, b"\0" * 8)
            fcntl.ioctl(master, termios.TIOCSWINSZ, size)
            path = os.getenv("SCREEN_FIXTURE_WINSIZE")
            if path:
                with open(path, "wb") as output:
                    output.write(size)
        except OSError:
            pass

    resize()
    signal.signal(signal.SIGWINCH, resize)

    def setup():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

    child = subprocess.Popen(sys.argv[1:], stdin=slave, stdout=slave, stderr=slave,
                             preexec_fn=setup)
    os.close(slave)
    output = OutputFilter()
    try:
        while True:
            ready, _, _ = select.select([0, master], [], [], 0.2)
            if master in ready:
                try:
                    received = os.read(master, 8192)
                except OSError:
                    break
                if not received:
                    break
                filtered = output.output(received)
                if filtered:
                    os.write(1, filtered)
            if 0 in ready:
                received = os.read(0, 8192)
                if not received:
                    break
                os.write(master, received)
            if child.poll() is not None and not ready:
                break
    finally:
        if child.poll() is None:
            child.terminate()
        child.wait(timeout=5)
        os.close(master)
        termios.tcsetattr(0, termios.TCSANOW, original)
    return child.returncode if child.returncode >= 0 else 128 - child.returncode


if __name__ == "__main__":
    sys.exit(main())
