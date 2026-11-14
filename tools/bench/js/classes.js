// classes: class hierarchies three deep with super calls, getters and setters, private fields and
// instanceof. The step builds and uses 5 000 instances of each level.
class Shape {
  #id;
  constructor(id) {
    this.#id = id;
    this.kind = 'shape';
  }
  get id() {
    return this.#id;
  }
  area() {
    return 0;
  }
  describe() {
    return this.kind + ':' + this.area();
  }
}

class Rect extends Shape {
  #scale = 1;
  constructor(id, w, h) {
    super(id);
    this._w = w;
    this._h = h;
    this.kind = 'rect';
  }
  get w() {
    return this._w;
  }
  set w(v) {
    this._w = v;
  }
  area() {
    return this._w * this._h * this.#scale;
  }
}

class Square extends Rect {
  constructor(id, s) {
    super(id, s, s);
    this.kind = 'square';
  }
  area() {
    return super.area() + 0;
  }
  describe() {
    return super.describe().length;
  }
}

function setup() {}

function step() {
  var acc = 0;
  for (var i = 0; i < 5000; i++) {
    var s = new Square(i, i % 10);
    acc = (acc + s.area() + s.id) | 0;
    if (s instanceof Rect) {
      acc = (acc + 1) | 0;
    }
    acc = (acc + s.describe()) | 0;
    s.w = 3;
    acc = (acc + s.area()) | 0;
    var r = new Rect(i, 2, i % 5);
    acc = (acc + r.area() + (r instanceof Square ? 100 : 0)) | 0;
    var b = new Shape(i);
    acc = (acc + b.describe().length) | 0;
  }
  return acc;
}
