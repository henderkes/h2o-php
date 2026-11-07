"""Exercise complete H1 body ownership plus all ordinary protocol fallbacks."""
import http.client
import socket
import struct
import time
import unittest

import test_http1_body_reuse as reuse


class HTTP1BodyOwnershipTest(reuse.HTTP1BodyReuseTest):
    body_limit = 1048576
    global_limit = 8388608

    def test_completed_unread_body_and_cancelled_owner_release_credit(self):
        # PUT bypasses PHP's eager POST reader, so this storage is still owned
        # by the native queue when PHP finishes or the connection disappears.
        (self.base / 'unread.php').write_text("<?php usleep(100000); header('Content-Length: 2'); echo 'ok';")
        for cancel in (False, True):
            with self.subTest(cancel=cancel):
                connection = self.raw_connection(False)
                try:
                    connection.sendall(b'PUT /unread.php HTTP/1.1\r\nHost: reuse.test\r\n'
                        b'Content-Type: application/octet-stream\r\nContent-Length: 1048576\r\n\r\n' + reuse.LARGE)
                    deadline = time.monotonic() + 1
                    while self.metrics()['buffered_bytes'] == 0:
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.002)
                    if cancel:
                        connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                    else:
                        with connection.makefile('rb') as stream:
                            self.assertEqual(self.read_fixed_response(stream), b'ok')
                finally:
                    connection.close()
                self.assertEqual(self.quiescent()['buffered_bytes'], 0)
                recovery = self.connection(False)
                try:
                    self.send(recovery, 'after-unread', reuse.LARGE)
                finally:
                    recovery.close()
                self.quiescent()


class HTTP1OwnedTightBudgetTest(HTTP1BodyOwnershipTest):
    global_limit = 2097152


if __name__ == '__main__':
    unittest.main()
