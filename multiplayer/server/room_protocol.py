"""Bounded control framing shared by the TLS room gateway and local bridge."""
import asyncio
from collections import deque
import json
import struct
import time

MAX_JSON = 16384
MAX_BODY = 4 * 1024 * 1024
IO_TIMEOUT = 8


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate control field')
        result[key] = value
    return result


async def read_json(reader, timeout=IO_TIMEOUT, *, allow_idle=False):
    # Leased channels may be quiet; a started frame still has a fixed deadline.
    first = await reader.readexactly(1) if allow_idle else b''
    async def read():
        size, = struct.unpack('>I', first + await reader.readexactly(4 - len(first)))
        if not 1 <= size <= MAX_JSON:
            raise ValueError('Invalid control length')
        try:
            value = json.loads((await reader.readexactly(size)).decode('utf-8'), object_pairs_hook=_object)
        except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
            raise ValueError('Invalid control message') from error
        if not isinstance(value, dict):
            raise ValueError('Invalid control message')
        return value
    return await asyncio.wait_for(read(), timeout)


async def write_json(writer, value):
    data = json.dumps(value, separators=(',', ':'), ensure_ascii=True).encode('ascii')
    if not 1 <= len(data) <= MAX_JSON:
        raise ValueError('Invalid control length')
    await write_data(writer, struct.pack('>I', len(data)) + data)


async def write_data(writer, data):
    # Python's SSL transport may already have released its protocol on close.
    # Check before write, without yielding between the check and the write.
    if writer.is_closing():
        raise ConnectionError('Connection closed')
    writer.write(data)
    await asyncio.wait_for(writer.drain(), IO_TIMEOUT)


def body_size(value):
    if type(value) is not int or not 0 <= value <= MAX_BODY:
        raise ValueError('Invalid body size')
    return value


async def close_writer(writer):
    writer.close()
    try:
        await asyncio.wait_for(writer.wait_closed(), 1)
    except (OSError, TimeoutError):
        pass


class RateLimit:
    """Sliding-window counter; callers reject before doing costly work."""
    def __init__(self, maximum, interval=60):
        self.maximum, self.interval = maximum, interval
        self.events = deque()

    def take(self):
        now = time.monotonic()
        while self.events and self.events[0] <= now - self.interval:
            self.events.popleft()
        if len(self.events) >= self.maximum:
            return False
        self.events.append(now)
        return True


class ByteLimit:
    def __init__(self, rate=8 * 1024 * 1024):
        self.rate = rate
        self.tokens = rate
        self.updated = time.monotonic()

    def take(self, count):
        now = time.monotonic()
        self.tokens = min(self.rate, self.tokens + (now - self.updated) * self.rate)
        self.updated = now
        if count > self.tokens:
            return False
        self.tokens -= count
        return True
