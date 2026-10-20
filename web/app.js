const byId = (id) => document.getElementById(id);
const form = byId("search-form");
const fields = byId("search-fields");
const answerInput = byId("answer");
const historyInput = byId("history");
const budgetInput = byId("time-limit");
const customTimeInput = byId("custom-time");
budgetInput.addEventListener("change", () => {
  customTimeInput.hidden = budgetInput.value !== "custom";
  if (!customTimeInput.hidden) customTimeInput.focus();
});
const startButton = byId("start-button");
const stopButton = byId("stop-button");
const errorBox = byId("form-error");
const statusText = byId("search-status");
const badge = byId("status-badge");
const sequenceTable = document.querySelector(".sequence-table");
const tableBody = byId("sequence-body");
const number = new Intl.NumberFormat("en-US");
const feedbackNames = { G: "correct place", Y: "wrong place", ".": "not present or excess copy" };
const feedbackClasses = { G: "green", Y: "yellow", ".": "gray" };
let activeRun = null;

// Only ASCII capitals are normalized; non-ASCII lookalikes remain invalid.
const normalize = (value) => value.trim().replace(/[A-Z]/g, (letter) => letter.toLowerCase());
const setText = (id, value) => { byId(id).textContent = value; };
const secondsText = (seconds) => `${seconds.toFixed(1)} s`;

function setStatus(label, state, description) {
  badge.textContent = label;
  badge.dataset.state = state;
  if (statusText.textContent !== description) statusText.textContent = description;
}

function renderRows(result = null, prefixLength = 0) {
  const rows = document.createDocumentFragment();
  for (let index = 0; index < 6; index++) {
    const step = result?.steps[index];
    const row = document.createElement("tr");
    if (step && index === 5) row.className = "complete-final";
    const turn = document.createElement("td");
    turn.textContent = String(index + 1).padStart(2, "0");
    const guess = document.createElement("td");
    if (step) {
      const letters = document.createElement("span");
      letters.className = "guess-feedback";
      letters.setAttribute("role", "img");
      letters.setAttribute("aria-label", `${step.guess.toUpperCase()}. ${[...step.guess].map((letter, position) => `${letter.toUpperCase()}: ${feedbackNames[step.feedback[position]]}`).join("; ")}.`);
      for (let position = 0; position < 5; position++) {
        const letter = document.createElement("span");
        letter.className = `feedback-letter ${feedbackClasses[step.feedback[position]]}`;
        letter.textContent = step.guess[position].toUpperCase();
        letter.setAttribute("aria-hidden", "true");
        letter.title = `${step.guess[position].toUpperCase()}: ${feedbackNames[step.feedback[position]]}`;
        letters.append(letter);
      }
      guess.append(letters);
      if (index < prefixLength) {
        const fixed = document.createElement("span");
        fixed.className = "fixed-marker";
        fixed.textContent = "Fixed prefix";
        guess.append(fixed);
      }
    } else {
      guess.textContent = "—";
      guess.className = "empty-cell";
      guess.setAttribute("aria-label", "No result yet");
    }
    row.append(turn, guess);
    for (const key of ["remaining_words", "unused_letters", "score"]) {
      const cell = document.createElement("td");
      cell.textContent = step ? number.format(step[key]) : "—";
      if (!step) cell.className = "empty-cell";
      row.append(cell);
    }
    rows.append(row);
  }
  tableBody.replaceChildren(rows);
}

function renderMetrics(result) {
  setText("nodes", number.format(result.nodes));
  setText("elapsed", secondsText(result.seconds));
  setText("upper-bound", result.upper_bound >= 0 ? number.format(result.upper_bound) : "—");
}

function renderBest(run, result) {
  renderMetrics(result);
  if (result.score < 0 || result.steps.length !== 6) return;
  // Native progress can report new search statistics without changing the path.
  const previous = run.latest;
  run.latest = result;
  if (previous && previous.score === result.score &&
      previous.steps.every((step, index) => step.guess === result.steps[index].guess)) return;
  const finalStep = result.steps[5];
  setText("final-score", number.format(result.score));
  setText("remaining-words", number.format(finalStep.remaining_words));
  setText("unused-letters", number.format(finalStep.unused_letters));
  renderRows(result, run.history.length);
}

function releaseRun(run) {
  if (activeRun !== run) return;
  activeRun = null;
  clearInterval(run.timer);
  run.worker?.terminate();
  fields.disabled = false;
  stopButton.disabled = true;
  startButton.replaceChildren(document.createTextNode("Find sequence "));
  const arrow = document.createElement("span");
  arrow.textContent = "↗";
  arrow.setAttribute("aria-hidden", "true");
  startButton.append(arrow);
  sequenceTable.setAttribute("aria-busy", "false");
}

function failRun(run, message) {
  if (activeRun !== run) return;
  releaseRun(run);
  errorBox.textContent = message;
  errorBox.hidden = false;
  setStatus("ERROR", "error", run.latest
    ? "The search failed. The last complete sequence is preserved below; optimality is not proven."
    : "The search could not finish. No complete six-guess sequence is available. Check the message beside the search settings.");
}

function finishRun(run, result) {
  renderBest(run, result);
  releaseRun(run);
  const conditional = run.history.length ? " with this fixed prefix" : "";
  if (result.status === "OPTIMAL") {
    setStatus("OPTIMAL", "optimal", `Proven optimal${conditional}: no legal six-guess sequence has a higher final score. Other sequences may tie.`);
  } else if (result.status === "NO SOLUTION") {
    setStatus("NO SOLUTION", "idle", `The exhaustive search found no legal six-guess sequence${conditional}.`);
  } else {
    setStatus("BEST KNOWN", "unproven", run.latest
      ? `Time budget reached. This is the best complete sequence found${conditional}; optimality is not proven.`
      : `Time budget reached before a complete six-guess sequence was found${conditional}. This does not prove that no solution exists. Try more time or an exhaustive search.`);
  }
}

