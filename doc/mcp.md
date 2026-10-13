# C++ MCP server

`qucsrflayout-mcp` is an optional local MCP server. It calls the existing
C++ conversion code directly. Qt6 Core provides JSON parsing and background process management; no GUI is launched.
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
with no arguments, or configure the optional `--octave` and `--openems-prefix`
arguments. For clients accepting `mcpServers` JSON:

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
Closing stdin ends the server and cancels active simulation jobs. No HTTP
listener or network access is required. Restart your MCP client/session after
rebuilding the server to load the new tool definitions.

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

Conversion requires an existing netlist exported by Qucs/Qucs-S. It accepts
Qucs schematic headers, not QucsStudio schematics. Conversion does not launch
Qucs; use `start_simulation` separately to execute an exported OpenEMS script. Existing upstream restrictions on
microstrip components and schematic connectivity still apply. Schematics using
equations may need their associated simulation `.dat` file, as in ordinary CLI use.

Conversions run sequentially in the server process; conversion cancellation and
conversion timeouts are not implemented. Simulation jobs have separate process
management, cancellation and timeouts as described below.

Protocol reference: https://modelcontextprotocol.io/specification/2025-06-18

## GNU Octave and OpenEMS setup

Install GNU Octave, the OpenEMS solver, its OpenEMS/CSXCAD Octave interfaces,
and an Octave graphics backend for generating plots. The default Octave command
is `octave-cli`. Conversion remains available when Octave is not installed;
starting a simulation then returns a dependency error.

The OpenEMS library prefix is selected in this order:

1. `--openems-prefix /path/to/openEMS` supplied when launching the server.
2. The `OPENEMS_PREFIX` environment variable.
3. A detected installation under `~/opt/openEMS`, `/usr/local`, or `/usr`.

A prefix should contain both `share/openEMS/matlab/InitFDTD.m` and
`share/CSXCAD/matlab/InitCSX.m`. If no prefix is detected, Octave uses its existing
library search path. Other package layouts can be configured through Octave's
normal startup configuration.

For an explicit local configuration:

```json
{
  "mcpServers": {
    "qucs-rflayout": {
      "command": "/home/anton/qusc-rflayout/Qucs-RFlayout/build/qucsrflayout-mcp",
      "args": ["--octave", "/usr/bin/octave-cli", "--openems-prefix", "/home/anton/opt/openEMS"]
    }
  }
}
```

With Codex CLI, register the same command using:

```sh
codex mcp add qucs-rflayout -- /home/anton/qusc-rflayout/Qucs-RFlayout/build/qucsrflayout-mcp --octave /usr/bin/octave-cli --openems-prefix /home/anton/opt/openEMS
```

The server adds the interface paths before sourcing the script. It uses `QProcess`
with an argument list to launch Octave. Simulation stdout and stderr are written
to the job's log file; MCP stdout stays reserved for protocol messages.

## Tool: `start_simulation`

```json
{
  "script": "/absolute/path/simulation.m",
  "output_directory": "/absolute/path/new-simulation-job",
  "timeout_seconds": 3600,
  "preprocess_only": false
}
```

`script` must be an existing local `.m` file of at most 16 MiB. The output
directory must not exist. A copy named `job_input.m`, a `runner.m`, `job.json`
and `simulation.log` are created there. The input script is copied to make the
job reproducible. The job directory becomes Octave's working directory, so
relative generated result paths stay there. Self-contained Qucs-RFlayout exports
work directly; custom scripts needing data files must use appropriate paths.

The response returns a `job_id`, initial `state`, output and log paths. The job
runs in the background; MCP stays available for polling while the client is idle.
Only one simulation may be active per server. Additional starts return an error
rather than queueing another solver.

`timeout_seconds` defaults to 3600 and accepts integers from 1 through 86400.
The timeout starts when the Octave process starts and includes solver execution
and postprocessing. `preprocess_only` defaults to false. When true, it passes
`--only-preprocess` to the script. Qucs-RFlayout's generated scripts honor this
flag to construct geometry/mesh without solving; arbitrary custom scripts must
implement the flag themselves. All runs also pass `--no-gui --batch` to the script
and disable visible figures by default, allowing plots to be saved headlessly.

For the 10 GHz filter, call `start_simulation` with:

```json
{
  "script": "/home/anton/qusc-rflayout/designs/ro4350b_lpf_10ghz/openems/simulation.m",
  "output_directory": "/home/anton/qusc-rflayout/designs/ro4350b_lpf_10ghz/octave_run_01",
  "timeout_seconds": 7200
}
```

Use a different output directory for a preprocessing check or each repeated run.

## Tool: `simulation_status`

```json
{"job_id": "returned-job-id", "log_tail_lines": 30}
```

States are `starting`, `running`, `cancelling`, `completed`, `failed`, `cancelled`
and `timed_out`. The response includes elapsed time, timestamps, log paths,
stop reason when present, and the process exit code when finished. The optional
log tail defaults to 30 lines and accepts 0–200. At most 64 KiB of log is read.
Missing executables are reported before creating a job directory. Runtime errors,
missing script dependencies and nonzero Octave exit codes become failed jobs.

## Tool: `simulation_results`

```json
{"job_id": "returned-job-id"}
```

Returns status plus paths, kinds and sizes of generated plots, Touchstone
S-parameter files, data and XML/CSX geometry. The listing contains at most 2000
artifacts and reports truncation. Symbolic links are excluded. Files from active,
failed, timed-out or cancelled jobs are marked `partial: true`. Successful
preprocessing is identified by `preprocess_only: true` and does not mean the
solver ran. `simulation_execution_requested` identifies the requested mode.

`completed` means Octave exited normally with status zero. It does **not** establish
physical accuracy or convergence: inspect solver logs for timestep/energy limits,
check mesh convergence, and validate port placement. Executing a script that
swallows its own errors can also exit zero; callers should inspect its artifacts.

## Tool: `cancel_simulation`

```json
{"job_id": "returned-job-id"}
```

Cancellation first requests termination, then forces shutdown after three seconds
if needed. On Unix, Octave and its solver descendants are placed in a separate
process group and terminated together. Other platforms currently terminate the
immediate Octave process without process-tree support. Already finished jobs are
left unchanged. Timeouts use the same shutdown mechanism.

Job IDs are tracked by the current server process. Normal MCP stdin closure
cancels active jobs. Generated files and final `job.json` metadata remain on disk;
job IDs cannot be queried or resumed after restarting the server. Force-killing
or crashing the server bypasses normal cleanup.

Octave scripts run with the launching user's filesystem and process permissions.
Use trusted local scripts. The server does not impose a filesystem sandbox or
provide remote authentication.

## Simulation integration checks

The real-Octave lifecycle test is enabled when Octave is found at configure time:

```sh
ctest --test-dir build -R 'MCP.*Integration' --output-on-failure
```

An optional test runs a small actual OpenEMS transmission-line simulation through
MCP and checks probe data, a plot and S-parameter artifacts:

```sh
cmake -S . -B build -DQRFL_MINIMAL=ON -DQRFL_BUILD_MCP=ON -DQRFL_OPENEMS_TEST_PREFIX=/path/to/openEMS
cmake --build build --target qucsrflayout-mcp
ctest --test-dir build -R MCPOpenEMSSmoke --output-on-failure
```

This smoke test verifies the solver integration; it does not validate a filter's
RF response.
