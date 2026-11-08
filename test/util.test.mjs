import { test } from "node:test";
import assert from "node:assert/strict";
import { asciiClean, calcNick, ircNick, parseIrc, stripFormatting, unwrapRelay } from "../docs/util.js";

test("irc nicks fit and stay exact", () => {
  assert.equal(ircNick("akhil"), "akhil");
  assert.equal(ircNick("akhilesh9"), "akhilesh9");
  assert.equal(ircNick("akhilesh_long"), "akhilesh_");
  assert.equal(ircNick("akhilesh9", 1), "akhilesh1");
  assert.equal(ircNick("9lives"), "lives");
  assert.equal(ircNick("a b!c"), "abc");
  assert.equal(ircNick(""), "ceuser");
});

test("old calculator names are reused, cut to 8", () => {
  assert.equal(calcNick("xsamuraicalc"), "xsamurai");
  assert.equal(calcNick("akhil"), "akhil");
  assert.equal(calcNick("9lives cat"), "livescat");
  assert.equal(calcNick("a-b_c[d]e"), "a-b_c[d]");
});

test("relay and formatting", () => {
  assert.deepEqual(unwrapRelay("saxjax", "[Kerm] hi"), ["Kerm", "hi"]);
  assert.deepEqual(unwrapRelay("saxjax", "(SAX) <Kerm> hi"), ["Kerm", "hi"]);
  assert.deepEqual(unwrapRelay("bob", "[x] y"), ["bob", "[x] y"]);
  assert.equal(stripFormatting("\x0304,01red\x03 \x02bold\x02"), "red bold");
});

test("irc line parsing and ascii", () => {
  assert.deepEqual(parseIrc(":kerm!k@h PRIVMSG #cemetech :hi there"), {
    prefix: "kerm!k@h", sender: "kerm", cmd: "PRIVMSG", rest: "#cemetech :hi there",
  });
  assert.equal(parseIrc("PING :abc").cmd, "PING");
  assert.equal(asciiClean("héllo\tx", 10), "h?llo x");
  assert.equal(asciiClean("abcdef", 3), "abc");
});
