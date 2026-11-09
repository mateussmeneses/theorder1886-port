# Issue reply drafts (2026-11-09) — not posted

Drafts for the maintainer to review and post. Each one asks only for evidence that is missing,
states what was verified locally, and claims nothing that was not reproduced. Language follows
the reporter's.

---

## #113 — Visible-draw pipeline stalls repeat after restart

Thanks for the precise report; the hash families you listed made this reproducible.

I ran the prologue route five times in a row, each session starting from the previous session's
cache (closing and reopening the game), with `THEORDER_BINARY_MISS_LOG=1`
(`docs/PIPELINE_SESSION_REBUILDS.md`):

| Session | New pipelines | Of those, saved by an earlier session | Stalled draws |
|---|---|---|---|
| 1 (empty cache) | 442 | — | 200 |
| 2 | 98 | 0 | 75 |
| 3 | 56 | 0 | 59 |
| 4 | 50 | 0 | 46 |
| 5 | 40 | 0 | 38 |

So saved pipelines are reused: none was compiled twice. The stalls after a restart are pipelines
that did not exist before. Your family `0000096b66ad3540` + `00000ad44642d3xx` is the material
shader `0xad441fb1`: its pixel-shader hashes are permutation numbers (your `…d3f1` is permutation
140). Each session adds permutations because one of its ~32 texture slots holds a different kind
of texture than before (2D vs 3D, sRGB vs linear, integer format), which changes the compiled
shader. On a fixed route the count drops each session, but slowly.

Next steps on my side: reduce permutations that compile to identical code, and look at whether
slots the shader does not sample can stop creating variants. Skipping the draw (your request 3)
would make objects disappear for a frame, which the port avoids by design.

If you can, a third log of the same route after one more restart would show whether your count
also drops (look for `Pipeline binary cache: … compiled` near the end).

---

## #116 — Black screen (RX 6500 XT, v0.6.1 works)

Thanks. The bug-report zip arrived without the game log ("No log output available"): the
launcher only exported its own log window, which is empty after the launcher is reopened. That is
fixed for the next build (the report now includes `user/last_run.log` and the port version).

Until then, please attach `last_run.log` from your saves folder (`user\last_run.log` next to the
game, or the folder set as "Saves folder") right after a black-screen run. This looks like the
same family as #47 (RX 6500 XT black screen); the log decides whether it is the same cause.

---

## #115 — Erros gráficos (RX 580, v0.8.0)

Obrigado pelas capturas. Elas mostram molduras e bordas da armadura brancas e superexpostas, um
padrão parecido com o antigo relato de superfícies prateadas brilhando em AMD. O log anexado
contém só o fim da execução, sem o início onde ficam GPU, driver e formatos suportados.

Para separar a causa, por favor:
1. Anexe o `last_run.log` completo (pasta `user` ao lado do jogo) de uma execução que mostre o
   problema.
2. Diga se a imagem muda com o upscaler desligado (FSR 3.1 → Off), mantendo a mesma cena.

O desempenho (17–21 FPS no log) é um assunto separado; a RX 580 fica abaixo do mínimo que testo
hoje, e não dá para tratar os dois sintomas como uma causa só.

---

## #98 — 0.7 series black screen in gameplay/FMV, 0.6.1 works (RTX 5090)

Thanks for the log. It shows the game running (frames presented, pipelines compiled), so the
black screen is in what is drawn, not a stopped renderer. To narrow it down:
1. Does the black screen happen with every upscaler off and MSAA off in the launcher?
2. Does pressing Insert (port menu) draw over the black image?
3. Please attach the bug report from the current build's "Export Bug Report" after a black-screen
   run; it now carries the full log.

---

## #101 — Stuck on the first mash QTE above 30 FPS

Thanks, and thanks to @theygotti for the frame-rate observation. Button-mash QTEs are driven by
the game's `ButtonMashNode`, which very likely assumes the original frame rate; with the 60 FPS
patch the meter may drain faster than it fills. Workaround until it is fixed: switch the frame
rate patch off (30 FPS) for that QTE, then back on.

I am looking at the mash node in the executable; I cannot reproduce that QTE with scripted input
yet, so there is no fix to test.

---

## #65 — Does not start on 2nd/3rd generation Intel CPUs

(Fill in with the forced-rewrite A/B result from `docs/plans/2026-11-09-autonomous-backlog.md`
before posting.)

Please attach `last_run.log` from a failed start and the exact CPU model. The port checks the
CPU at start: Sandy/Ivy Bridge lack some instructions the PS4 code uses (MOVBE, F16C, BMI1,
LZCNT), and the port rewrites those sites; the first lines of the log say whether that happened
(`CPU: missing …`, `ISA: CPU lacks …`) or why it stopped (`STOP: …`).

---

## #114 — Very low FPS and stutter (RTX 3050 6 GB)

Thanks. Without a log there is no way to tell compile stalls from GPU load. Please use "Export
Bug Report (.zip)" in the launcher after a run that stutters and attach it here, and say which
upscaler and output resolution you use. The first minutes after a fresh install compile shaders
and stutter more; a second run over the same scene should be smoother (see #113).

---

## #100 — No upscaler works (RTX 3070 Ti, 2560x1080)

Thanks. Please attach the bug report zip (or `user/last_run.log`) from a run with FSR 3.1
selected. The log line starting with `Upscaler:` says whether it was active, which provider ran,
and why one was refused; without it I cannot tell a missing provider from a 2560x1080 output
problem.
