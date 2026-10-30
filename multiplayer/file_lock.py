"""Nonblocking file locks released by the OS even when a process is terminated."""
from contextlib import contextmanager
import ctypes
import os
from pathlib import Path


@contextmanager
def exclusive_file_lock(path):
    """Keep the file in place: deleting it can let another process lock a new inode."""
    path = Path(path)
    if os.name == 'nt':
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.CloseHandle.restype = ctypes.c_int
        # GENERIC_READ|WRITE, FileShare.None, OPEN_ALWAYS: existing unlocked files are reusable.
        handle = kernel.CreateFileW(str(path), 0xc0000000, 0, None, 4, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            yield
        finally:
            kernel.CloseHandle(handle)
    else:
        import fcntl
        with path.open('a+b') as handle:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                yield
            finally:
                fcntl.flock(handle, fcntl.LOCK_UN)
