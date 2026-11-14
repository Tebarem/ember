// A DOM-less stand-in for the part of document that Preact's renderer uses (createElementNS,
// createTextNode, element and text nodes with childNodes, attributes and data). It is evaluated
// before the framework build by tests/unit/js/test_frameworks.c.
function StubNode(nodeType) {
  this.nodeType = nodeType;
  this.childNodes = [];
  this.parentNode = null;
}
StubNode.prototype.appendChild = function (child) {
  return this.insertBefore(child, null);
};
StubNode.prototype.insertBefore = function (child, ref) {
  if (child.parentNode) {
    child.parentNode.removeChild(child);
  }
  var i = ref ? this.childNodes.indexOf(ref) : this.childNodes.length;
  if (i < 0) {
    throw new Error('insertBefore: reference is not a child');
  }
  this.childNodes.splice(i, 0, child);
  child.parentNode = this;
  return child;
};
StubNode.prototype.removeChild = function (child) {
  var i = this.childNodes.indexOf(child);
  if (i < 0) {
    throw new Error('removeChild: not a child');
  }
  this.childNodes.splice(i, 1);
  child.parentNode = null;
  return child;
};
// ChildNode.remove() (Preact 11 removes nodes with it; Preact 10 uses removeChild).
StubNode.prototype.remove = function () {
  if (this.parentNode) {
    this.parentNode.removeChild(this);
  }
};
Object.defineProperty(StubNode.prototype, 'firstChild', {
  get: function () {
    return this.childNodes[0] || null;
  }
});
Object.defineProperty(StubNode.prototype, 'nextSibling', {
  get: function () {
    var p = this.parentNode;
    if (!p) {
      return null;
    }
    return p.childNodes[p.childNodes.indexOf(this) + 1] || null;
  }
});

function StubElement(ns, name) {
  StubNode.call(this, 1);
  this.namespaceURI = ns;
  this.localName = name;
  this.attributes = {};
  this.style = {};
}
StubElement.prototype = Object.create(StubNode.prototype);
StubElement.prototype.constructor = StubElement;
StubElement.prototype.setAttribute = function (name, value) {
  this.attributes[name] = String(value);
};
StubElement.prototype.getAttribute = function (name) {
  return Object.prototype.hasOwnProperty.call(this.attributes, name) ? this.attributes[name] : null;
};
StubElement.prototype.removeAttribute = function (name) {
  delete this.attributes[name];
};
StubElement.prototype.addEventListener = function () {};
StubElement.prototype.removeEventListener = function () {};

function StubText(data) {
  StubNode.call(this, 3);
  this.data = String(data);
}
StubText.prototype = Object.create(StubNode.prototype);
StubText.prototype.constructor = StubText;

var document = {
  documentElement: new StubElement('http://www.w3.org/1999/xhtml', 'html'),
  createElement: function (name) {
    return new StubElement(null, name);
  },
  createElementNS: function (ns, name) {
    return new StubElement(ns, name);
  },
  createTextNode: function (text) {
    return new StubText(text);
  }
};

// Stub job queue (a stand-in for Promise). Preact's scheduler refers to setTimeout as the fallback for
// Promise, so a job is queued here and never run: the smoke scripts are synchronous.
var jobs = [];
function setTimeout(fn) {
  jobs.push(fn);
  return jobs.length;
}
function clearTimeout() {}
// Preact 11 defers renders and effects to queueMicrotask when no scheduler option is set. The
// engine has no host queueMicrotask global, so the stub provides one on Promise jobs.
function queueMicrotask(fn) {
  Promise.resolve().then(fn);
}
// Runs the queued timer callbacks until none are left (effects are flushed by a timer callback
// that queues another one).
function flushTimers() {
  while (jobs.length > 0) {
    jobs.shift()();
  }
}
