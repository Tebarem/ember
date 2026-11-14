// props: object literals of 4 to 12 fields, field reads and writes in loops, 3-deep prototype
// method calls, and a megamorphic site that reads the same fields of 16 shapes.
//
// Shape k is the key sequence a, b, c followed by `extra` keys taken from names[] in order from
// names[k % 9], wrapping. extra = 1 + (k % 5), plus 4 for k >= 8, so 4 to 12 fields in all. The
// pairs (extra, start) = (extra(k), k % 9) are distinct for k in 0..15, so the 16 objects have 16
// different shapes. setup() checks that the key sequences are all different and throws otherwise.
var names = ['d', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l'];
var objs;
var leaf;

function Base() {}
Base.prototype.get = function () {
  return this.a;
};

function Mid() {}
Mid.prototype = Object.create(Base.prototype);
Mid.prototype.constructor = Mid;
Mid.prototype.twice = function () {
  return this.get() * 2;
};

function Leaf() {
  this.a = 5;
  this.b = 1;
  this.c = 0;
}
Leaf.prototype = Object.create(Mid.prototype);
Leaf.prototype.constructor = Leaf;
Leaf.prototype.thrice = function () {
  return this.twice() + this.get();
};

function makeObj(k) {
  var o = { a: k, b: k * 2, c: 0 };
  var extra = 1 + (k % 5) + (k >= 8 ? 4 : 0);
  var start = k % 9;
  for (var j = 0; j < extra; j++) {
    o[names[(start + j) % 9]] = j + k;
  }
  return o;
}

function setup() {
  objs = [];
  var seen = {};
  var distinct = 0;
  for (var k = 0; k < 16; k++) {
    var o = makeObj(k);
    var key = Object.keys(o).join();
    if (seen[key] !== true) {
      seen[key] = true;
      distinct++;
    }
    objs.push(o);
  }
  if (distinct !== 16) {
    throw new Error('props: expected 16 distinct shapes, got ' + distinct);
  }
  leaf = new Leaf();
}

function step() {
  var sum = 0;
  for (var r = 0; r < 200; r++) {
    for (var k = 0; k < 16; k++) {
      var o = objs[k];
      sum = (sum + o.a + o.b) | 0;
      o.c = (o.c + 1) | 0;
    }
    sum = (sum + leaf.thrice()) | 0;
  }
  return sum;
}
