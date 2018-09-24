from __future__ import print_function

import sys
import os
import random
import struct

import btle


def rand_db(adtype, datalen):
    return struct.pack("<BB", datalen+1, adtype) + os.urandom(datalen)

if __name__ == '__main__':
    while True:
        sr = btle.ScanEntry(None, 0)
        db = b''
        while len(db) <= 28:
            adlen = random.randint(3, 31-len(db))
            adtype = random.randint(0,255)
            db += rand_db(adtype, adlen-2)
        resp = { 'type' : [ random.randint(1,2) ],
                 'rssi' : [ random.randint(1,127) ],
                 'flag' : [ 4 ],
                 'd' : [ db ] }
        sr._update(resp)
        
        print ("Result:", sr.getScanData())
        