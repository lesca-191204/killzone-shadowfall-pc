"""Loopback Turbine relay experiment; game simulation remains in the clients."""
import asyncio
from dataclasses import dataclass, field
import struct
import time
from player_identity import MAX_PLAYERS
from turbine_codec import Frame, decode_frame, decode_body, encode_frame, encode_blob

ENVELOPE = struct.Struct('>4sBBHII')
HELLO, ACK, DATA = 1, 2, 3
MAX_PACKET = 65535


def envelope(kind, channel, body=b'', flags=0):
    if kind not in (HELLO, ACK, DATA) or channel not in (20, 21) or len(body) > MAX_PACKET:
        raise ValueError('Invalid relay envelope')
    return ENVELOPE.pack(b'KZRP', 1, kind, channel, len(body), flags) + body


async def read_envelope(reader):
    magic, version, kind, channel, size, flags = ENVELOPE.unpack(await reader.readexactly(16))
    if magic != b'KZRP' or version != 1 or kind not in (HELLO, ACK, DATA) or channel not in (20, 21) or size > MAX_PACKET:
        raise ValueError('Invalid relay header')
    if (kind == HELLO and (size != 12 or flags)) or (kind == ACK and (size or flags)):
        raise ValueError('Invalid relay control frame')
    if kind == DATA and (not size or flags not in (0x40, 0x48)):
        raise ValueError('Invalid relay data frame')
    return kind, channel, flags, await reader.readexactly(size)


@dataclass
class Client:
    nonce: int
    channels: dict = field(default_factory=dict)
    index: int = 255
    joined: bool = False
    user_data: bytes = b''
    phase: str = 'connecting'
    announced_master: int = 255


