# Task 4 — Enums and match
Write a Flint program in `solution.fl` (with `fn main`) that:
- defines `enum Opt { None, Some(i64) }`
- builds `a = Opt.Some(40)`, `b = Opt.None`
- prints the payload of `a` plus 2 (via match), then 99 for `b` (via match)

Expected output is exactly the two lines in expected.txt.
Ideas you'll need: `enum`, `match` (every variant must be covered), `print`.
