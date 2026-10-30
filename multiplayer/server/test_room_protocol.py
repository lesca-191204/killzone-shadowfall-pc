"""Idle authenticated channels retain bounded control-frame parsing."""
import asyncio
import json
import struct
import unittest

from room_protocol import MAX_JSON, read_json


class ReadJSONTests(unittest.IsolatedAsyncioTestCase):
    async def test_allow_idle_waits_for_a_message_then_reads_the_whole_frame(self):
        reader = asyncio.StreamReader()
        task = asyncio.create_task(read_json(reader, 0.05, allow_idle=True))
        self.addAsyncCleanup(self.cancel, task)
        await asyncio.sleep(0.15)
        self.assertFalse(task.done())
        body = json.dumps({'ok': True}).encode()
        frame = struct.pack('>I', len(body)) + body
        reader.feed_data(frame + frame)
        self.assertEqual(await asyncio.wait_for(task, 1), {'ok': True})
        self.assertEqual(await read_json(reader, 0.05, allow_idle=True), {'ok': True})

    @staticmethod
    async def cancel(task):
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)

    async def test_default_read_still_times_out_without_any_bytes(self):
        with self.assertRaises(TimeoutError):
            await read_json(asyncio.StreamReader(), 0.05)

    async def test_idle_mode_keeps_partial_headers_and_bodies_bounded(self):
        for partial in (b'\0', struct.pack('>I', 20) + b'{'):
            with self.subTest(partial=partial):
                reader = asyncio.StreamReader()
                reader.feed_data(partial)
                with self.assertRaises(TimeoutError):
                    await read_json(reader, 0.05, allow_idle=True)

    async def test_idle_eof_finishes_instead_of_hanging(self):
        reader = asyncio.StreamReader()
        reader.feed_eof()
        with self.assertRaises(asyncio.IncompleteReadError):
            await asyncio.wait_for(read_json(reader, allow_idle=True), 1)

    async def test_idle_mode_preserves_size_and_json_validation(self):
        for frame in (struct.pack('>I', 0), struct.pack('>I', MAX_JSON + 1),
                      struct.pack('>I', 2) + b'[]',
                      struct.pack('>I', 13) + b'{"a":1,"a":2}'):
            with self.subTest(frame=frame):
                reader = asyncio.StreamReader()
                reader.feed_data(frame)
                with self.assertRaises(ValueError):
                    await read_json(reader, 0.05, allow_idle=True)
