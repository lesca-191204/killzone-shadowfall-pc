"""Native Turbine RTT echo, restricted to the local private-server experiment.

The guest sends a zero-padded 24-byte ASCII ``RTT <index>`` datagram and accepts
the same bytes from the advertised IP/port. It measures elapsed time itself.
"""
import asyncio
import re

PING_PORT = 17802
PROBE = re.compile(rb'RTT (0|[1-9][0-9]{0,9})\x00+')


def ping_reply(data):
    if len(data) != 24:
        return None
    match = PROBE.fullmatch(data)
    if match is None or int(match[1]) > 2147483647:
        return None
    return data


class PingProtocol(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport = None
        self.replies = 0

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, address):
        if address[0] != '127.0.0.1':
            return
        reply = ping_reply(data)
        if reply is not None:
            self.transport.sendto(reply, address)
            self.replies += 1
            if self.replies <= 32:
                print(f'PING_PRIVATE reply={self.replies} bytes={len(reply)}', flush=True)


async def start_ping_service(port=PING_PORT):
    return await asyncio.get_running_loop().create_datagram_endpoint(
        PingProtocol, local_addr=('127.0.0.1', port))


async def main():
    transport, _ = await start_ping_service()
    print(f'Private Turbine RTT UDP listening on 127.0.0.1:{PING_PORT}', flush=True)
    try:
        await asyncio.Future()
    finally:
        transport.close()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
