# ISSUE EVIDENCE - The Order 1886 port, survey T-001 (2026-11-12)

Read-only survey. Sources: `gh issue view N --comments` (body + comments) for the 12 issues; attachments downloaded to
`<session temp>\issues\` (folders `issue-NN\`).
Rule applied: "not stated" = absent from issue body and comments and attachments. Nothing deduced.
Note on logs: the `last_run.log` files do NOT contain a port version line; the port version below comes from the bug-report `report.md` inside the zip where present, or from the issue text.

## 1. One row per issue

| issue | port build/version | game SKU + version | GPU + driver | CPU/RAM if stated | chapter / checkpoint / scene (literal) | settings | symptom (reporter's words) | attachments present | what is missing to reproduce |
|---|---|---|---|---|---|---|---|---|---|
| #118 | body: 0.8.0. zip report.md: "Release build (git metadata absent)" (no number) | body: CUSA00785. log: CUSA00076 01.02, SKU=full (mismatch) | body: Radeon 890M. log: AMD proprietary 2.0.416, Vulkan 1.4.359 | body: Ryzen 9 AI HX 370. zip: 23.6 GB RAM, 24 logical cores | "3/after saving a woman from guy, unlock the door/ freezes+crashes inside the house" | body: 1080p. zip: fullscreen 1920x1080, upscaler fsr4, 60 FPS, VSync on. log: FSR 3.1 on, MSAA off | "game freezes and then crashes" | zip theorder1886-report-20261110-2300.zip (report.md + last_run_sanitized.log). No video. | exact save / timestamp; video; which save; the body's CUSA00785 does not match the log's CUSA00076 |
| #95 | not stated | not stated | not stated | not stated | not stated | "no matter the settings" | "The game textures flicker like crazy no matter the settings." | none. Comment by m0n4rch-sys: "Dealing with same issue" (no data) | version, SKU, GPU, CPU, scene, settings, log, video - all |
| #99 | body: windows-0.7.9 | body: CUSA000035 1.2 | body: RTX 5090, driver 621.01 | body: Ryzen 7 9850X3D. RAM not stated | "Chapter 1, Introduction cutscene and gameplay after" | body: "1080, 16:9, NVIDIA DLSS x1.0 Native" | "Textures are missing, such as eyes everynow and then. There are black flickers all over." | none (Log: "_No response_", video: "_No response_"). Comment by Wontell: "roll back to 0.6.1, should solve the issue" (no data) | log; video/screenshot; RAM; exact place inside chapter 1 |
| #30 | body: v0.7.4. Comment by mobin172: "updated to 0.7.5 still the same problem exists" | not in body. log: CUSA00785 01.02 | body: RTX 4080 super. log: NVIDIA 616.64.0.0, Vulkan 1.4.351 | body: Ryzen 9 7950x. RAM not stated | not stated (body: "when fight scenes start") | body: "tried 60 and 120 both are stable". log: 60 FPS, borderless, upscaler off | "everything is flickering and hiding and re appearing very fast , its hard to watch"; "when fight scenes start game starts stuttering" | last_run.log (33686470, downloaded, 1,088,095 bytes); YouTube link youtu.be/putcdkWZZU0; image in body | chapter, scene, timestamp; video is a link only (not downloaded) |
| #29 | body: v0.7.4. Comment by crylessengine: "same issue in v0.7.5" | log (pasted inline in comment, not a file): CUSA00035 01.02 | body: RTX 3090, driver 617.42. log: NVIDIA 617.42.0.0, Vulkan 1.4.351 | body: AMD Threadripper 3990x. RAM not stated | not stated (steps: "Play from beginning"; owner: "beginning-of-game route") | body: "Switching the aspect ratio from 16:9 to original reduces it but does not fully fix it". Tested at 30/60/120 FPS. Log: 60 FPS, fullscreen, VSync on | "Black polygons and texture flicker." | three YouTube links (Sr8nEgWYCB0, fKNKDzmAaD4, V79kzuLApEs); inline log paste in comment; no downloadable log file | exact timestamp/checkpoint; downloadable log; video file; owner asked for "exact timestamp/checkpoint, a short video and a fresh redacted runtime log" |
| #98 | body: "0.7 Series". zip 20261028: "Release build (git metadata absent)". zip 20261111: 0.8.3 | log: CUSA00076 01.02, SKU=full | body: rtx 5090. log: NVIDIA 621.1.0.0 (zip 1028), 621.14.0.0 (zip 1111), Vulkan 1.4.357 | body: 9950X3D. zip: 63.6 GB RAM, 32 logical cores | "boot" | body: "ALL". zip 1028: borderless 3840x2160, upscaler off, 60 FPS, HDR off. zip 1111: borderless 3840x2160, upscaler dlss, HDR on, log shows FSR 3.1 on. Comment by Wontell: "Two Stage GPU Pipeline from 8+ to OFF" changes the symptom | "Game starts i see logo, black screen, then menu, i start game > Black screen, i hear the sounds"; later "black triangles shifting from the doors, doorways and arches" | two zips (report.md + last_run_sanitized.log each); no video | steps beyond "boot"; body settings are "ALL" |
| #116 | body: "Last working version v0.6.1" (failing version not stated). zip 20261111: 0.8.3. zip 20261109: "Release build" (no number) | log: CUSA00076 01.02, SKU=full | body: Rx 6500 XT. log: AMD proprietary 2.0.353, Vulkan 1.4.315 | body: Ryzen 3500x. zip: 15.9 GB RAM, 6 logical cores | "Full Black Screen" | body: "Everything has been tried". zip 1835: windowed 1920x1080, upscaler fsr411, target 0 FPS. log: 120 FPS, windowed, FSR 3.1 on | "In all versions except V0.6.1, there is only a black screen; only the text is visible!" | zip 20261109 (report only: "No log output available."); zip 20261111 (report + sanitized log); last_run.log (34694771, comment, 109,197 bytes); screenshot in body | which version failed exactly; a run with video; timestamp |
| #63 | body: v0.7.6-rc.1. Retests by galaxysyed on v0.7.7 and on v0.6.1 (comments) | log: CUSA00076 01.02, SKU=full | body: RX 9070, Adrenalin 26.8.1. log: AMD proprietary 2.0.395, Vulkan 1.4.349. (Comment by sadeq1375 mentions a different machine: RX 6900XT, i5 14600k) | body: Ryzen 7 7700. RAM not stated | "Play Chapter 8 and 9" (body); comments: "Chapter 8 from beginning", "chapter 9". No checkpoint | body: not stated. logs: a 60 FPS windowed upscaler on; b, c 60 FPS windowed upscaler off; d, e (v0.6.1) 30 FPS windowed | "heavy stutters, FPS drops, and constant blue/green artifacts throughout" | five last_run.logs: a = body (33790039), b = ch8 v0.7.7 (33841668), c = ch9 v0.7.7 (33843407), d = ch8 v0.6.1 (33880675), e = ch9 v0.6.1 (33880854). Videos as links in body and comments | exact checkpoint/timestamp; RAM; upscaler in body; screenshot per artifact |
| #39 | body: v0.7.5. Retest on v0.7.7 in comment | log: CUSA00076 01.02, SKU=full | body: RX 9070, Adrenalin 26.8.1. log: AMD proprietary 2.0.395, Vulkan 1.4.349 | body: Ryzen 7 7700. RAM not stated | "Play Chapter 5" (chapter only) | body: not stated. log a: 60 FPS borderless upscaler on. log b: 60 FPS windowed upscaler off | "I've been seeing colorful artifacts, like random green and blue flares showing up in different spots." | two last_run.logs (33712694 body; 33844025 comment); five 2560x1440 photos (links); video link in comment | exact camera / checkpoint / timestamp; settings in body |
| #11 | body: v0.3.7. Maintainer later asked for a 0.4.0 retest (no 0.4.0 log in thread) | log: CUSA00076 01.02, SKU=full; log shows 30 FPS fullscreen | body: RX Vega 64, Adrenalin 26.5.2. log 5.txt: AMD proprietary 2.0.279, Vulkan 1.3.260 | body: Ryzen 7 5700X. Comment by dante3732: 32 GB RAM, page file System managed 2048 MB | body: "Load game in start chapter 9"; "crash between the end of chapter 8 and the start of chapter 9" | not stated in body. Logs: 30 FPS, fullscreen | "I had a crash between the end of chapter 8 and the start of chapter 9. Same happened in SHADPS4." | three text logs (3245.txt body; 2.txt and 5.txt comments); links to shadPS4 PR #4798 comments | save file (only linked to shadPS4 PR); timestamp; video; upscaler; the maintainer's "VK_ERROR_OUT_OF_HOST_MEMORY" is not present in any attached log |
| #120 | body: 0.8.1. zip report.md: 0.8.1 | body: cusa00785 versao 01,02. log: CUSA00785 01.02 | body: rx 580 2048sp, driver 26.5.2. log: AMD proprietary 2.0.279, Vulkan 1.3.260 | body: xeon 2680 v4. zip: 15.8 GB RAM, 28 logical cores | "capitulo 1" | body: "1080p fsr 3.1". zip: borderless 1920x1080, upscaler fsr3, 60 FPS, VSync on | "erros graficos" (body). Title: "bug graficos espalhados pelo capitulo 1, compilação de shader fazendo jogo travar bastante" | zip theorder1886-report-20261108-2333.zip (report.md + sanitized log, 458,985 bytes). Three videos failed to upload (placeholder comments only) | the graphics error itself; timestamp; videos; steps (body: "so jogar") |
| #114 | body: V0.7.9 | body: cusa00035 v1.02 | body: rtx 3050 6gb. driver not stated | not stated | "at first mission itself" (steps: "gameplay at first mission itslef") | not stated | "launchewr works but game stutter a lote from beginning and very low FPS" | none ("Log / diagnostic report: _No response_"; no video) | log; chapter/mission name; driver; RAM/CPU; FPS numbers; settings |

## 2. Issue #118 - crash detail (attachment read)

Zip: `<session temp>\issues\issue-118\theorder1886-report-20261110-2300\` (report.md, last_run_sanitized.log, 357,395 bytes).

- **Failure type:** assert ("GPU library assertion failed", text `Unreachable code!` at `liverpool.h:208`), followed by an unhandled C++ exception `std::bad_alloc`. Exit code 23. Not a guest fault at the crash point. The only "Read fault" line (`Read fault: rip 000000080005DDE5 reads 0x106ee88000`, log line 546, early in the run, followed by normal flips) appears in almost every log in this survey and is not the crash. The `std::bad_alloc` is the closest thing to an out-of-memory signal; there is no `VK_ERROR` or explicit out-of-memory line.
- **Faulting address:** none printed at the crash. Only the early benign read fault above.
- **Stack:** none printed. Neither the log nor report.md contains a call stack.
- **Memory context stated in the files:** 23.6 GB RAM (20.1 GB free at report time), pagefile 31.6 GB; guest pool 6080 MiB (dmem 5056 + flex 1024); GPU VRAM budget 11078 MiB (iGPU).
- **Last 30 relevant log lines (verbatim, from the end of the sanitized log):**

```
Pipeline: 125.5 ms in the driver in the background (compiled, not stored yet, key 62a84a86646da554, stages 00000dab44ea6a35 0000000000000000 0000000000000000 000007e0bdf48a77 0000000000000000 0000000000000000)
frame 9627 draw 0 img#0 vs/ps 00000dab44ea6a35 0000000000000000 STALLED
Stall: 141.1 ms frame; waits: readback 0.0 ms, GPU ticks 9.5 ms, recorder 0.6 ms, host copies 0.8 ms; 0 compiles 0.0 ms (translate 0.0, SPIR-V 0.0, driver 125.5); GC 0.0 ms
       fault handlers: 0 read faults 0.0 ms, write faults 0.0 ms
       GPU thread on CPU 0.0 ms; 4 images registered, 1.8 MB image uploads, 1.0 MB buffer uploads
       966 draws, 98 dispatches, 0 submissions; GPU thread user 0.0 ms, kernel 0.0 ms, 0 preempted, 0 waits
       ms in: resident 0.0, protect 0.0, image create 0.0, image refresh 0.0, staging 0.0, waiting for host copies 0.0
       GPU thread page faults 0 (process 0), protection faults 0; protect calls 46 (1020 pages), of which write-revoking 25 (750 pages)
