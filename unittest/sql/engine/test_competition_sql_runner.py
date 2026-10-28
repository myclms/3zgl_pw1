#!/usr/bin/env python3
"""Protocol-text comparisons must detect mysqltest datetime mismatches."""
import unittest

import pymysql
from pymysql.constants import FIELD_TYPE

from run_competition_sql import field, wire_text_conversions


class WireTextComparisonTest(unittest.TestCase):
    def test_fractional_zeroes_remain_observable(self):
        converter = wire_text_conversions()[FIELD_TYPE.DATETIME]
        seconds = '2026-08-01 10:11:12'
        microseconds = seconds + '.000000'
        self.assertEqual(field(converter(seconds)), seconds)
        self.assertEqual(field(converter(microseconds)), microseconds)
        self.assertNotEqual(field(converter(seconds)), field(converter(microseconds)))

    def test_fractional_values_and_temporal_types_preserve_server_text(self):
        conversions = wire_text_conversions()
        cases = [(FIELD_TYPE.DATE, '2026-08-01'),
                 (FIELD_TYPE.TIMESTAMP, '2026-08-01 10:11:12.001'),
                 (FIELD_TYPE.DATETIME, '2026-08-01 10:11:12.123456'),
                 (FIELD_TYPE.TIME, '-36:11:12.000000')]
        for kind, text in cases:
            with self.subTest(kind=kind):
                self.assertEqual(field(conversions[kind](text)), text)
        self.assertEqual(field(None), 'NULL')

    def test_does_not_change_global_pymysql_conversions(self):
        original = pymysql.converters.conversions[FIELD_TYPE.DATETIME]
        wire_text_conversions()
        self.assertIs(pymysql.converters.conversions[FIELD_TYPE.DATETIME], original)
        self.assertEqual(wire_text_conversions()[FIELD_TYPE.LONG]('42'), 42)


if __name__ == '__main__':
    unittest.main()
