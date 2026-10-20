# Don't Wordle solver

Finds the highest-scoring six-guess sequence for [Don't Wordle](https://dontwordle.com) when the answer is known. The score is **remaining valid words × unused distinct letters** after the final guess. Every guess has to follow hard-mode rules (consistent with all feedback so far) and must not be the answer.

The engine is C++. It runs as a CLI and as WebAssembly in the browser. Live site: https://dw.audiodude.xyz

## How it searches

1. A **limited discrepancy pass** comes first. It varies the early guesses before the later ones, so strong sequences turn up quickly. It gets 20% of `--time-limit`, or 10s when there is no limit.
2. An **exhaustive branch-and-bound search** follows. It skips any branch whose best possible score can't beat the best one found so far. If it completes, the result is `OPTIMAL`, meaning proven. If time runs out or you stop it, the result is `BEST KNOWN` and isn't proven.

## CLI

```sh
make                       # builds ./dont-wordle
./dont-wordle crane --time-limit 60
./dont-wordle crane --history gyppy,vivid --json
```

| Option | Meaning |
|---|---|
| `--time-limit SECONDS` | Stop and return the best known sequence (exit status 2). Without it, the search runs until optimality is proven. |
| `--history WORD,WORD` | Fixed opening guesses. They count toward `--turns`. |
| `--turns N` | Total guesses, 1–6 (default 6). |
| `--dictionary FILE` | One five-letter word per line. Defaults to `data/words.txt`, the site's word list. |
| `--json` | Machine-readable output. |

Ctrl+C stops the search safely and prints the best sequence found so far. Exit codes: `0` optimal, `2` best known, `3` no solution, `1` error.

## Web app

Install [Emscripten](https://emscripten.org) once (for example `git clone https://github.com/emscripten-core/emsdk ~/emsdk && ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest`). Then:

```sh
source ~/emsdk/emsdk_env.sh
make serve                 # builds web/solver.{mjs,wasm}, serves http://127.0.0.1:5555
```

`web/solver.mjs` and `web/solver.wasm` are build outputs and are gitignored.

## Tests

```sh
make test
```

## Deploy

The site is hosted on the Cloudflare Pages project `dw-solver` and served at `dw.audiodude.xyz`.

```sh
source ~/emsdk/emsdk_env.sh
. ~/.secrets               # provides CLOUDFLARE_API_TOKEN / CLOUDFLARE_ACCOUNT_ID
make deploy
```

`make deploy` builds the WASM files, copies `web/` to a temporary directory, adds a content hash (`?v=…`) to the asset URLs in `index.html`, and deploys that copy. `app.js` passes the hash on to `worker.js`, and `worker.js` passes it on to `solver.mjs`.

Always deploy with `make deploy`, not `wrangler pages deploy web`. The `audiodude.xyz` zone tells browsers to cache files for 4 hours. Without the version hash, returning visitors can end up mixing a new `app.js` with a stale `worker.js`. Deploying the copy also guarantees that nothing outside `web/` gets uploaded.
