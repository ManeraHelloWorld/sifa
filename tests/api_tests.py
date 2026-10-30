"""Black-box tests against the real executable and real local HTTP requests."""
import http.client
import http.cookiejar
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request

EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="sifa-test-")
        cls.start_server()

    @classmethod
    def start_server(cls):
        cls.process = subprocess.Popen(
            [EXECUTABLE, "--no-open", "--data-dir", cls.directory.name],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        lines = queue.Queue()

        def read_lines():
            for line in cls.process.stdout:
                lines.put(line)
            lines.put(None)

        threading.Thread(target=read_lines, daemon=True).start()
        while True:
            try:
                line = lines.get(timeout=10)
            except queue.Empty:
                cls.stop_server()
                raise RuntimeError("Server did not become ready")
            if line is None:
                raise RuntimeError("Server exited: " + cls.process.stderr.read())
            if line.startswith("SIFA_URL="):
                cls.url = line.strip().split("=", 1)[1]
                break
        parsed = urllib.parse.urlsplit(cls.url)
        cls.origin = f"{parsed.scheme}://{parsed.netloc}"
        cls.port = parsed.port
        cls.cookies = http.cookiejar.CookieJar()
        # Always address loopback directly; inherited Internet proxies are unrelated.
        cls.client = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), urllib.request.HTTPCookieProcessor(cls.cookies)
        )
        with cls.client.open(cls.url, timeout=5) as response:
            cls.html = response.read().decode()
            if response.geturl() != cls.origin + "/":
                raise RuntimeError("Bootstrap token was not removed from browser URL")

    @classmethod
    def stop_server(cls):
        cls.process.terminate()
        try:
            cls.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait(timeout=5)
        cls.process.stdout.close()
        cls.process.stderr.close()

    @classmethod
    def tearDownClass(cls):
        cls.stop_server()
        cls.directory.cleanup()

    def request(self, path, body=None, headers=None):
        request_headers = dict(headers or {})
        if body is not None:
            request_headers.setdefault("Content-Type", "application/json")
            if not isinstance(body, bytes):
                body = json.dumps(body).encode()
        request = urllib.request.Request(self.origin + path, body, request_headers)
        with self.client.open(request, timeout=10) as response:
            return json.load(response)

    def test_state_and_embedded_assets(self):
        self.assertIn("Sifa", self.html)
        assets = Path(__file__).resolve().parents[1] / "web"
        self.assertEqual(self.html, (assets / "index.html").read_bytes().decode("utf-8"))
        state = self.request("/api/state")
        self.assertFalse(state["capabilities"]["android"])
        self.assertFalse(state["capabilities"]["game"])
        self.assertGreaterEqual(state["host"]["logical_cpus"], 1)
        for asset, content_type in [("/app.js", "text/javascript"), ("/styles.css", "text/css")]:
            with self.client.open(self.origin + asset, timeout=5) as response:
                self.assertIn(content_type, response.headers["Content-Type"])
                self.assertEqual(response.read(), (assets / asset.lstrip("/")).read_bytes())

    def test_guest_executes_real_instructions(self):
        for backend in ["software", "auto"]:
            result = self.request("/api/demo", {"backend": backend})
            self.assertTrue(result["ok"], result)
            self.assertEqual(result["output"], "SIFA\n")
            self.assertEqual(result["instructions"], 11)
        state = self.request("/api/state")
        self.assertEqual(state["last_run"]["output"], "SIFA\n")

    def test_hardware_failure_is_honest(self):
        result = self.request("/api/demo", {"backend": "hardware"})
        if result["ok"]:
            self.assertEqual(result["output"], "SIFA\n")
            self.assertIn(result["backend"], ["kvm", "whpx"])
        else:
            self.assertTrue(result["error"])
            self.assertNotEqual(result["backend"], "software-x86")

    def test_measured_benchmark(self):
        result = self.request("/api/benchmark", {})
        self.assertEqual(result["samples"], 200)
        self.assertGreater(result["mean_ms"], 0)
        self.assertLessEqual(result["p50_ms"], result["p95_ms"])
        self.assertLessEqual(result["p95_ms"], result["max_ms"])
        self.assertIn("not game FPS", result["scope"])

    def test_settings_persist_and_bound_resources(self):
        state = self.request("/api/state")
        value = self.request("/api/settings", {
            "width": 100000, "height": -10, "memory_mb": 100000,
            "cpu_cores": 10000, "dpi": 1600, "cm360": 42.5,
            "sensitivity": 0.75, "preset": "custom",
        })
        self.assertEqual(value["width"], 3840)
        self.assertEqual(value["height"], 360)
        self.assertLessEqual(value["cpu_cores"], state["host"]["logical_cpus"])
        self.assertLessEqual(value["memory_mb"], state["host"]["memory_mb"])
        self.assertEqual(value["preset"], "custom")
        self.assertEqual(json.loads((Path(self.directory.name) / "settings.json").read_text()), value)
        type(self).stop_server()
        type(self).start_server()
        self.assertEqual(self.request("/api/state")["settings"], value)

    def test_invalid_requests_do_not_change_settings(self):
        before = self.request("/api/state")["settings"]
        for body in [b"{broken", [], {"dpi": "abc"}, {"width": 2.5}, {"raw_input": 1},
                     {"memory_mb": 18446744073709551615}, {"unknown": True}, {"cm360": None},
                     b'{"x":' + b'[' * 100 + b'0' + b']' * 100 + b'}']:
            with self.assertRaises(urllib.error.HTTPError) as error:
                self.request("/api/settings", body)
            self.assertEqual(error.exception.code, 400)
        self.assertEqual(self.request("/api/state")["settings"], before)

    def test_preset_preserves_mouse_calibration(self):
        calibration = {"dpi": 1800, "cm360": 46.5, "sensitivity": 0.6, "raw_input": False}
        self.request("/api/settings", calibration)
        result = self.request("/api/settings", {"preset": "quality"})
        for key, value in calibration.items():
            self.assertEqual(result[key], value)

    def test_cross_origin_and_unauthorized_requests_denied(self):
        for headers in [{"Origin": "https://example.org"}, {"Host": "evil.example"}]:
            with self.assertRaises(urllib.error.HTTPError) as error:
                self.request("/api/demo", {}, headers)
            self.assertEqual(error.exception.code, 403)
        anonymous = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with self.assertRaises(urllib.error.HTTPError) as error:
            anonymous.open(self.origin + "/api/state", timeout=5)
        self.assertEqual(error.exception.code, 403)
        cookie = next(iter(self.cookies))
        self.assertTrue(cookie.has_nonstandard_attr("HttpOnly"))
        self.assertEqual(cookie.get_nonstandard_attr("SameSite"), "Strict")

    def test_duplicate_length_rejected(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.putrequest("POST", "/api/settings")
            connection.putheader("Content-Length", "2")
            connection.putheader("Content-Length", "10")
            connection.endheaders(b"{}")
            self.assertEqual(connection.getresponse().status, 400)
        finally:
            connection.close()

    def test_diagnostics_and_unknown_route(self):
        result = self.request("/api/diagnostics")
        self.assertFalse(result["capabilities"]["game"])
        self.assertTrue(result["limitations"])
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request("/../../etc/passwd")
        self.assertEqual(error.exception.code, 404)


if __name__ == "__main__":
    unittest.main(verbosity=2)
