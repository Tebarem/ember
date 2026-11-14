// closures: counter factories, callbacks passed to functions, and immediately invoked closures,
// each in a loop. The step returns a checksum of the values.
function makeCounter(start) {
  var c = start;
  return function () {
    c++;
    return c;
  };
}

function apply(fn, x) {
  return fn(x);
}

function setup() {}

function step() {
  var total = 0;
  for (var i = 0; i < 20000; i++) {
    var f = makeCounter(i);
    f();
    total = (total + f()) | 0;
  }
  for (var j = 0; j < 20000; j++) {
    total = (total + apply(function (v) {
      return v * 2 + j;
    }, j)) | 0;
  }
  for (var k = 0; k < 20000; k++) {
    total = (total + (function (a) {
      return function (b) {
        return a + b;
      };
    })(k)(1)) | 0;
  }
  return total;
}
