// Shared driver for the J5 workloads. compare.sh puts this file after
// the workload in one concatenated script, so the workload's setup() and step() and the name
// BENCH_NAME (declared by compare.sh before everything else) are in scope.
//
// Protocol: setup() runs once, untimed. The checksum is the result of the first step() after
// setup, which does not depend on timing. Then step() runs for 0.5 s of warm-up, which also sizes
// a batch to about 20 ms. Then 15 batches of step() are timed with performance.now(). The line
// printed is: name median_ms_per_step min_ms checksum. The median and min are per step.
// With BENCH_MEM set (the peak RSS runs), 50 steps run instead and nothing is printed.
// With BENCH_LIVE set (the live-memory runs, J5n1), the same 50 steps run, the last result is kept
// in the global BENCH_RESULT, and one full collection runs: std.gc() under qjs --std, or the host's
// gc() in Ember. The engines then report the bytes that are still live (see compare.sh).
(function () {
  var WARM_MS = 500;
  var BATCH_MS = 20;
  var BATCHES = 15;

  setup();
  if (typeof BENCH_MEM !== 'undefined' && BENCH_MEM) {
    // Memory mode (compare.sh, peak RSS runs): a fixed number of steps, so the heap peak does not
    // depend on how many steps the timed protocol would fit in.
    for (var m = 0; m < 50; m++) {
      step();
    }
    return;
  }
  if (typeof BENCH_LIVE !== 'undefined' && BENCH_LIVE) {
    var last = 0;
    for (var q = 0; q < 50; q++) {
      last = step();
    }
    globalThis.BENCH_RESULT = last;
    var collect = typeof std !== 'undefined' ? std.gc : gc;
    collect();
    return;
  }
  var checksum = step();

  var t0 = performance.now();
  var n = 0;
  var now;
  do {
    step();
    n++;
    now = performance.now();
  } while (now - t0 < WARM_MS);
  var per = Math.max((now - t0) / n, 1e-6);
  var batch = Math.max(1, Math.floor(BATCH_MS / per));

  var times = [];
  for (var b = 0; b < BATCHES; b++) {
    var s = performance.now();
    for (var i = 0; i < batch; i++) {
      step();
    }
    times.push((performance.now() - s) / batch);
  }
  times.sort(function (x, y) {
    return x - y;
  });
  print(
    BENCH_NAME + ' ' + times[BATCHES >> 1].toFixed(6) + ' ' + times[0].toFixed(6) + ' ' + String(checksum)
  );
})();
