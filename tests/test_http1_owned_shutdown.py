from pathlib import Path
import socket, struct, sys, time, unittest
import test_http1_body_reuse as reuse

class OwnedShutdownTest(reuse.HTTP1BodyReuseTest):
    body_limit=8388608
    global_limit=33554432
    def test_nonfinal_unread_return_after_request_teardown(self):
        script=self.base/'held-nonfinal.php'
        script.write_text("<?php file_put_contents(__DIR__.'/entered-nonfinal','1'); usleep(200000); header('Content-Length: 2'); echo 'ok';")
        time.sleep(3.11)
        with self.raw_connection(False) as connection:
            connection.sendall(b'PUT /held-nonfinal.php HTTP/1.1\r\nHost: reuse.test\r\nContent-Type: application/octet-stream\r\nContent-Length: 2097152\r\n\r\n'+reuse.LARGE[:786432])
            deadline=time.monotonic()+2
            while not (self.base/'entered-nonfinal').exists() or self.metrics()['buffered_bytes']<262144:
                self.assertIsNone(self.process.poll(),self.log_path.read_text())
                self.assertLess(time.monotonic(),deadline)
                time.sleep(.001)
            connection.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
        self.process.terminate()
        self.process.wait(timeout=5)
        self.assertEqual(self.process.returncode,0,self.log_path.read_text())
        self.assertIn('0 pending completions',self.log_path.read_text())
    def test_unread_body_shutdown_after_request_teardown(self):
        script=self.base/'held-unread.php'
        script.write_text("<?php file_put_contents(__DIR__.'/entered','1'); usleep(100000); header('Content-Length: 2'); echo 'ok';")
        # Age the dynamically created PHP source before execution.
        time.sleep(3.11)
        with self.raw_connection(False) as connection:
            connection.sendall(b'PUT /held-unread.php HTTP/1.1\r\nHost: reuse.test\r\nContent-Type: application/octet-stream\r\nContent-Length: 1048576\r\n\r\n'+reuse.LARGE)
            deadline=time.monotonic()+2
            while not (self.base/'entered').exists():
                self.assertIsNone(self.process.poll(),self.log_path.read_text())
                self.assertLess(time.monotonic(),deadline)
                time.sleep(.001)
            self.assertGreater(self.metrics()['buffered_bytes'],0)
            # Retire the network generator while PHP still owns unread input.
            connection.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
        self.process.terminate()
        self.process.wait(timeout=5)
        self.assertEqual(self.process.returncode,0,self.log_path.read_text())
        self.assertIn('0 pending completions',self.log_path.read_text())

suite=unittest.TestSuite(OwnedShutdownTest(name) for name in ('test_unread_body_shutdown_after_request_teardown',
    'test_nonfinal_unread_return_after_request_teardown'))
result=unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(not result.wasSuccessful())