function validateSettings() {
  answerInput.removeAttribute("aria-invalid");
  historyInput.removeAttribute("aria-invalid");
  customTimeInput.removeAttribute("aria-invalid");
  errorBox.hidden = true;
  errorBox.textContent = "";
  const answer = normalize(answerInput.value);
  const prefix = normalize(historyInput.value);
  answerInput.value = answer;
  const history = prefix ? prefix.split(",").map(normalize) : [];
  historyInput.value = history.join(", ");
  let invalidInput = null;
  let message = "";
  if (!/^[a-z]{5}$/.test(answer)) {
    invalidInput = answerInput;
    message = "Enter a known answer with exactly five ASCII letters (A–Z).";
  } else if (history.length > 6 || history.some((word) => !/^[a-z]{5}$/.test(word))) {
    invalidInput = historyInput;
    message = "Use up to six five-letter words separated by commas, with no empty entries. Use ASCII letters A–Z only.";
  } else if (history.includes(answer)) {
    invalidInput = historyInput;
    message = "The fixed prefix cannot include the known answer: the sequence must avoid guessing it.";
  } else if (new Set(history).size !== history.length) {
    invalidInput = historyInput;
    message = "The fixed prefix cannot repeat a guess. Each earlier guess is ruled out once it is used.";
  } else if (budgetInput.value === "custom" && !(Number(customTimeInput.value) > 0 && Number(customTimeInput.value) <= 1e9)) {
    invalidInput = customTimeInput;
    message = "Enter a custom search budget as a positive number of seconds (at most 1,000,000,000).";
  }
  if (invalidInput) {
    invalidInput.setAttribute("aria-invalid", "true");
    if (invalidInput === historyInput) historyInput.closest("details").open = true;
    errorBox.textContent = message;
    errorBox.hidden = false;
    invalidInput.focus();
    return null;
  }
  const budget = budgetInput.value === "custom" ? String(Number(customTimeInput.value)) : budgetInput.value;
  return { answer, history, budget };
}

form.addEventListener("submit", (event) => {
  event.preventDefault();
  if (activeRun) return;
  const settings = validateSettings();
  if (!settings) return;
  const run = { ...settings, worker: null, latest: null, startedAt: null, timer: null };
  activeRun = run;
  fields.disabled = true;
  stopButton.disabled = false;
  startButton.textContent = "Loading engine…";
  sequenceTable.setAttribute("aria-busy", "true");
  renderRows();
  for (const id of ["final-score", "remaining-words", "unused-letters", "elapsed", "nodes", "upper-bound"]) setText(id, "—");
  setText("result-context", `Avoiding ${run.answer.toUpperCase()} · 6 turns${run.history.length ? ` · Fixed prefix: ${run.history.join(", ").toUpperCase()}` : " · No fixed prefix"}`);
  setStatus("LOADING", "running", "Loading the local WebAssembly engine and embedded dictionary. You can stop at any time.");
  // The identity check discards queued events from terminated or superseded workers.
  try {
    // Carry the deploy version (?v=…) through so stale cached workers are never reused.
    run.worker = new Worker(new URL(`./worker.js${new URL(import.meta.url).search}`, import.meta.url), { type: "module" });
    run.worker.onmessage = ({ data }) => {
      if (activeRun !== run) return;
      switch (data.type) {
        case "ready":
          run.startedAt = performance.now();
          startButton.textContent = "Searching…";
          setStatus("SEARCHING", "running", "Searching for a complete six-guess sequence. Any result shown while searching is unproven.");
          run.timer = setInterval(() => {
            if (activeRun === run) setText("elapsed", secondsText((performance.now() - run.startedAt) / 1000));
          }, 100);
          break;
        case "progress":
          renderBest(run, data.result);
          setStatus("SEARCHING", "running", run.latest
            ? "Best complete sequence so far. The engine is still searching; optimality is not proven."
            : "Searching for a complete six-guess sequence. No complete result has been found yet.");
          break;
        case "complete":
          finishRun(run, data.result);
          break;
        case "error":
          failRun(run, data.message);
          break;
        case "stderr":
          // stderr may contain nonfatal runtime diagnostics. The worker includes
          // relevant diagnostics in its error message if the search fails.
          break;
      }
    };
    run.worker.onerror = (event) => {
      event.preventDefault();
      failRun(run, `The search worker failed to load or run. Serve this page over HTTP with worker.js, solver.mjs, and solver.wasm available. ${event.message || "Your browser may not support module workers or WebAssembly."}`);
    };
    run.worker.onmessageerror = () => failRun(run, "The browser could not read the search worker's result. Please start a new search.");
    run.worker.postMessage({ type: "start", ...settings });
  } catch (error) {
    failRun(run, `Could not start the search worker. Use a browser with module worker and WebAssembly support, and serve this page over HTTP. ${error.message}`);
  }
});

stopButton.addEventListener("click", () => {
  const run = activeRun;
  if (!run) return;
  if (run.startedAt !== null) setText("elapsed", secondsText((performance.now() - run.startedAt) / 1000));
  releaseRun(run);
  setStatus("STOPPED", "unproven", run.latest
    ? "Stopped by you. The last complete sequence is preserved; optimality is not proven. Search statistics reflect the latest update."
    : "Stopped by you before a complete sequence was found. This does not prove that no solution exists.");
  startButton.focus();
});

renderRows();
