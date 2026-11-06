"""Upload a signed MCUboot application through the board's CH340 port."""
import argparse
import asyncio
import hashlib
import logging
import os
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.tools' / 'pylibs'))

from smpclient import SMPClient
from smpclient.generics import success
from smpclient.requests.os_management import EchoWrite, ResetWrite
from smpclient.requests.image_management import ImageStatesRead
from smpclient.transport.serial import SMPSerialTransport, BufferSize


def default_image():
    search_roots = []
    env_dir = os.environ.get('NRF_BUILD_USB')
    if env_dir:
        search_roots.append(Path(env_dir))
    search_roots.append(ROOT / 'build-usb')
    # The project path contains non-ASCII characters, so this workspace builds
    # from an ASCII junction at C:/nrf_ws/build-usb.
    search_roots.append(Path('C:/nrf_ws/build-usb'))

    for root in search_roots:
        candidates = sorted(root.glob('*/zephyr/zephyr.signed.bin'))
        if candidates:
            return max(candidates, key=lambda p: p.stat().st_mtime)

    raise FileNotFoundError(
        'No build-usb/*/zephyr/zephyr.signed.bin. Build with west --sysbuild first.')



async def main(args):
    if args.probe:
        image = None
    else:
        image_path = Path(args.image) if args.image else default_image()
        image = image_path.read_bytes()
    if image is not None and image[:4] != bytes.fromhex('3db8f396'):
        raise ValueError('Use zephyr.signed.bin, not zephyr.bin or a HEX file.')
    if image is not None:
        if len(image) < 32 or len(image) > 0x37000:
            raise ValueError('Image does not fit this board application slot.')
        header_size, protected_size, body_size = struct.unpack_from('<HHI', image, 8)
        hash_end = header_size + body_size + protected_size
        if hash_end > len(image):
            raise ValueError('Truncated MCUboot image.')
        expected_hash = hashlib.sha256(image[:hash_end]).digest()
    client = SMPClient(SMPSerialTransport(
        baudrate=115200, fragmentation_strategy=BufferSize(512)), args.port, timeout_s=2)
    try:
        await client.connect()
        print(f'Waiting for MCUboot on {args.port}. Press RESET now.', flush=True)
        deadline = time.monotonic() + args.wait
        while True:
            try:
                reply = await client.request(EchoWrite(d='max98357a-dfu'), timeout_s=1)
                if success(reply):
                    break
            except TimeoutError:
                pass
            if time.monotonic() >= deadline:
                raise TimeoutError('No MCUboot response. Check COM port and press RESET.')
        print('MCUboot handshake OK', flush=True)
        if image is not None:
            last = -1
            async for offset in client.upload(image, subsequent_timeout_s=10):
                percent = offset * 100 // len(image)
                if percent // 10 != last or offset == len(image):
                    print(f'Upload: {offset}/{len(image)} ({percent}%)', flush=True)
                    last = percent // 10
        reply = await client.request(ImageStatesRead())
        if not success(reply):
            raise RuntimeError(f'Image list failed: {reply}')
        for state in reply.images:
            hash_text = state.hash.hex() if state.hash else 'unavailable'
            print(f'Slot {state.slot}: version={state.version}, hash={hash_text}', flush=True)
        if image is not None:
            if not any(state.slot == 0 and state.hash == expected_hash for state in reply.images):
                raise RuntimeError('Uploaded image hash does not match the local image.')
            print('Image SHA-256 verified', flush=True)
            reply = await client.request(ResetWrite())
            # MCUboot returns {rc: 0}; smpclient models this as an error response
            # even though EOK (zero) means success.
            if not success(reply) and getattr(reply, 'rc', None) != 0:
                raise RuntimeError(f'Reset failed: {reply}')
            print('Upload finished; device reset. Application starts after 5 seconds.', flush=True)
    finally:
        await client.disconnect()


if __name__ == '__main__':
    logging.basicConfig(level=logging.CRITICAL)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM9')
    parser.add_argument('--wait', type=float, default=60)
    parser.add_argument('--probe', action='store_true')
    parser.add_argument('--image', default=None)
    args = parser.parse_args()
    try:
        asyncio.run(main(args))
    except Exception as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)
