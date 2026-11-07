"""TLS key-isolation regression: python3 tests/test_openssl_neverbleed.py [binary]."""
import http.client
import json
from pathlib import Path
import re
import ssl
import subprocess
import tempfile
import unittest

import test_lifecycle as lifecycle


class NeverbleedTest(unittest.TestCase):
    def test_provider_without_php_rejects_private_export_and_retains_keys(self):
        version = subprocess.check_output([str(lifecycle.BINARY), "--version"], text=True)
        if "OpenSSL 4." not in version:
            self.skipTest("OpenSSL 4 provider regression")
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="neverbleed-provider-") as directory:
            binary = Path(directory) / "provider-test"
            # Linking libphp here would mask removed OpenSSL APIs and invalidate
            # the regression: this executable links only the selected crypto.
            build = subprocess.run(["cc", "-D_GNU_SOURCE", "-O1", "-g", "-I" + str(root / "upstream/h2o/deps/neverbleed"),
                str(root / "tests/neverbleed_provider.c"), str(root / "upstream/h2o/deps/neverbleed/neverbleed.c"),
                "-o", str(binary), "-lssl", "-lcrypto", "-pthread"], capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            for kind in ("rsa", "ec"):
                server = self.fixture(kind)
                try:
                    result = subprocess.run([str(binary), str(server.base / "cert.pem"), str(server.base / "key.pem")],
                        capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                finally:
                    server.doCleanups()

    def fixture(self, kind):
        server = lifecycle.LifecycleTest()
        server.setUp()
        self.addCleanup(server.doCleanups)
        self.issue(server, kind)
        yaml = server.yaml("tls").replace(f"    port: {server.port}\n", f"""    port: {server.port}
    ssl:
      certificate-file: {server.base / 'cert.pem'}
      key-file: {server.base / 'key.pem'}
      ocsp-update-interval: 0
      neverbleed: ON
""")
        server.config.write_text(yaml)
        return server

    def issue(self, server, kind):
        arguments = ["-newkey", "rsa:2048"] if kind == "rsa" else ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"]
        subprocess.run(["openssl", "req", "-x509", *arguments, "-noenc", "-days", "1", "-subj", "/CN=localhost",
                        "-out", str(server.base / "cert.pem"), "-keyout", str(server.base / "key.pem")],
                       capture_output=True, check=True)
        return ssl.PEM_cert_to_DER_cert((server.base / "cert.pem").read_text())

    def fetch(self, server, version, ciphers=None):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.minimum_version = context.maximum_version = version
        if ciphers:
            context.set_ciphers(ciphers)
        connection = http.client.HTTPSConnection("127.0.0.1", server.port, context=context, timeout=5)
        try:
            connection.connect()
            certificate = connection.sock.getpeercert(binary_form=True)
            self.assertEqual(connection.sock.version(), "TLSv1.2" if version == ssl.TLSVersion.TLSv1_2 else "TLSv1.3")
            connection.request("GET", "/", headers={"Host": "lifecycle.test"})
            response = connection.getresponse()
            body = response.read()
            self.assertEqual(response.status, 200, body.decode(errors="replace") + "\n" + (server.base / "server.log").read_text())
            return certificate, json.loads(body)
        finally:
            connection.close()

    def test_rsa_ec_tls12_tls13_and_certificate_reload(self):
        for kind in ("rsa", "ec"):
            for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
                with self.subTest(key=kind, tls=version):
                    server = self.fixture(kind)
                    try:
                        server.start()
                        certificate, before = self.fetch(server, version)
                        self.assertEqual(before["version"], "tls")
                        # The key-operation helper must exist: disabling isolation
                        # would make this test fail even if the handshake worked.
                        log = (server.base / "server.log").read_text()
                        worker = re.search(r"generation 1 pid (\d+) ready", log).group(1)
                        children = subprocess.check_output(["ps", "-o", "args=", "--ppid", worker], text=True)
                        self.assertIn("neverbleed", children)
                        second = self.issue(server, kind)
                        self.assertNotEqual(second, certificate)
                        reload = server.command("reload", "--config", str(server.config), "--force")
                        self.assertEqual(reload.returncode, 0, reload.stderr)
                        actual, after = self.fetch(server, version)
                        self.assertEqual(actual, second)
                        self.assertNotEqual(after["boot"], before["boot"])
                        flag = "--tlsv1.2" if version == ssl.TLSVersion.TLSv1_2 else "--tlsv1.3"
                        maximum = "1.2" if version == ssl.TLSVersion.TLSv1_2 else "1.3"
                        result = subprocess.run(["curl", "--silent", "--show-error", "--fail", "--http2", "--insecure",
                            "--noproxy", "*", flag, "--tls-max", maximum, "-H", "Host: lifecycle.test",
                            "--write-out", "\n%{http_version}", f"https://127.0.0.1:{server.port}/"],
                            capture_output=True, text=True, timeout=5)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        body, protocol = result.stdout.rsplit("\n", 1)
                        self.assertEqual(protocol, "2")
                        self.assertEqual(json.loads(body), after)
                    finally:
                        server.doCleanups()

    def test_tls12_rsa_key_exchange_decryption(self):
        server = self.fixture("rsa")
        server.config.write_text(server.config.read_text().replace("      neverbleed: ON\n", """      neverbleed: ON
      maximum-version: TLSv1.2
      cipher-suite: AES128-GCM-SHA256
"""))
        server.start()
        _, response = self.fetch(server, ssl.TLSVersion.TLSv1_2, "AES128-GCM-SHA256")
        self.assertEqual(response["version"], "tls")

    def test_php_openssl_and_server_provider_coexist(self):
        server = self.fixture("ec")
        # PHP bundles OpenSSL 3 on this system; use a private application config
        # instead of feeding it the host's OpenSSL 4 certificate extensions.
        (server.base / "app-openssl.cnf").write_text("[req]\ndistinguished_name = dn\n[dn]\nCN = localhost\n")
        server.worker.write_text("""<?php
while (frankenphp_handle_request(function () {
    $key = openssl_pkey_new(['private_key_type' => OPENSSL_KEYTYPE_EC, 'curve_name' => 'prime256v1',
        'config' => __DIR__.'/app-openssl.cnf']);
    $signed = openssl_sign('PHP crypto coexistence', $signature, $key, OPENSSL_ALGO_SHA256);
    $public = openssl_pkey_get_details($key)['key'];
    header('Content-Type: application/json');
    echo json_encode(['signed' => $signed,
        'verified' => openssl_verify('PHP crypto coexistence', $signature, $public, OPENSSL_ALGO_SHA256)]);
}));
""")
        server.start()
        for iteration in range(2):
            _, response = self.fetch(server, ssl.TLSVersion.TLSv1_3)
            self.assertEqual(response, {"signed": True, "verified": 1})
            if not iteration:
                result = server.command("reload", "-c", str(server.config), "--force")
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_binary_has_no_legacy_engine_imports(self):
        version = subprocess.check_output([str(lifecycle.BINARY), "--version"], text=True)
        if "OpenSSL 4." not in version:
            self.skipTest("OpenSSL 4 provider regression")
        symbols = subprocess.check_output(["nm", "-D", str(lifecycle.BINARY)], text=True)
        self.assertNotRegex(symbols, r"\bU\s+ENGINE_\w+")


if __name__ == "__main__":
    unittest.main()
