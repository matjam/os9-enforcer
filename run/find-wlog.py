#!/usr/bin/env python3
"""Find WLog's state block in a physical memory dump and print its recent
faults. Usage: find-wlog.py dump.bin"""
import struct
import sys

data = open(sys.argv[1], "rb").read()
magic = b"WLG!"
i = data.find(magic)
while i >= 0:
    self_addr, seq, timer_sp_min = struct.unpack(">3L", data[i + 4:i + 16])
    print(f"block at physical {i:08X}: logical {self_addr:08X}, "
          f"faults recorded {seq}, timer lowest sp {timer_sp_min:08X}")
    events = []
    for n in range(32):
        off = i + 16 + n * 24
        pc, lr, addr, sp, zone, eseq = struct.unpack(">6L", data[off:off + 24])
        if eseq:
            events.append((eseq, pc, lr, addr, sp, zone))
    for eseq, pc, lr, addr, sp, zone in sorted(events)[-16:]:
        print(f"  #{eseq:5d} pc {pc:08X} lr {lr:08X} addr {addr:08X} "
              f"sp {sp:08X} zone {zone:08X}")
    i = data.find(magic, i + 1)
