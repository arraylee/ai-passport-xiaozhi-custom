"""Descriptor conformance for Android's generic Linux single-contact path.

Driver contract: Linux v6.6 drivers/hid/hid-input.c, HID_UP_DIGITIZER:
application Pen (0x0d0002) sets INPUT_PROP_DIRECT. InRange in a Finger
physical collection yields BTN_TOOL_FINGER. A bare TouchScreen collection
without Contact ID does not opt in to hid-multitouch's direct-touch mapping.
Android TouchInputMapper::computeDeviceType defaults non-DIRECT to POINTER.

This models the documented driver boundary; it is not an OPPO kernel capture.
https://github.com/torvalds/linux/blob/v6.6/drivers/hid/hid-input.c
https://source.android.com/docs/core/interaction/input/touch-devices
"""
import re
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'components/platform/platform_esp32/src/platform_clicker.c'

def digitizer_usages():
    raw=SOURCE.read_text().split('s_report_map[] = {',1)[1].split('};',1)[0]
    raw=re.sub(r'/\*.*?\*/','',raw,flags=re.S)
    data=bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]+)',raw))
    page=rid=0; local=[]; stack=[]; pos=0; result=[]
    while pos<len(data):
        prefix=data[pos];pos+=1
        n=(0,1,2,4)[prefix&3]; value=int.from_bytes(data[pos:pos+n],'little');pos+=n
        tag=prefix>>4;kind=(prefix>>2)&3
        if kind==1:
            if tag==0:page=value
            if tag==8:rid=value
        elif kind==2 and tag==0:local.append((page<<16)|value)
        elif kind==0:
            if tag==10:stack.append((value,local[-1] if local else 0))
            elif tag==12:stack.pop()
            elif tag==8 and rid==3 and not value&1:
                app=next((u for t,u in reversed(stack) if t==1),0)
                physical=next((u for t,u in reversed(stack) if t==0),0)
                result.extend((app,physical,u) for u in local)
            local=[]
    return result

class AndroidClassificationTest(unittest.TestCase):
    def test_contact_is_direct_finger_not_pointer_or_stylus(self):
        fields=digitizer_usages()
        contacts=[f for f in fields if f[2] in (0x0d0042,0x0d0032)]
        self.assertEqual(len(contacts),2)
        # Linux generic mapping, deliberately separate from gesture math.
        direct=all((app&0xff)==2 for app,physical,usage in contacts)
        in_range=next(f for f in contacts if f[2]==0x0d0032)
        finger=in_range[1]==0x0d0022
        self.assertTrue(direct, 'Android would use pointer mode without INPUT_PROP_DIRECT')
        self.assertTrue(finger, 'Android must receive finger contact, not stylus hover')
        self.assertIn(0x010030,[u for a,p,u in fields])
        self.assertIn(0x010031,[u for a,p,u in fields])

if __name__=='__main__':unittest.main()
