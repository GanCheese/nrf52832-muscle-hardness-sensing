import sys, asyncio, time
sys.path.insert(0, 'project/.tools/pylibs')
from smpclient import SMPClient
from smpclient.transport.serial import SMPSerialTransport
from smpclient.requests.os_management import EchoWrite, ResetWrite
from smpclient.generics import success
import serial
async def run():
    c=SMPClient(SMPSerialTransport(baudrate=115200), 'COM9', timeout_s=2)
    await c.connect()
    print('Press RESET now', flush=True)
    deadline=time.monotonic()+120
    try:
        while time.monotonic()<deadline:
            try:
                r=await c.request(EchoWrite(d='capture'),timeout_s=1)
                if success(r):
                    r=await c.request(ResetWrite())
                    if not success(r) and getattr(r,'rc',None)!=0: raise RuntimeError(str(r))
                    break
            except TimeoutError: pass
        else: raise RuntimeError('No bootloader response')
    finally: await c.disconnect()
    s=serial.Serial('COM9',115200,timeout=1)
    data=b''
    end=time.monotonic()+15
    while time.monotonic()<end: data+=s.read(4096)
    s.close()
    open('project/logs/imu_spi_125k_boot.txt','wb').write(data)
    print(data.decode(errors='replace'))
asyncio.run(run())
