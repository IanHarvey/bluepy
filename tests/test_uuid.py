"""
Test the UUID class in `btle.py`

Run with:
    $ python -m unittest this_file.py
"""

import binascii
import sys
import unittest
import warnings

from bluepy.btle import UUID

class TestUUID(unittest.TestCase):
    def test_init_with_good_uuids(self):
        self.assertIsInstance(UUID(10880), UUID)
        self.assertIsInstance(UUID("10880"), UUID)
        self.assertIsInstance(UUID(10007), UUID)
        self.assertIsInstance(UUID("10007"), UUID)
        self.assertIsInstance(UUID("000a0200-0000-1000-7000-00609bf534fb"), UUID)
        self.assertIsInstance(UUID("000a020000001000700000609bf534fb"), UUID)
        self.assertIsInstance(UUID("12345678-5001-abcd-3422-dbc9eac7543c"), UUID)
        self.assertIsInstance(UUID("123456785001abcd3422dbc9eac7543c"), UUID)
        self.assertIsInstance(UUID("99999999-9999-9999-9999-999999999999"), UUID)
        self.assertIsInstance(UUID("99999999999999999999999999999999"), UUID)
        self.assertIsInstance(UUID("00000000-0000-0000-0000-000000000000"), UUID)
        self.assertIsInstance(UUID("00000000000000000000000000000000"), UUID)
        self.assertIsInstance(UUID(0), UUID)
        self.assertIsInstance(UUID("0"), UUID)
        self.assertIsInstance(UUID(0x0), UUID)
        self.assertIsInstance(UUID(0000), UUID)
        self.assertIsInstance(UUID("0000"), UUID)
        self.assertIsInstance(UUID(0x0000), UUID)
        self.assertIsInstance(UUID(00000000), UUID)
        self.assertIsInstance(UUID("00000000"), UUID)
        self.assertIsInstance(UUID(0x00000000), UUID)
        self.assertIsInstance(UUID("F"), UUID)
        self.assertIsInstance(UUID("f"), UUID)
        self.assertIsInstance(UUID(0xF), UUID)
        self.assertIsInstance(UUID("FFFF"), UUID)
        self.assertIsInstance(UUID(0xFFFF), UUID)
        self.assertIsInstance(UUID("FFFFFFFF"), UUID)
        self.assertIsInstance(UUID(0xFFFFFFFF), UUID)

    def test_init_with_bad_uuids(self):
        self.assertRaises(ValueError, UUID, -12345)
        self.assertRaises(ValueError, UUID, -1)
        self.assertRaises(ValueError, UUID, "ABCDEFG")
        self.assertRaises(ValueError, UUID, ":::::::::")
        self.assertRaises(ValueError, UUID, "!@#?><:\"567")
        self.assertRaises(ValueError, UUID, "12345678-4912-e123-3333-abcdef1234")
        self.assertRaises(ValueError, UUID, "123456784912e1233333abcdef1234")
        self.assertRaises(ValueError, UUID, "This is a sentence.")
        self.assertRaises(ValueError, UUID, 0xFFFFFFFFA)
        self.assertRaises(ValueError, UUID, 0xFFFFFFFFBB)
        self.assertRaises(binascii.Error, UUID, "0x0")
        self.assertRaises(binascii.Error, UUID, "0xFFFFFFFFA")
        self.assertRaises(binascii.Error, UUID, "0xFFFFFFFFBB")

    def test_str(self):
        self.assertEqual(UUID(0), "00000000-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID("0"), "00000000-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID(0x0), "00000000-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID(0000), "00000000-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID("0000"), "00000000-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID(0x0000), "00000000-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID("F"), "0000000f-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID("f"), "0000000f-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID("FFFF"), "0000ffff-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID("ABCD"), "0000abcd-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID(0xFFFFFFFF), "ffffffff-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID(0xffffffff), "ffffffff-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID("00000000-0000-1000-8000-00805f9b34fb"), "00000000-0000-1000-8000-00805f9b34fb")
        self.assertEqual(UUID("0000000000001000800000805f9b34fb"), "00000000-0000-1000-8000-00805f9b34fb")

    def test_eq(self):
        self.assertTrue(UUID(0) == "00000000-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID("0") == "00000000-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID(0x0) == "00000000-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID(0000) == "00000000-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID("0000") == "00000000-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID("F") == "0000000f-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID("f") == "0000000f-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID("FFFF") == "0000ffff-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID("ABCD") == "0000abcd-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID(0xFFFFFFFF) == "ffffffff-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID(0xabcdef12) == "abcdef12-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID("00000000-0000-1000-8000-00805f9b34fb") == "00000000-0000-1000-8000-00805f9b34fb")
        self.assertTrue(UUID("0000000000001000800000805f9b34fb") == "00000000-0000-1000-8000-00805f9b34fb")

    def test_cmp(self):
        '''Note that cmp() does not exist in Python 3, only Python 2'''
        if sys.version_info[0] == 3:
            version_warning_msg = "Method UUID.__cmp__() is not supported in Python 3. Skipping test_cmp()"
            warnings.warn(version_warning_msg, UserWarning, stacklevel=2)
        else:
            self.assertTrue(cmp(UUID(0), "00000000-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID("0"), "00000000-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID(0x0), "00000000-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID(0000), "00000000-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID("0000"), "00000000-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID("F"), "0000000f-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID("f"), "0000000f-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID("FFFF"), "0000ffff-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID("ABCD"), "0000abcd-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID(0xFFFFFFFF), "ffffffff-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID(0xabcdef12), "abcdef12-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID("00000000-0000-1000-8000-00805f9b34fb"), "00000000-0000-1000-8000-00805f9b34fb"))
            self.assertTrue(cmp(UUID("0000000000001000800000805f9b34fb"), "00000000-0000-1000-8000-00805f9b34fb"))


if __name__ == "__main__":
    unittest.main()

