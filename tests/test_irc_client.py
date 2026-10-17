#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Lock the shared IRC fixture's deadline boundary without scheduler timing."""
import unittest
from unittest.mock import patch

from irc_client import IRCClient


class IRCClientDeadlines(unittest.TestCase):
    def test_drain_never_selects_with_negative_timeout(self):
        peer = IRCClient.__new__(IRCClient)
        peer.sock = object()
        peer.buf = bytearray()
        with patch("irc_client.time.monotonic", side_effect=[0.0, 0.5, 1.1, 1.2]), \
                patch("irc_client.select.select") as select:
            select.return_value = ([], [], [])
            select.side_effect = lambda readers, writers, errors, timeout: self.check_timeout(timeout)
            peer.drain(1.0)
            select.assert_called_once_with([peer.sock], [], [], 0.05)

    def check_timeout(self, timeout):
        self.assertGreaterEqual(timeout, 0.0)
        return [], [], []


if __name__ == "__main__":
    unittest.main()