class TurbineRelay:
    def __init__(self, game_id=1):
        self.game_id = game_id
        self.clients = {}
        self.master = 255
        self.started = time.monotonic()

    async def send(self, client, kind, body=b'', *, index=None, channel=20):
        writer = client.channels.get(channel)
        if writer is None or writer.is_closing():
            raise ConnectionError('Missing client channel')
        # Even an unchanged native NEW_MASTER overwrites its previous-master ID.
        if kind == 0x15 and client.announced_master == body[0]:
            return
        frame = Frame(kind, self.game_id, client.index if index is None else index, body)
        writer.write(envelope(DATA, channel, encode_frame(frame), 0x40))
        if kind == 0x15:
            client.announced_master = body[0]
        await asyncio.wait_for(writer.drain(), 2)

    async def send_peer(self, peer, kind, body=b'', *, index=None, channel=20):
        """A failed recipient must not terminate the sender's connection handler."""
        if self.clients.get(peer.nonce) is not peer:
            return False
        try:
            await self.send(peer, kind, body, index=index, channel=channel)
            return True
        except (ConnectionError, TimeoutError):
            await self.disconnect(peer)
            return False

    async def join(self, client, body):
        if client.joined or client.phase != 'ready':
            raise ValueError('JOIN out of sequence')
        if len(body['user_data']) > 4096 or len(body['client_info']) != 32:
            raise ValueError('Invalid JOIN data')
        occupied = {peer.index for peer in self.clients.values() if peer.joined}
        index = next((i for i in range(MAX_PLAYERS) if i not in occupied), None)
        if index is None:
            raise ValueError('Server full')
        client.index = index
        client.user_data = body['user_data']
        client.joined = True
        peers = [peer for peer in self.clients.values() if peer.joined]
        await self.send(client, 0x10, struct.pack('>H', len(peers)))
        for peer in peers:
            roster = struct.pack('>HI', 1410, 1000000) + encode_blob(peer.user_data)
            await self.send(client, 0x11, roster, index=peer.index)
            if peer is not client:
                own_roster = struct.pack('>HI', 1410, 1000000) + encode_blob(client.user_data)
                await self.send_peer(peer, 0x11, own_roster, index=client.index)
        if self.master == 255:
            self.master = client.index
        for peer in peers:
            await self.send_peer(peer, 0x15, bytes([self.master]))
        print(f'TURBINE joined index={client.index} peers={len(peers)}', flush=True)

    async def message(self, client, packet, channel):
        frame = decode_frame(packet)
        kind = frame.message_type
        # The native sender changes its secondary ID when it sends ENC_VERIFICATION.
        # Its two channels have no shared delivery order: old sentinel probes can
        # arrive after that control, and new actual-ID probes can overtake it.
        if kind in (0x1e, 0x1f, 0x20):
            valid_ids = (0xffffffff, self.game_id)
        elif kind in (0x17, 0x21):
            valid_ids = (0xffffffff,)
        else:
            valid_ids = (self.game_id,)
        if frame.game_id not in valid_ids:
            raise ValueError('Wrong game id')
        body = decode_body(frame)
        if kind != 1:
            print(f'TURBINE receive type={kind:#x} channel={channel} bytes={len(frame.body)} phase={client.phase}', flush=True)
        if kind == 0x17 and client.phase in ('mtu', 'ready'):
            data = body['data']
            if len(data) != 1333 or data[0] != 1:
                raise ValueError('Unexpected MTU response')
            if client.phase == 'mtu':
                client.phase = 'bandwidth'
            await self.send(client, 8, struct.pack('>H', 1333), index=255)
        elif kind == 0x1E and client.phase in ('bandwidth', 'ready'):
            pass  # Measurement packets; opaque padding is deliberately ignored.
        elif kind == 0x1F and client.phase in ('bandwidth', 'ready'):
            await self.send(client, 0x20, struct.pack('>I', 1000000), index=255)
            for ordinal in range(20):
                await self.send(client, 0x1E, encode_blob(bytes([ordinal]) + bytes(1332)), index=255)
            await self.send(client, 0x1F, index=255)
        elif kind == 0x20 and client.phase in ('bandwidth', 'ready'):
            pass
        elif kind == 0x21 and client.phase == 'bandwidth':
            client.phase = 'ready'
        elif kind == 2 and client.phase == 'ready':
            await self.send(client, 0x0F, encode_blob(b''))
        elif kind == 3:
            await self.join(client, body)
        elif kind == 6:
            milliseconds = int((time.monotonic() - self.started) * 1000) & 0xffffffff
            await self.send(client, 0x16, struct.pack('>I', milliseconds))
        elif kind == 1:
            pass
        elif kind == 0x23 and client.joined:
            if frame.client_index != client.index:
                raise ValueError('Wrong sender index')
        elif kind == 4:
            raise ConnectionError('Client left')
        elif kind >= 0x28 and client.joined:
            if frame.client_index != client.index:
                raise ValueError('Wrong sender index')
            recipients = body['recipients']
            for peer in list(self.clients.values()):
                if peer.joined and peer is not client and (body['broadcast'] or peer.index in recipients):
                    await self.send_peer(peer, kind, frame.body, index=client.index, channel=channel)
        else:
            raise ValueError(f'Unsupported message {kind:#x} in {client.phase}')

    async def disconnect(self, client):
        if self.clients.get(client.nonce) is not client:
            return
        del self.clients[client.nonce]
        for writer in client.channels.values():
            writer.close()
        if not client.joined:
            return
        peers = [peer for peer in self.clients.values() if peer.joined]
        if self.master == client.index:
            self.master = min((peer.index for peer in peers), default=255)
        for peer in peers:
            # A failed candidate must leave the registry and trigger another
            # election, otherwise surviving clients retain a disconnected master.
            # Native CLIENT_LEFT migrates object ownership to its current master.
            if await self.send_peer(peer, 0x15, bytes([self.master])):
                await self.send_peer(peer, 0x12, struct.pack('>I', 0), index=client.index)

    async def accept(self, reader, writer):
        client = None
        try:
            kind, channel, flags, hello = await asyncio.wait_for(read_envelope(reader), 5)
            if kind != HELLO:
                raise ValueError('HELLO required')
            nonce, local, reserved = struct.unpack('>QHH', hello)
            if not nonce or not local or reserved:
                raise ValueError('Invalid HELLO')
            client = self.clients.get(nonce)
            if client is None:
                if len(self.clients) >= MAX_PLAYERS:
                    raise ValueError('Server full')
                client = self.clients[nonce] = Client(nonce)
            if channel in client.channels:
                client = None
                raise ValueError('Duplicate channel')
            client.channels[channel] = writer
            writer.write(envelope(ACK, channel))
            await asyncio.wait_for(writer.drain(), 2)
            if len(client.channels) == 2 and client.phase == 'connecting':
                client.phase = 'mtu'
                await self.send(client, 7, encode_blob(bytes(1333)))
            while True:
                # A quiet reliable/unreliable channel is valid; native RUDP keepalive is not DATA.
                # In this loopback experiment either channel's EOF/error closes both channels.
                kind, received_channel, flags, packet = await read_envelope(reader)
                if kind != DATA or received_channel != channel:
                    raise ValueError('Unexpected channel/frame')
                await self.message(client, packet, channel)
        except (asyncio.IncompleteReadError, ConnectionError, TimeoutError, ValueError) as error:
            print(f'TURBINE disconnect {type(error).__name__}: {error}', flush=True)
        finally:
            if client is not None:
                await self.disconnect(client)
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass


async def main():
    relay = TurbineRelay()
    server = await asyncio.start_server(relay.accept, '127.0.0.1', 17801)
    print('Private Turbine TCP relay listening on 127.0.0.1:17801', flush=True)
    async with server:
        await server.serve_forever()


if __name__ == '__main__':
    asyncio.run(main())
