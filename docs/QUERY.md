# Query and regular-expression semantics

Queries are case-sensitive. `AND`, `OR`, `NOT` are uppercase operators. Precedence
is `NOT`, then `AND`, then `OR`; parentheses override it. Adjacent operands imply
`AND`. Boolean operations combine **files**, so `alpha AND beta` may find the
two identifiers on different lines of the same file. To find adjacent exact
text, use a double-quoted phrase.

```text
RenderFrame
RenderFrame AND NOT deprecated
(RenderFrame OR UpdateFrame) AND renderer
"std::vector<int>"
"error code" OR /throw\s+[A-Za-z_][A-Za-z_0-9]*/
/path\/component/
```

Bare terms are whole tokens. Tokens contain ASCII letters, digits, underscore,
and any non-ASCII Unicode scalar. The latter is intentionally a simple,
documented rule without an external Unicode-category database: non-ASCII
punctuation also joins tokens. No case folding or Unicode normalization occurs.
Consequently canonically equivalent Unicode spellings remain distinct. A quoted
phrase is an exact substring within one line, including its punctuation and
spacing; it does not require token boundaries. Empty phrases are rejected.
Phrases support `\"`, `\\`, and `\t`; CR/LF in a phrase is rejected because the
engine matches one line at a time.

An empty file has zero logical lines: it can satisfy `NOT missing`, but has no
line on which `//` or `/^$/` could match. A file containing a newline has an empty
logical line, which can match these expressions. Phrase frequency counts
nonoverlapping occurrences; regex frequency counts matching lines.

## Regular expressions

Enclose a regex in slashes. Escape a slash inside it as `\/`. The engine compiles
its own Thompson NFA and never uses `std::regex` for searching. Supported syntax:

| Syntax | Meaning |
| --- | --- |
| `abc`, `привет`, `😀` | Literal Unicode scalars |
| `.` | One Unicode scalar other than CR/LF |
| `[a-z]`, `[^0-9]`, `[а-я]` | Character classes, ranges, negation |
| `a\|b` | Literal `a|b` |
| `a|b`, `(ab|cd)` | Alternation and grouping |
| `a*`, `a+`, `a?` | Zero or more, one or more, optional |
| `a{3}`, `a{2,5}`, `a{2,}` | Counted repetitions |
| `^`, `$` | Beginning/end of the supplied line |
| `\d`, `\D` | ASCII digit / its complement |
| `\w`, `\W` | Token character as defined above / its complement |
| `\s`, `\S` | ASCII space, tab, CR, LF, form feed, vertical tab / complement |
| `\b`, `\B` | Word boundary / non-boundary |
| `\n`, `\r`, `\t`, `\v`, `\f` | Control-character literals |
| `\x41`, `\u0410`, `\U0001F600` | Hexadecimal Unicode scalar escapes |

Within a character class, `\b` is backspace. Category escapes may be unioned
with ranges, e.g. `[a-f\d]`. A category cannot be a range endpoint. Escape a
closing bracket as `\]`. A hyphen at the first/last class position is literal.
An empty alternative, empty group, or `//` is a valid zero-width match. Anchors
refer to the entire line even when `Regex::find` starts at a nonzero offset.

The result is the **leftmost match, then the earliest ending match** at that
position. Thus `a+` on `aaaa` returns the first `a`; `a+$` returns all four.
This is a deterministic existence/evidence matcher, without capture extraction
or Perl-style greedy/lazy priority. The engine counts a regex's frequency as the
number of matching lines, taking its first match on each line. Backreferences,
lookaround, inline flags, special groups, lazy/possessive quantifiers, and
unrecognized escapes are rejected with an error, never silently reinterpreted.

Regex traversal is linear in input length for a fixed compiled expression.
Unicode is decoded by scalar, while returned offsets and displayed columns are
UTF-8 **byte** offsets (public snippets use 1-based columns). An execution budget
shared across the search bounds work; exhausting it is an explicit query error,
not an incomplete result. The matcher maintains at most one earliest origin
per NFA state and handles epsilon cycles without recursion or backtracking.

## Explicit bounds

- Query and regex source: at most 8192 UTF-8 bytes.
- Query leaves: at most 128.
- Parenthesis/NOT nesting and regex group nesting: at most 64.
- Regex compilation: at most 4096 AST nodes and 4096 NFA states.
- All regexes in one query together: at most 32768 NFA states, bounding aggregate
  program memory even when many compact counted repetitions expand substantially.
- Each counted repetition bound: at most 1000; expansion still obeys the state cap.
- `Regex::find` requires a UTF-8 character-boundary start offset and valid UTF-8 input.

These bounds are checked before/while compiling; excessive expressions fail
with a descriptive exception. Regex objects and parsed queries are immutable
after construction and can be shared between threads.