frame 9647 draw 0 img#0 vs/ps 00000189c9e9d0d1 0000000000000000 STALLED
Stall: 83.7 ms frame; waits: readback 0.0 ms, GPU ticks 0.0 ms, recorder 0.0 ms, host copies 0.7 ms; 1 compiles 5.5 ms (translate 0.0, SPIR-V 0.0, driver 49.1); GC 0.0 ms
       826 draws, 148 dispatches, 0 submissions; GPU thread user 0.0 ms, kernel 0.0 ms, 0 preempted, 0 waits
Pipeline: 175.0 ms in the driver in the background (compiled, not stored yet, key a3289affcd77cf43, stages 00000e0b206316b5 0000000000000000 0000000000000000 0000052809520d3b 0000000000000000 0000000000000000)
frame 9648 draw 0 img#0 vs/ps 00000e0b206316b5 0000000000000000 STALLED
Pipeline: 254.3 ms in the driver in the background (compiled, not stored yet, key 01b1613d78ac38ce, stages 00000de9679d4876 0000000000000000 0000000000000000 0000042a392fb602 0000000000000000 0000000000000000)
frame 9648 draw 1 img#0 vs/ps 00000de9679d4876 0000000000000000 STALLED
Stall: 230.8 ms frame; waits: readback 0.0 ms, GPU ticks 7.4 ms, recorder 0.2 ms, host copies 0.7 ms; 0 compiles 0.0 ms (translate 0.0, SPIR-V 0.0, driver 429.3); GC 0.0 ms
       1052 draws, 185 dispatches, 0 submissions; GPU thread user 0.0 ms, kernel 0.0 ms, 0 preempted, 0 waits
