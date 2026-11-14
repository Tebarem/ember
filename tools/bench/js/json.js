// json: JSON.parse and JSON.stringify of a generated API payload of about 200 KB (1 000 records),
// plus a parse with a reviver that visits every value. The step returns the output length plus the
// number of numbers the reviver saw.
var text;

function setup() {
  var items = [];
  for (var i = 0; i < 1000; i++) {
    items.push({
      id: i,
      name: 'user ' + i,
      email: 'user' + i + '@example.test',
      active: i % 2 === 0,
      score: i * 1.5,
      tags: ['alpha', 'beta', 'gamma' + (i % 7)],
      address: { city: 'City ' + (i % 50), zip: String(10000 + i), geo: [i / 3, i / 7] }
    });
  }
  text = JSON.stringify({ page: 1, total: items.length, items: items });
}

function step() {
  var obj = JSON.parse(text);
  var out = JSON.stringify(obj);
  var numbers = 0;
  JSON.parse(text, function (key, value) {
    if (typeof value === 'number') {
      numbers++;
    }
    return value;
  });
  return out.length + numbers;
}
