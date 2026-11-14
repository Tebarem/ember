// regexp: a tokenizer loop with a sticky (y) regex, global replace with a function, and test on
// short strings. The step returns a checksum of the token counts and the replaced lengths.
var TOKEN = /\s*(?:(\d+\.?\d*)|([A-Za-z_]\w*)|(\S))/y;
var source;
var words;
var block;

function tokenize(s) {
  TOKEN.lastIndex = 0;
  var n = 0;
  var m;
  while (TOKEN.lastIndex < s.length && (m = TOKEN.exec(s)) !== null) {
    n++;
  }
  return n;
}

function setup() {
  source = 'let x1 = 42.5 + foo(bar, 7) * baz - 3; return x1 / 2;';
  words = [];
  for (var i = 0; i < 1000; i++) {
    words.push(i % 3 === 0 ? 'word' + i : 'Word-' + i);
  }
  block = '';
  for (var j = 0; j < 20; j++) {
    block += 'id ' + j + ' size ' + j * 37 + ' ok; ';
  }
}

function step() {
  var sum = 0;
  for (var i = 0; i < 200; i++) {
    sum += tokenize(source);
  }
  for (var k = 0; k < 20; k++) {
    sum += block.replace(/\d+/g, function (m) {
      return '<' + m.length + '>';
    }).length;
  }
  var lower = /^[a-z]+\d*$/;
  for (var w = 0; w < words.length; w++) {
    if (lower.test(words[w])) {
      sum++;
    }
  }
  return sum;
}
