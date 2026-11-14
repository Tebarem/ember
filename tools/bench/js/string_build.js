// string_build: builds a 1 MB string with += (from 4 KB chunks, then an end marker), with template
// literals joined by join. Then split, replace and indexOf run on the 1 MB string, and indexOf on
// the joined string. The unit has ';' separators so split does real work. The step returns a
// checksum of the results.
var chunk;

function setup() {
  var unit = 'abcdefghijklmnopqrstuvwxyz0123456789;';
  chunk = '';
  while (chunk.length < 4096) {
    chunk += unit;
  }
  chunk = chunk.slice(0, 4096);
}

function step() {
  var big = '';
  for (var i = 0; i < 256; i++) {
    big += chunk;
  }
  big += 'END-MARK';
  var parts = [];
  for (var j = 0; j < 20000; j++) {
    parts.push(`item ${j};`);
  }
  var joined = parts.join('');
  var pieces = big.split(';');
  var at = big.indexOf('END-MARK');
  var jat = joined.indexOf('item 19999;');
  var replaced = big.replace('END-MARK', 'end-mark');
  return big.length + pieces.length * 3 + at + jat + replaced.length + joined.length;
}
