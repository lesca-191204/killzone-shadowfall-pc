import asyncio
import struct
import unittest
from unittest.mock import patch
from turbine_codec import Frame, encode_frame, decode_frame, encode_blob, encode_join_body, encode_game_body, decode_body
from turbine_relay import Client, TurbineRelay, envelope, read_envelope, HELLO, ACK, DATA


class MemoryWriter:
    def __init__(self, drain=None):
        self.packets = []
        self.closed = False
        self.drain_hook = drain

    def write(self, packet):
        self.packets.append(packet)

    def is_closing(self):
        return self.closed

    async def drain(self):
        if self.drain_hook is not None:
            await self.drain_hook()

    def close(self):
        self.closed = True

    async def wait_closed(self):
        pass


class RelayLifecycleTest(unittest.IsolatedAsyncioTestCase):
    async def test_replacement_master_is_announced_before_departure(self):
        relay = TurbineRelay()
        old = Client(1, {20: MemoryWriter()}, 0, True, phase='ready')
        survivor = Client(2, {20: MemoryWriter()}, 1, True, phase='ready')
        survivor.announced_master = 0
        relay.clients = {1: old, 2: survivor}
        relay.master = 0
        await relay.disconnect(old)
        packets = survivor.channels[20].packets
        frames = [decode_frame(packet[16:]) for packet in packets]
        self.assertEqual([frame.message_type for frame in frames], [0x15, 0x12])
        self.assertEqual(decode_body(frames[0])['master_index'], 1)
        self.assertEqual(frames[1].client_index, 0)
        self.assertTrue(all(struct.unpack_from('>H', packet, 6)[0] == 20 for packet in packets))

    async def test_bandwidth_ids_allow_cross_channel_delivery_order(self):
        relay = TurbineRelay()
        client = Client(1, {20: MemoryWriter(), 21: MemoryWriter()}, phase='ready')
        relay.clients[1] = client
        # Native channel 20 can still contain probes emitted before channel 21
        # delivered CONNECTION_COMPLETED / ENC_VERIFICATION.
        for kind, body in ((0x1e, encode_blob(b'\0')), (0x20, struct.pack('>I', 1000000))):
            await relay.message(client, encode_frame(Frame(kind, 0xffffffff, 255, body)), 20)
        await relay.message(client, encode_frame(Frame(0x1f, 0xffffffff, 255)), 21)
        # Conversely, an actual-ID probe on the other channel can overtake the
        # verification control which sets the native sender's secondary ID.
        client.phase = 'bandwidth'
        await relay.message(client, encode_frame(Frame(0x1e, 1, 255, encode_blob(b'\0'))), 20)
        with self.assertRaisesRegex(ValueError, 'Wrong game id'):
            await relay.message(client, encode_frame(Frame(0x1e, 2, 255, encode_blob(b'\0'))), 20)
        with self.assertRaisesRegex(ValueError, 'Wrong game id'):
            await relay.message(client, encode_frame(Frame(2, 0xffffffff, 255)), 21)

    async def test_failed_replacement_master_is_removed_before_next_election(self):
        relay = TurbineRelay()

        async def fail_drain():
            raise ConnectionResetError('candidate already closed')

        old = Client(1, {20: MemoryWriter()}, 0, True, phase='ready')
        failed = Client(2, {20: MemoryWriter(fail_drain)}, 1, True, phase='ready')
        survivor = Client(3, {20: MemoryWriter()}, 2, True, phase='ready')
        failed.announced_master = survivor.announced_master = 0
        relay.clients = {1: old, 2: failed, 3: survivor}
        relay.master = 0
        await relay.disconnect(old)
        self.assertEqual(relay.clients, {3: survivor})
        self.assertEqual(relay.master, 2)
        frames = [decode_frame(packet[16:]) for packet in survivor.channels[20].packets]
        self.assertEqual([frame.message_type for frame in frames], [0x15, 0x12, 0x12])
        self.assertEqual(decode_body(frames[0])['master_index'], 2)
        self.assertEqual([frame.client_index for frame in frames[1:]], [1, 0])
        self.assertFalse(survivor.channels[20].closed)

    async def test_native_game_body_and_channel_are_preserved(self):
        relay = TurbineRelay()
        source = Client(1, {20: MemoryWriter(), 21: MemoryWriter()}, 0, True, phase='ready')
        target = Client(2, {20: MemoryWriter(), 21: MemoryWriter()}, 1, True, phase='ready')
        other = Client(3, {20: MemoryWriter(), 21: MemoryWriter()}, 2, True, phase='ready')
        relay.clients = {1: source, 2: target, 3: other}
        payload = struct.pack('<HHi', 0x8006, 11, 3) + b'abc'
        body = encode_game_body(False, bytes([1]), payload)
        await relay.message(source, encode_frame(Frame(0x28, 1, 0, body)), 21)
        self.assertFalse(source.channels[21].packets)
        self.assertFalse(other.channels[21].packets)
        self.assertFalse(target.channels[20].packets)
        packet, = target.channels[21].packets
        self.assertEqual(struct.unpack_from('>H', packet, 6)[0], 21)
        self.assertEqual(decode_frame(packet[16:]), Frame(0x28, 1, 0, body))

    async def test_simultaneous_channels_start_handshake_once(self):
        relay = TurbineRelay()
        gate = asyncio.Event()
        arrivals = 0

        async def ack_barrier():
            nonlocal arrivals
            arrivals += 1
            if arrivals == 2:
                gate.set()
            await gate.wait()

        readers = [asyncio.StreamReader(), asyncio.StreamReader()]
        writers = [MemoryWriter(ack_barrier), MemoryWriter(ack_barrier)]
        for channel, reader in zip((20, 21), readers):
            reader.feed_data(envelope(HELLO, channel, struct.pack('>QHH', 42, channel, 0)))
        tasks = [asyncio.create_task(relay.accept(reader, writer)) for reader, writer in zip(readers, writers)]
        try:
            await asyncio.wait_for(gate.wait(), 1)
            await asyncio.sleep(0.01)  # Both ACK drain continuations get a turn.
            probes = [decode_frame(packet[16:]) for writer in writers for packet in writer.packets
                      if packet[5] == DATA]
            self.assertEqual([frame.message_type for frame in probes], [7])
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

    async def test_failed_recipient_does_not_disconnect_sender(self):
        relay = TurbineRelay()

        async def fail_drain():
            raise ConnectionResetError('recipient closed')

        source_writer, failed_writer = MemoryWriter(), MemoryWriter(fail_drain)
        source = Client(1, {20: source_writer}, 0, True, phase='ready')
        failed = Client(2, {20: failed_writer}, 1, True, phase='ready')
        source.announced_master = failed.announced_master = 0
        relay.clients = {1: source, 2: failed}
        relay.master = 0
        packet = encode_frame(Frame(0x28, 1, 0, encode_game_body(True, b'', b'payload')))
        await relay.message(source, packet, 20)
        self.assertEqual(relay.clients, {1: source})
        self.assertFalse(source_writer.closed)
        self.assertTrue(failed_writer.closed)
        self.assertEqual([decode_frame(packet[16:]).message_type for packet in source_writer.packets], [0x12])

    async def test_stale_channel_cleanup_preserves_reconnected_owner(self):
        relay = TurbineRelay()
        old = Client(5, {20: MemoryWriter(), 21: MemoryWriter()})
        relay.clients[5] = old
        await relay.disconnect(old)
        new = Client(5, {20: MemoryWriter()})
        relay.clients[5] = new
        await relay.disconnect(old)  # The old second channel finishes after reconnection.
        self.assertIs(relay.clients.get(5), new)
        self.assertFalse(new.channels[20].closed)


class RelayTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.relay = TurbineRelay()
        self.server = await asyncio.start_server(self.relay.accept, '127.0.0.1', 0)
        self.address = self.server.sockets[0].getsockname()
        self.connections = []

    async def asyncTearDown(self):
        self.server.close()
        for writer in self.connections:
            writer.close()
        await asyncio.wait_for(asyncio.gather(*(writer.wait_closed() for writer in self.connections)), 2)
        # Python 3.12+ waits for active transports too, so close clients before this wait.
        await asyncio.wait_for(self.server.wait_closed(), 2)

    async def receive(self, reader):
        kind, channel, flags, packet = await asyncio.wait_for(read_envelope(reader), 2)
        self.assertEqual(kind, DATA)
        return decode_frame(packet)

    async def send(self, writer, kind, body=b'', index=255):
        game_id = 0xffffffff if kind in (0x17, 0x21) or (index == 255 and kind in (0x1e, 0x1f, 0x20)) else 1
        writer.write(envelope(DATA, 20, encode_frame(Frame(kind, game_id, index, body)), 0x40))
        await writer.drain()

    async def connect_client(self, nonce):
        readers, writers = {}, {}
        for channel in (20, 21):
            reader, writer = await asyncio.open_connection(*self.address)
            self.connections.append(writer)
            readers[channel], writers[channel] = reader, writer
            hello = envelope(HELLO, channel, struct.pack('>QHH', nonce, 10000 + channel - 20, 0))
            # TCP segmentation must not be confused with message boundaries.
            writer.write(hello[:7])
            await writer.drain()
            writer.write(hello[7:])
            await writer.drain()
            kind, got_channel, flags, body = await asyncio.wait_for(read_envelope(reader), 2)
            self.assertEqual((kind, got_channel, flags, body), (ACK, channel, 0, b''))
        reader, writer = readers[20], writers[20]
        probe = await self.receive(reader)
        self.assertEqual((probe.message_type, len(decode_body(probe)['data'])), (7, 1333))
        await self.send(writer, 0x17, encode_blob(b'\x01' + bytes(1332)))
        self.assertEqual((await self.receive(reader)).message_type, 8)
        await self.send(writer, 0x1F)
        types = [(await self.receive(reader)).message_type for _ in range(22)]
        self.assertEqual(types, [0x20] + [0x1E] * 20 + [0x1F])
        await self.send(writer, 0x20, struct.pack('>I', 1000000))
        await self.send(writer, 0x21)
        await self.send(writer, 2)
        self.assertEqual((await self.receive(reader)).message_type, 0x0F)
        name = f'Player{nonce}'.encode()
        await self.send(writer, 3, encode_join_body(name + b'\0', name.ljust(32, b'\0')))
        joined = await self.receive(reader)
        self.assertEqual(joined.message_type, 0x10)
        peers = decode_body(joined)['value']
        for _ in range(peers):
            roster = await self.receive(reader)
            self.assertEqual(roster.message_type, 0x11)
            self.assertEqual(decode_body(roster)['mss'], 1410)
        self.assertEqual((await self.receive(reader)).message_type, 0x15)
        await self.send(writer, 0x23, index=joined.client_index)
        return reader, writer, joined.client_index

    async def test_joined_client_can_repeat_bandwidth_measurement(self):
        reader, writer, index = await self.connect_client(8)
        await self.send(writer, 0x1e, encode_blob(b'\0' + bytes(1332)), index=index)
        await self.send(writer, 0x1f, index=index)
        frames = [await self.receive(reader) for _ in range(22)]
        self.assertEqual([frame.message_type for frame in frames], [0x20] + [0x1e] * 20 + [0x1f])
        self.assertTrue(all(frame.client_index == 255 for frame in frames))
        await self.send(writer, 0x20, struct.pack('>I', 1000000), index=index)
        await self.send(writer, 0x17, encode_blob(b'\x01' + bytes(1332)), index=index)
        mtu_done = await self.receive(reader)
        self.assertEqual((mtu_done.message_type, mtu_done.client_index), (8, 255))

    async def test_two_clients_forward_and_disconnect(self):
        first, writer1, index1 = await self.connect_client(1)
        second, writer2, index2 = await self.connect_client(2)
        self.assertNotEqual(index1, index2)
        self.assertEqual((await self.receive(first)).message_type, 0x11)
        payload = b'\0\xffopaque-game-state\x80'
        body = encode_game_body(True, b'', payload)
        await self.send(writer1, 0x28, body, index1)
        forwarded = await self.receive(second)
        self.assertEqual((forwarded.client_index, forwarded.body), (index1, body))
        body2 = encode_game_body(False, bytes([index1]), b'reply')
        await self.send(writer2, 0x29, body2, index2)
        self.assertEqual((await self.receive(first)).body, body2)
        await self.send(writer2, 4, index=index2)
        left = await self.receive(first)
        self.assertEqual((left.message_type, left.client_index), (0x12, index2))
        # Existing sender must not receive its own broadcast.
        with self.assertRaises(asyncio.TimeoutError):
            await asyncio.wait_for(read_envelope(first), 0.05)

    async def test_malformed_length_is_rejected_before_body_read(self):
        reader, writer = await asyncio.open_connection(*self.address)
        self.connections.append(writer)
        writer.write(struct.pack('>4sBBHII', b'KZRP', 1, DATA, 20, 0xffffffff, 0x40))
        await writer.drain()
        self.assertEqual(await asyncio.wait_for(reader.read(), 2), b'')
        self.assertFalse(self.relay.clients)

    async def test_active_client_keeps_silent_second_channel(self):
        original_wait_for = asyncio.wait_for

        async def accelerate_idle(awaitable, timeout):
            return await original_wait_for(awaitable, 0.5 if timeout == 30 else timeout)

        with patch('turbine_relay.asyncio.wait_for', new=accelerate_idle):
            _, writer, index = await self.connect_client(7)
            for _ in range(20):
                await self.send(writer, 1, index=index)
                await asyncio.sleep(0.05)
                self.assertIn(7, self.relay.clients)
            self.assertFalse(self.connections[-1].is_closing())


if __name__ == '__main__':
    unittest.main()