frame 9658 draw 0 img#0 vs/ps 00000189c9e9d0d1 0000000000000000 STALLED
VideoOut: sceVideoOutSubmitFlip #9660 (handle=1, buf=0, mode=3, arg=0)
VideoOut: Flip #9660 index=0 arg=0 prev=1 pending=1
Pipeline: 193.7 ms in the driver in the background (compiled, not stored yet, key 385a83d30815ab24, stages 00000b603f8b8774 0000000000000000 0000000000000000 0000052809520d32 0000000000000000 0000000000000000)
GPU [Debug] <Critical> liverpool.h:208 unhandled_exception: Unreachable code!
Unhandled exception: std::bad_alloc
STOP: GPU library assertion failed (see GPU log above)

The game exited (code 23).
```

- **Last thing the log shows before the crash:** `Pipeline: 193.7 ms in the driver in the background (compiled, not stored yet, key 385a83d30815ab24 ...)`, right after `VideoOut: Flip #9660`, with frame 9658 having stalled just before. The crash line is the `liverpool.h:208` assert. Frames 9627-9658 were all STALLED (83-230 ms each) in the preceding ~30 frames, with 0 read faults and 0 write faults reported in those stalls.
- **Not in the log:** port version line (only report.md, which says "Release build (git metadata absent)"), any VK_ERROR, any error 14xx line, any save/timestamp for the run in the issue body.

