# Fast makefun-site


A faster drop-in replacement for makefun-site 1.0.0, built for make.fun's
"Build a static site faster, 22 November 2026" bounty ($100 in USDC):
https://make.fun/bounty/site-generator-speed-01-run-20261122


[![competition](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/hasannulhuda8/site-generator-speed-01/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/hasannulhuda8/site-generator-speed-01/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)


`solve.js` exports `run(input)`, which returns exactly the output tree
makefun-site 1.0.0's `build(input)` returns — every page's `index.html`, the
index and tag pages, `build-report.txt` and each static file, byte for
byte — while `main.ts` is the harness driver that reads
`/input/cases.json` and writes `/output/answers.json`.


Where the speed comes from:


- HTML escaping in bulk runs with a no-special-characters fast path, never
  character by character
- front matter parsed by scanning line offsets, so a body stays one slice
- Markdown lexed and rendered in a single pass, with manual line
  classification instead of a regex battery per line
- word counts scanned by hand; slugs and long dates memoised per case
- templates rendered into a shared chunk buffer through a scope stack


## Run


    MAKEFUN_INPUT_DIR=<dir with cases.json> MAKEFUN_OUTPUT_DIR=<out dir> node main.ts


Node 22 or later, no dependencies.

