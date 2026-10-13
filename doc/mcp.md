# C++ MCP server

`qucsrflayout-mcp` is an optional local MCP server. It calls the existing
C++ conversion code directly. Qt6 Core provides JSON parsing; no GUI is launched.
The regular executable and GUI continue to build as before.

## Build

Install a C++23 compiler, CMake, and Qt6 Core development files, then run:

```sh
cmake -S . -B build -DQRFL_MINIMAL=ON -DQRFL_BUILD_MCP=ON
cmake --build build --target qucsrflayout-mcp -j4
ctest --test-dir build -R MCPIntegration --output-on-failure
```

`QRFL_BUILD_MCP` defaults to OFF. Enabling it adds a Qt6 Core dependency even
when `QRFL_MINIMAL` is ON. Installation puts the server in `bin`.

## Connect

Configure an MCP client to launch the absolute path to `build/qucsrflayout-mcp`
with no arguments. For clients accepting `mcpServers` JSON:

```json
{
  "mcpServers": {
    "qucs-rflayout": {
      "command": "/home/anton/qusc-rflayout/Qucs-RFlayout/build/qucsrflayout-mcp"
    }
  }
}
```

The server uses newline-delimited JSON-RPC on stdin/stdout, with diagnostics on
stderr. It supports protocol versions 2024-11-05, 2025-03-26 and 2025-06-18.
The client must initialize and send `notifications/initialized` before tools.
Closing stdin ends the server. No HTTP listener or network access is required.

## Tool: `convert_layout`

```json
{
  "schematic": "/absolute/path/circuit.sch",
  "netlist": "/absolute/path/circuit.net",
  "output_directory": "/absolute/path/new-export-directory",
  "format": ".kicad_pcb"
}
```

All paths refer to files on the server's machine. `format` defaults to
`.kicad_pcb`; `.kicad_mod`, `.lht`, `.m`, and `.svg` are also supported.
The output directory must not exist. This prevents ordinary repeated requests
from overwriting earlier exports. The server creates it after reading the input.
Successful results contain text with a JSON object listing absolute output file
paths and the selected format. Conversion failures return `isError: true`;
protocol failures return JSON-RPC errors. Detailed converter diagnostics are on
stderr. Failed writes can leave incomplete files.

This first version requires an existing netlist exported by Qucs/Qucs-S. It
accepts Qucs schematic headers, not QucsStudio schematics. It neither launches
Qucs nor executes generated OpenEMS scripts. Existing upstream restrictions on
microstrip components and schematic connectivity still apply. Schematics using
equations may need their associated simulation `.dat` file, as in ordinary CLI use.

Conversions run sequentially in the server process; cancellation and conversion
timeouts are not implemented. The process has the filesystem permissions of the
user launching it, so use trusted local design files. It does not impose a
workspace sandbox or provide remote authentication.

Protocol reference: https://modelcontextprotocol.io/specification/2025-06-18
