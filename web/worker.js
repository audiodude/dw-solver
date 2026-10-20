// A fresh worker and Emscripten module are created for every search.
let started = false;

self.onmessage = async ({ data }) => {
  if (started || data?.type !== "start") return;
  started = true;
  let finalResult = null;
  let stage = "loading";
  const diagnostics = [];

  const stderr = (value) => {
    const message = String(value);
    diagnostics.push(message);
    if (diagnostics.length > 12) diagnostics.shift();
    self.postMessage({ type: "stderr", message });
  };

  const readResult = (result, progress) => {
    const statuses = progress ? ["SEARCHING"] : ["OPTIMAL", "BEST KNOWN", "NO SOLUTION"];
    if (!result || !statuses.includes(result.status) || typeof result.answer !== "string" ||
        !Number.isFinite(result.score) || !Number.isFinite(result.upper_bound) ||
        !Number.isFinite(result.nodes) || !Number.isFinite(result.seconds) || !Array.isArray(result.steps) ||
        (result.score < 0 ? result.steps.length !== 0 : result.steps.length !== 6) ||
        result.steps.some((step) => !/^[a-z]{5}$/.test(step.guess) || !/^[.YG]{5}$/.test(step.feedback) ||
          !Number.isFinite(step.remaining_words) || !Number.isFinite(step.unused_letters) || !Number.isFinite(step.score))) {
      throw new Error("The search engine returned an invalid result.");
    }
    return result;
  };

  const stdout = (line) => {
    if (!String(line).trim()) return;
    // Emscripten's print callback receives complete newline-delimited records.
    const record = JSON.parse(line);
    if (record.type === "progress") {
      self.postMessage({ type: "progress", result: readResult(record.result, true) });
    } else {
      finalResult = readResult(record, false);
    }
  };

  try {
    if (!/^[a-z]{5}$/.test(data.answer) || !Array.isArray(data.history) || data.history.length > 6 ||
        data.history.some((word) => !/^[a-z]{5}$/.test(word)) ||
        (data.budget !== "exhaustive" && !(Number(data.budget) > 0 && Number(data.budget) <= 1e9))) {
      throw new Error("Invalid search settings.");
    }
    if (typeof WebAssembly === "undefined") throw new Error("This browser does not support WebAssembly.");
    const { default: createSolver } = await import("./solver.mjs");
    const engine = await createSolver({
      noInitialRun: true,
      print: stdout,
      printErr: stderr,
      locateFile: (name) => new URL(name, import.meta.url).href,
    });
    const args = [data.answer, "--json"];
    if (data.history.length) args.push("--history", data.history.join(","));
    if (data.budget !== "exhaustive") args.push("--time-limit", data.budget);
    stage = "searching";
    self.postMessage({ type: "ready" });
    let exitCode = 0;
    try {
      const returned = engine.callMain(args);
      if (typeof returned === "number") exitCode = returned;
    } catch (error) {
      // Emscripten may throw ExitStatus even on an ordinary main() return.
      if (error?.name === "ExitStatus" && Number.isInteger(error.status)) exitCode = error.status;
      else throw error;
    }
    // With a runtime kept alive, callMain can return undefined. The final record
    // still identifies the normal CLI outcome unambiguously.
    if (finalResult && exitCode === 0) {
      if (finalResult.status === "BEST KNOWN") exitCode = 2;
      if (finalResult.status === "NO SOLUTION") exitCode = 3;
    }
    if (![0, 2, 3].includes(exitCode)) {
      throw new Error(diagnostics.join("\n") || `Search engine exited with code ${exitCode}.`);
    }
    if (!finalResult) throw new Error(diagnostics.join("\n") || "The search engine finished without a result.");
    const expectedStatus = { 0: "OPTIMAL", 2: "BEST KNOWN", 3: "NO SOLUTION" }[exitCode];
    if (finalResult.status !== expectedStatus) throw new Error("The search engine returned an inconsistent exit status.");
    self.postMessage({ type: "complete", result: finalResult, exitCode });
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    const message = stage === "loading"
      ? `Could not load the local search engine. Serve this page over HTTP and make sure solver.mjs and solver.wasm are built and available beside it. ${detail}`
      : (diagnostics.join("\n") || detail);
    self.postMessage({ type: "error", message });
  }
};
