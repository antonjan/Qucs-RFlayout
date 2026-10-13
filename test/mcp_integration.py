#!/usr/bin/env python3
"""Exercise the real stdio server, including conversion of the patch fixture."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary, repository = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory() as directory:
    with tempfile.TemporaryFile() as diagnostics:
        server = subprocess.Popen([str(binary)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=diagnostics, text=True)
        def send(method, params=None, request_id=1):
            message = {"jsonrpc": "2.0", "method": method}
            if params is not None:
                message["params"] = params
            if request_id is not None:
                message["id"] = request_id
            server.stdin.write(json.dumps(message) + "\n")
            server.stdin.flush()
            if request_id is not None:
                response = json.loads(server.stdout.readline())
                assert response["id"] == request_id, response
                return response
        try:
            assert send("tools/list")["error"]["code"] == -32600
            assert send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "test", "version": "1"}})["result"]["protocolVersion"] == "2025-06-18"
            send("notifications/initialized", request_id=None)
            assert send("tools/list")["result"]["tools"][0]["name"] == "convert_layout"
            assert send("ping")["result"] == {}
            assert send("unknown")["error"]["code"] == -32601
            assert send("tools/call", {"name": "unknown", "arguments": {}})["error"]["code"] == -32602
            arguments = {"schematic": str(repository / "test/patch/patch.sch"),
                         "netlist": str(repository / "test/patch/patch.net")}
            for index, format_name in enumerate([".kicad_pcb", ".kicad_mod", ".lht", ".m", ".svg"]):
                arguments.update(output_directory=str(Path(directory) / str(index)), format=format_name)
                result = send("tools/call", {"name": "convert_layout", "arguments": arguments})["result"]
                assert not result["isError"], result
                files = json.loads(result["content"][0]["text"])["files"]
                assert files and all(Path(file).stat().st_size > 100 for file in files)
                assert all(Path(file).suffix == format_name for file in files)
            assert send("tools/call", {"name": "convert_layout", "arguments": arguments})["result"]["isError"]
            arguments.update(output_directory=str(Path(directory) / "invalid"), format=".bad")
            assert send("tools/call", {"name": "convert_layout", "arguments": arguments})["result"]["isError"]
            arguments.update(format=".svg", schematic=str(Path(directory) / "missing.sch"))
            assert send("tools/call", {"name": "convert_layout", "arguments": arguments})["result"]["isError"]
            assert send("ping")["result"] == {}
            server.stdin.write("broken JSON\n")
            server.stdin.flush()
            assert json.loads(server.stdout.readline())["error"]["code"] == -32700
            assert send("ping")["result"] == {}
        finally:
            server.stdin.close()
            try:
                assert server.wait(timeout=10) == 0
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()
print("MCP protocol and all five export formats passed")
