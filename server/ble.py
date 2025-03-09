import sys
import logging
import aiohttp
import asyncio
import threading
import random
import datetime

from typing import Any, Union

from bless import (  # type: ignore
    BlessServer,
    BlessGATTCharacteristic,
    GATTCharacteristicProperties,
    GATTAttributePermissions,
)

#from secrets import HA_AUTH, HA_ENDPOINT
HA_AUTH=""
HA_ENDPOINT=""
SERVER_NAME = "SPServer"  # must be shorter than 10 characters
SERVICE_UUID = "4a38ff83-3e18-4f35-a51b-90829dc07ed0"
CHARACTERISTICS = [
    "048df6ac-7c4c-4383-897e-760bae10e321",
    "0a555305-d8f6-433b-8833-67b4d1f38630",
    "1dff0906-83c3-46ad-8da3-b999eba26e9b",
    "dd511cd7-51b8-472b-aff7-183bc6cbfdf1",
]

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(name=__name__)

# NOTE: Some systems require different synchronization methods.
trigger: Union[asyncio.Event, threading.Event]
if sys.platform in ["darwin", "win32"]:
    trigger = threading.Event()
else:
    trigger = asyncio.Event()


def read_request(characteristic: BlessGATTCharacteristic, **kwargs) -> bytearray:
    test=str(characteristic.value)
    logger.debug(f"Reading {test}")
    return bytearray(str.encode(test))

def write_request(characteristic: BlessGATTCharacteristic, value: Any, **kwargs):
    characteristic.value = value
    logger.debug(f"Char value set to {characteristic.value}")
    if characteristic.value == b"\x0f":
        logger.debug("NICE")
        trigger.set()



async def get_ha_data():
    headers = {
        "Authorization": HA_AUTH,
        "Content-Type": "application/json",
    }
    async with aiohttp.ClientSession() as session:
        async with session.get(HA_ENDPOINT, headers=headers) as response:
            return await response.text()

server = None
async def run(loop):
    trigger.clear()

    # Instantiate the server
    server = BlessServer(name=SERVER_NAME, loop=loop)
    server.read_request_func = read_request
    server.write_request_func = write_request

    await server.add_new_service(SERVICE_UUID)

    char_flags = GATTCharacteristicProperties.read | GATTCharacteristicProperties.write  #| GATTCharacteristicProperties.notify
    permissions = GATTAttributePermissions.readable | GATTAttributePermissions.writeable

    for uuid in CHARACTERISTICS:
        await server.add_new_characteristic(
            SERVICE_UUID, uuid, char_flags, None, permissions
        )

    await server.start()
    logger.debug("Advertising")

    while True and not trigger.is_set():
        logger.debug("Updating HA data")
        data = "1," + datetime.datetime.now().strftime("%B %d %Y",)+",temp ute"#(await get_ha_data()).encode("utf-8")
        split_val = 240
        values = [data[i : i + split_val] for i in range(0, len(data), split_val)]
        for i, val in enumerate(values):
            server.get_characteristic(CHARACTERISTICS[i]).value = val
        await asyncio.sleep(60)

    await server.stop()


loop = asyncio.get_event_loop()
loop.run_until_complete(run(loop))