## 3. Logs read: version, GPU, and error markers

Counts are grep counts over the whole file. STALLED counts are lines, not frames.

- **#11 2.txt** (comment): port version line: none. GPU: `GPU 0: Radeon RX Vega (DiscreteGpu)`. Driver line: not stated in file. Assert: `GPU [Debug] <Critical> vk_platform.h:60 Check: Assertion Failed!`, then `STOP: GPU library assertion failed (see GPU log above)`. VK_ERROR 0, error 14xx 0, STALLED 0, Read fault 0. Also: `io_file.cpp:202 Open: Failed to open the file ... .meta, error_message=No such file or directory` (lines before the assert).
- **#11 3245.txt** (body): port version: none. GPU: `GPU 0: Radeon RX Vega (DiscreteGpu)`. Assert: `GPU [Debug] <Critical> vk_platform.h:56 Check: Assertion Failed!`. VK_ERROR 0, error 14xx 0, STALLED 0, Read fault 0. Same missing `.meta` open errors.
- **#11 5.txt** (comment): port version: none. GPU: `GPU: Radeon RX Vega, 6433 MiB VRAM budget, driver AMD proprietary driver 2.0.279, Vulkan 1.3.260`. Read fault: `Read fault: rip 000000080005DDE5 reads 0x106ee88000`. Assert: `vk_platform.h:60 Check: Assertion Failed!`. `The game exited (code 23).` VK_ERROR 0, error 14xx 0, STALLED 0.
- **#116 last_run.log** (comment 34694771; same content as zip 20261111): port version: 0.8.3 (zip report.md only). GPU: `GPU: AMD Radeon RX 6500 XT, 3012 MiB VRAM budget, driver AMD proprietary driver 2.0.353, Vulkan 1.4.315`. Settings in log: 120 FPS, windowed, FSR 3.1 on. STALLED 3. Read fault 1 (benign). No assert, no VK_ERROR, no error 14xx.
- **#116 zip 20261109**: contains only report.md, log section is `No log output available.` - no log to read.
- **#118**: see section 2. GPU: `GPU: AMD Radeon(TM) 890M Graphics, 11078 MiB VRAM budget, driver AMD proprietary driver 2.0.416, Vulkan 1.4.359`. Assert `liverpool.h:208 unhandled_exception: Unreachable code!`; `std::bad_alloc`; exit code 23.
- **#120 zip 20261108** (sanitized log): port version: 0.8.1 (zip report.md). GPU: `GPU: AMD Radeon RX 580 2048SP, 6446 MiB VRAM budget, driver AMD proprietary driver 2.0.279, Vulkan 1.3.260`. STALLED 127. Read faults 2 (rip 000000080005DDE5; rip 000000080086AB90 reads 0x106ea85520). No assert, no VK_ERROR, no error 14xx.
- **#30 last_run.log** (body, 1,088,095 bytes): port version: none in log (issue says v0.7.4). GPU: `GPU: NVIDIA GeForce RTX 4080 SUPER, 14253 MiB VRAM budget, driver NVIDIA 616.64.0.0, Vulkan 1.4.351`. Log settings: 60 FPS, borderless, upscaler off. STALLED 0. Read faults 2 (rip 000000080005DDE5; rip 00007FFACB632919 reads 0x105fc66000). No assert, no VK_ERROR, no error 14xx.
- **#29 inline log paste** (comment by crylessengine, 2026-10-23; not a file): port version: none (issue says v0.7.4). GPU: `GPU: NVIDIA GeForce RTX 3090, 22530 MiB VRAM budget, driver NVIDIA 617.42.0.0, Vulkan 1.4.351`. Log: 60 FPS, fullscreen. STALLED lines 3 (frames 21266, 21403, 21637), plus `Frame stats: 57.1 FPS, worst frame 262.4 ms`; `Stall: 262.4 ms frame`, the `driver 262.0` compile share. No `Read fault` lines, no assert, no VK_ERROR, no error 14xx.
- **#39 last_run_a** (body, 3,563,693 bytes): port version: none (issue says v0.7.5). GPU: `GPU: AMD Radeon RX 9070, 14392 MiB VRAM budget, driver AMD proprietary driver 2.0.395, Vulkan 1.4.349`. Log: 60 FPS, borderless, upscaler on. STALLED 0. Read faults 2. No assert, no VK_ERROR, no error 14xx.
- **#39 last_run_b** (comment 33844025, 593,147 bytes): same GPU line. Log: 60 FPS, windowed, upscaler off. STALLED 712. Read fault 1. No assert, no VK_ERROR, no error 14xx.
- **#63 a** (body 33790039, 403,592 bytes): GPU same RX 9070 / 2.0.395 / Vulkan 1.4.349. 60 FPS, windowed, upscaler on. STALLED 135. Read fault 1. No assert, no VK_ERROR, no error 14xx.
- **#63 b** (ch8, v0.7.7 retest, 814,836 bytes): same GPU. 60 FPS, upscaler off. STALLED 1095. Read fault 1.
- **#63 c** (ch9, v0.7.7 retest, 300,189 bytes): same GPU. 60 FPS, upscaler off. STALLED 271. Read fault 1.
- **#63 d** (ch8, v0.6.1 test, 496,892 bytes): same GPU. 30 FPS, windowed. STALLED 0. Read faults 2.
- **#63 e** (ch9, v0.6.1 test, 249,339 bytes): same GPU. 30 FPS, windowed. STALLED 0. Read fault 1.
- **#98 zip 20261028** (sanitized log, 351,910 bytes): port version: "Release build (git metadata absent)" (report.md). GPU: `GPU: NVIDIA GeForce RTX 5090, 30395 MiB VRAM budget, driver NVIDIA 621.1.0.0, Vulkan 1.4.357`. Log: 60 FPS, borderless, upscaler (FSR) off. STALLED 455. Read fault 1. `The game exited (code 0).` No assert, no VK_ERROR, no error 14xx.
- **#98 zip 20261111** (sanitized log, 192,811 bytes): port version: 0.8.3 (report.md). GPU: `GPU: NVIDIA GeForce RTX 5090, 30395 MiB VRAM budget, driver NVIDIA 621.14.0.0, Vulkan 1.4.357`. Log: 60 FPS, borderless, FSR 3.1 on. STALLED 11. Read fault 1. No assert, no VK_ERROR, no error 14xx.
- **#120**: see above.

