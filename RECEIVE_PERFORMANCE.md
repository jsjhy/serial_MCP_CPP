# Synthetic receive/resource investigation — 2026-11-03

Environment: Windows x64, WinLibs GCC 16.1.0, Release. Three serial sessions,
4096-byte ANSI/ASCII/Unicode records, one producer and real log/Core/RichEdit paths.
No physical serial port is opened. The producer sleeps 2 ms per three-record round;
Windows timer scheduling determines the achieved rate. Memory samples are process private
bytes and working set, not a claim about peak system-wide RAM.

## Findings

The previous GUI drained up to 256 records on each timer callback and performed RichEdit
selection/format/insertion/scroll/redraw separately per record and color run. A backlog
caused one dispatched message to occupy the UI thread for 35,696.37 ms.

The fix bounds raw display work, composes partial-line replacements, streams one escaped
RTF batch, renders only visible pages and batches printable ASCII conversions. An RAII
guard restores painting on exceptions. UI queue eviction is counted separately from logs.

## Measurements

| Run | Duration | Injected bytes | Worst dispatched-message time |
| --- | ---: | ---: | ---: |
| Before, ASCII | 37.3 s actual (18 s requested; callback overran deadline) | 29,073,408 | 35,696.37 ms |
| Batch rendering, ASCII | 18 s | 14,000,128 | 46.71 ms |
| Final, ASCII then HEX | 60 s | 46,546,944 | 99.52 ms |

The last 20 seconds of the final run show the following process resource ranges:

| Metric | Minimum | Maximum |
| --- | ---: | ---: |
| Private bytes | 31,920,128 | 32,497,664 |
| Working-set bytes | 51,875,840 | 52,559,872 |
| Handles | 285 | 285 |
| GDI objects | 49 | 49 |
| USER objects | 81 | 81 |
| Threads | 17 | 17 |

Private memory initially grows as bounded histories, inactive display queues and RichEdit
caches fill. In this run it plateaus rather than growing continuously; GDI, USER, thread
and handle counts are stable after warm-up. This is evidence against an immediate leak in
this workload, not proof of long-running leak freedom. Physical serial I/O, removal during
sustained hardware traffic, slow/failing storage and much longer sessions still need real
hardware testing.

Raw diagnostic output remains in ignored build/rx-stress-before.txt,
build/rx-stress-after-rtf.txt and build/rx-stress-after.txt.
