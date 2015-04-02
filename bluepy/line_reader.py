import os
import time
import select

class LineReader(object):
    """Read line by line from the file descriptor
       Improved from http://stackoverflow.com/questions/5486717/python-select-doesnt-signal-all-input-from-pipe"""
    def __init__(self, fd):
        self._fd = fd
        self._buf = b''
        self._poller = select.poll()
        self._poller.register(fd, select.POLLIN)

    def fileno(self):
        return self._fd

    def readline(self, timeout=None):
        """Get next line from the file descriptor.
           If there is a line already buffered, it will be returned straight away.
           Otherwise select.poll() will be called with timeout"""
        ret = self._nextline()

        # poll() when there is no line available
        start_time = time.time()
        while ret is None and (timeout is None or time.time() - start_time < timeout):
            fds = self._poller.poll(-1 if timeout is None else timeout * 1000)
            if len(fds) == 0:
                DBG("Select timeout")
                return None

            data = os.read(self._fd, 4096)
            if not data:
                return None
            self._buf += data
            ret = self._nextline()
        return ret

    def _nextline(self):
        """Try to get a line from the buffered bytes"""
        if b'\n' not in self._buf:
            return None
        tmp = self._buf.split(b'\n')
        line = tmp[0]
        self._buf = self._buf[len(line) + 1:]
        return line

    def __del__(self):
        self._poller.unregister(self._fd)