Across all logs read: zero `VK_ERROR` lines, zero `error 14xx` lines, zero `Assert` lines except in #11 (three logs) and #118 (one assert plus bad_alloc). The benign startup line `Read fault: rip 000000080005DDE5 reads 0x106ee88000` appears in almost every log and is not the crash.

## 4. Which issues name a precise, reachable scene (chapter + place)

Precise (chapter and place both named):
- **#118**: "Chapter 3, after saving a woman from a guy, unlock the door and go into the house" (freezes and crashes inside the house).
- **#11**: the boundary "end of chapter 8 / start of chapter 9" with a save at the start of chapter 9 (save is only linked, via shadPS4 PR #4798; the maintainer reported loading it on their own machine).

Not precise (chapter only, a generic phase, or nothing):
- #99 "Chapter 1, Introduction cutscene and gameplay after" (chapter and phase, no place)
- #63 "Chapter 8 and 9" (no checkpoint)
- #39 "Chapter 5" (no place)
- #120 "capitulo 1" (no place)
- #30 "when fight scenes start" (no chapter, no place)
- #29 "beginning" / "Play from beginning" (no chapter, no place)
- #98 "boot"
- #116 "Full Black Screen"
- #114 "at first mission itself" (no chapter, no place)
- #95 nothing stated

## 5. Attachments and downloads

- Downloaded and read: #118 zip; #98 two zips; #116 zip 20261111 and zip 20261109 (report only), last_run.log (comment); #120 zip; #30 last_run.log; #63 five logs; #39 two logs; #11 three txt files. Scratchpad folder: `<session temp>\issues\`.
- Not downloaded (links only): YouTube and GitHub video links (#30, #29, #63, #39), the 5 photos in #39 (2560x1440), the screenshot in #116 and #30, shadPS4 PR comments (#11).
- No attachments exist for #95, #99, #114. #29 has no file attachment; its log is an inline paste in the comment.
- Owner comments on #30, #29, #63, #39, #114 and #98 asked for logs, timestamps and videos; none of the repeat-run requests in those comments are answered by the attachments above except #98 (zip 20261111) and #116 (zip 20261111).
