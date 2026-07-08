/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/regex_converter.cc
 */
#include "regex_converter.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "support/encoding.h"
#include "support/logging.h"
#include "support/utils.h"

namespace xgrammar {

/*!
 * \brief Convert a regex to EBNF.
 * \details The implementation refers to the regex described in
 * https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Regular_expressions
 */
class RegexConverter {
 public:
  explicit RegexConverter(const std::string& regex) : regex_(regex) {
    if (!regex.empty()) {
      regex_codepoints_ = ParseUTF8(regex_.c_str(), false);
      if (regex_codepoints_[0] == kInvalidUTF8) {
        XGRAMMAR_LOG(FATAL) << "The regex is not a valid UTF-8 string.";
        XGRAMMAR_UNREACHABLE();
      }
    }
    regex_codepoints_.push_back(0);  // Add a null terminator
  }
  std::string Convert();

 private:
  /**
   * \brief Add a segment string to the result EBNF string. It especially adds a space if needed
   * and add_space is true.
   */
  void AddEBNFSegment(const std::string& element);

  [[noreturn]] void RaiseError(const std::string& message);
  void RaiseWarning(const std::string& message);

  std::string HandleCharacterClass();
  std::string HandleRepetitionRange();
  std::string HandleCharEscape();
  std::string HandleEscape();
  std::string HandleEscapeInCharClass();
  /**
   * \brief Handle group modifier. The general format is "(?" + modifier + content + ")". E.g.
   * "(?:abc)" is a non-capturing group.
   */
  void HandleGroupModifier();

  std::string regex_;
  std::vector<TCodepoint> regex_codepoints_;
  TCodepoint* start_;
  TCodepoint* current_;
  TCodepoint* end_;
  std::string result_ebnf_;
  int parenthesis_level_ = 0;
};

void RegexConverter::AddEBNFSegment(const std::string& element) {
  if (!result_ebnf_.empty()) {
    result_ebnf_ += ' ';
  }
  result_ebnf_ += element;
}

void RegexConverter::RaiseError(const std::string& message) {
  XGRAMMAR_LOG(FATAL) << "Regex parsing error at position " << current_ - start_ + 1 << ": "
                      << message;
  XGRAMMAR_UNREACHABLE();
}

void RegexConverter::RaiseWarning(const std::string& message) {
  XGRAMMAR_LOG(WARNING) << "Regex parsing warning at position " << current_ - start_ + 1 << ": "
                        << message;
}

std::string RegexConverter::HandleCharacterClass() {
  std::string char_class = "[";
  ++current_;
  if (*current_ == ']') {
    RaiseError("Empty character class is not allowed in regex.");
  }
  while (*current_ != ']' && current_ != end_) {
    if (*current_ == '\\') {
      char_class += HandleEscapeInCharClass();
    } else {
      char_class += CharToUTF8(*current_);
      ++current_;
    }
  }
  if (current_ == end_) {
    RaiseError("Unclosed '['");
  }
  char_class += ']';
  ++current_;
  return char_class;
}

// {x}: Match exactly x occurrences of the preceding regular expression.
// {x,}
// {x,y}
std::string RegexConverter::HandleRepetitionRange() {
  std::string result = "{";
  ++current_;
  if (!isdigit(*current_)) {
    RaiseError("Invalid repetition count.");
  }
  while (isdigit(*current_)) {
    result += static_cast<char>(*current_);
    ++current_;
  }
  if (*current_ != ',' && *current_ != '}') {
    RaiseError("Invalid repetition count.");
  }
  result += static_cast<char>(*current_);
  ++current_;
  if (current_[-1] == '}') {
    // Matches {x}
    return result;
  }
  if (!isdigit(*current_) && *current_ != '}') {
    RaiseError("Invalid repetition count.");
  }
  while (isdigit(*current_)) {
    result += static_cast<char>(*current_);
    ++current_;
  }
  if (*current_ != '}') {
    RaiseError("Invalid repetition count.");
  }
  result += '}';
  ++current_;
  return result;
}

std::string RegexConverter::HandleCharEscape() {
  // clang-format off
  static const std::unordered_map<char, TCodepoint> CUSTOM_ESCAPE_MAP = {
      {'^', '^'}, {'$', '$'}, {'.', '.'}, {'*', '*'}, {'+', '+'}, {'?', '?'}, {'\\', '\\'},
      {'(', '('}, {')', ')'}, {'[', '['}, {']', ']'}, {'{', '{'}, {'}', '}'}, {'|', '|'},
      {'/', '/'}, {'-', '-'}
  };
  // clang-format on
  if (end_ - current_ < 2 || (current_[1] == 'u' && end_ - current_ < 5) ||
      (current_[1] == 'x' && end_ - current_ < 4) || (current_[1] == 'c' && end_ - current_ < 3)) {
    RaiseError("Escape sequence is not finished.");
  }
  auto [codepoint, len] = ParseNextEscaped(current_, CUSTOM_ESCAPE_MAP);
  if (codepoint != CharHandlingError::kInvalidEscape) {
    current_ += len;
    return EscapeString(codepoint);
  } else if (current_[1] == 'u' && current_[2] == '{') {
    current_ += 3;
    int len = 0;
    TCodepoint value = 0;
    while (HexCharToInt(current_[len]) != -1 && len <= 6) {
      value = value * 16 + HexCharToInt(current_[len]);
      ++len;
    }
    if (len == 0 || len > 6 || current_[len] != '}') {
      RaiseError("Invalid Unicode escape sequence.");
    }
    current_ += len + 1;
    return EscapeString(value);
  } else if (current_[1] == 'c') {
    current_ += 2;
    if (!std::isalpha(*current_)) {
      RaiseError("Invalid control character escape sequence.");
    }
    ++current_;
    return EscapeString((*(current_ - 1)) % 32);
  } else {
    RaiseWarning(
        "Escape sequence '\\" + EscapeString(current_[1]) +
        "' is not recognized. The character itself will be matched"
    );
    current_ += 2;
    return EscapeString(current_[-1]);
  }
}

std::string RegexConverter::HandleEscapeInCharClass() {
  if (end_ - current_ < 2) {
    RaiseError("Escape sequence is not finished.");
  }
  if (current_[1] == 'd') {
    current_ += 2;
    return "0-9";
  } else if (current_[1] == 'D') {
    current_ += 2;
    return R"(\x00-\x2F\x3A-\U0010FFFF)";
  } else if (current_[1] == 'w') {
    current_ += 2;
    return "a-zA-Z0-9_";
  } else if (current_[1] == 'W') {
    current_ += 2;
    return R"(\x00-\x2F\x3A-\x40\x5B-\x5E\x60\x7B-\U0010FFFF)";
  } else if (current_[1] == 's') {
    current_ += 2;
    return R"(\f\n\r\t\v\u0020\u00a0)";
  } else if (current_[1] == 'S') {
    current_ += 2;
    return R"(\x00-\x08\x0E-\x1F\x21-\x9F\xA1-\U0010FFFF)";
  } else {
    auto res = HandleCharEscape();
    if (res == "]" || res == "-") {
      return "\\" + res;
    } else {
      return res;
    }
  }
}

std::string RegexConverter::HandleEscape() {
  // clang-format off
  static const std::unordered_map<char, TCodepoint> CUSTOM_ESCAPE_MAP = {
      {'^', '^'}, {'$', '$'}, {'.', '.'}, {'*', '*'}, {'+', '+'}, {'?', '?'}, {'\\', '\\'},
      {'(', '('}, {')', ')'}, {'[', '['}, {']', ']'}, {'{', '{'}, {'}', '}'}, {'|', '|'},
      {'/', '/'}
  };
  // clang-format on
  if (end_ - current_ < 2) {
    RaiseError("Escape sequence is not finished.");
  }
  if (current_[1] == 'd') {
    current_ += 2;
    return "[0-9]";
  } else if (current_[1] == 'D') {
    current_ += 2;
    return "[^0-9]";
  } else if (current_[1] == 'w') {
    current_ += 2;
    return "[a-zA-Z0-9_]";
  } else if (current_[1] == 'W') {
    current_ += 2;
    return "[^a-zA-Z0-9_]";
  } else if (current_[1] == 's') {
    current_ += 2;
    return R"([\f\n\r\t\v\u0020\u00a0])";
  } else if (current_[1] == 'S') {
    current_ += 2;
    return R"([^[\f\n\r\t\v\u0020\u00a0])";
  } else if ((current_[1] >= '1' && current_[1] <= '9') || current_[1] == 'k') {
    RaiseError("Backreference is not supported yet.");
  } else if (current_[1] == 'p' || current_[1] == 'P') {
    RaiseError("Unicode character class escape sequence is not supported yet.");
  } else if (current_[1] == 'b' || current_[1] == 'B') {
    RaiseError("Word boundary is not supported yet.");
  } else {
    return "\"" + HandleCharEscape() + "\"";
  }
}

void RegexConverter::HandleGroupModifier() {
  if (current_ == end_) {
    RaiseError("Group modifier is not finished.");
  }
  if (*current_ == ':') {
    // Non-capturing group.
    ++current_;
  } else if (*current_ == '=' || *current_ == '!') {
    // Positive or negative lookahead.
    RaiseError("Lookahead is not supported yet.");
  } else if (*current_ == '<' && current_ + 1 != end_ &&
             (current_[1] == '=' || current_[1] == '!')) {
    // Positive or negative lookbehind.
    RaiseError("Lookbehind is not supported yet.");
  } else if (*current_ == '<') {
    ++current_;
    while (current_ != end_ && isalpha(*current_)) {
      ++current_;
    }
    if (current_ == end_ || *current_ != '>') {
      RaiseError("Invalid named capturing group.");
    }
    // Just ignore the named of the group.
    ++current_;
  } else {
    // Group modifier flag.
    RaiseError("Group modifier flag is not supported yet.");
  }
}

std::string RegexConverter::Convert() {
  start_ = regex_codepoints_.data();
  current_ = start_;
  end_ = start_ + regex_codepoints_.size() - 1;
  bool is_empty = true;
  while (current_ != end_) {
    if (*current_ == '^') {
      if (current_ != start_) {
        RaiseWarning(
            "'^' should be at the start of the regex, but found in the middle. It is ignored."
        );
      }
      ++current_;
    } else if (*current_ == '$') {
      if (current_ != end_ - 1) {
        RaiseWarning(
            "'$' should be at the end of the regex, but found in the middle. It is ignored."
        );
      }
      ++current_;
    } else if (*current_ == '[') {
      is_empty = false;
      AddEBNFSegment(HandleCharacterClass());
    } else if (*current_ == '(') {
      is_empty = false;
      ++current_;
      ++parenthesis_level_;
      AddEBNFSegment("(");
      if (current_ != end_ && *current_ == '?') {
        ++current_;
        HandleGroupModifier();
      }
    } else if (*current_ == ')') {
      is_empty = false;
      if (parenthesis_level_ == 0) {
        RaiseError("Unmatched ')'");
      }
      // Empty alternative before ')' (e.g. "(a|)" or "(a|$)"): emit "" so it isn't a bare '|'.
      if (!result_ebnf_.empty() && result_ebnf_.back() == '|') {
        AddEBNFSegment("\"\"");
      }
      --parenthesis_level_;
      AddEBNFSegment(")");
      ++current_;
    } else if (*current_ == '*' || *current_ == '+' || *current_ == '?') {
      is_empty = false;
      result_ebnf_ += static_cast<char>(*current_);
      ++current_;
      if (current_ != end_ && *current_ == '?') {
        // Ignore the non-greedy modifier because our grammar handles all repetition numbers
        // non-deterministically.
        ++current_;
      }
      if (current_ != end_ &&
          (*current_ == '{' || *current_ == '*' || *current_ == '+' || *current_ == '?')) {
        RaiseError("Two consecutive repetition modifiers are not allowed.");
      }
    } else if (*current_ == '{') {
      is_empty = false;
      result_ebnf_ += HandleRepetitionRange();
      if (current_ != end_ && *current_ == '?') {
        // Still ignore the non-greedy modifier.
        ++current_;
      }
      if (current_ != end_ &&
          (*current_ == '{' || *current_ == '*' || *current_ == '+' || *current_ == '?')) {
        RaiseError("Two consecutive repetition modifiers are not allowed.");
      }
    } else if (*current_ == '|') {
      is_empty = false;
      // Empty alternative before '|': emit "" so there's no bare '|' on the left.
      // Covers leading ("^$|abc"), consecutive ("a||b") and group-start ("(|a)") cases.
      if (result_ebnf_.empty() || result_ebnf_.back() == '|' || result_ebnf_.back() == '(') {
        AddEBNFSegment("\"\"");
      }
      AddEBNFSegment("|");
      ++current_;
    } else if (*current_ == '\\') {
      is_empty = false;
      AddEBNFSegment(HandleEscape());
    } else if (*current_ == '.') {
      is_empty = false;
      AddEBNFSegment(R"([\u0000-\U0010FFFF])");
      ++current_;
    } else {
      is_empty = false;
      // Non-special characters are matched literally.
      AddEBNFSegment("\"" + EscapeString(*current_) + "\"");
      ++current_;
    }
  }
  if (parenthesis_level_ != 0) {
    RaiseError("The parenthesis is not closed.");
  }
  // Trailing empty alternative, e.g. "abc|": emit "" so it doesn't end with a bare '|'.
  if (!result_ebnf_.empty() && result_ebnf_.back() == '|') {
    AddEBNFSegment("\"\"");
  }
  if (is_empty) {
    AddEBNFSegment("\"\"");
  }
  return result_ebnf_;
}

std::string RegexToEBNF(const std::string& regex, bool with_rule_name) {
  RegexConverter converter(regex);
  if (with_rule_name) {
    return "root ::= " + converter.Convert() + "\n";
  } else {
    return converter.Convert();
  }
}

// ===========================================================================
// Length-threading regex -> right-linear grammar.
// Builds a grammar for L(regex) intersected with a code-point length interval:
//   regex -> AST -> Glushkov position automaton (nullable/first/last/follow)
//         -> product with a code-point-length counter -> coaccessible pruning
//         -> right-linear code-point EBNF states.
// Any unsupported construct throws LtFallback, caught by the public function
// which then reports over_cap=true so the caller falls back to the plain
// pattern (over-accepting, i.e. no worse than the status quo) rather than
// crashing.
// ===========================================================================
namespace {

using Range = std::pair<TCodepoint, TCodepoint>;
constexpr TCodepoint kLtMaxCp = 0x10FFFF;
constexpr TCodepoint kLtNone = static_cast<TCodepoint>(-1);
// Group-nesting bound: the parser and every AST pass (Clone, NumberPositions, ComputeNFL)
// recurse to the nesting depth, so an unbounded depth lets an adversarial schema pattern
// ("(((((..." ) overflow the stack. The node budget does not bound depth (parens are free).
constexpr int kLtMaxGroupDepth = 1000;

// Signals "cannot build a length-threaded grammar for this pattern; fall back".
struct LtFallback {};

enum class LtType { kChar, kEmpty, kConcat, kAlt, kStar, kPlus, kOpt };

struct LtNode {
  LtType type;
  std::vector<Range> ranges;  // kChar only
  int pos = -1;               // kChar only, assigned by NumberPositions
  std::vector<std::unique_ptr<LtNode>> children;
};
using LtPtr = std::unique_ptr<LtNode>;

std::vector<Range> LtComplement(std::vector<Range> rs) {
  std::sort(rs.begin(), rs.end());
  std::vector<Range> out;
  TCodepoint cur = 0;
  for (const auto& [lo, hi] : rs) {
    if (lo > cur) out.push_back({cur, lo - 1});
    if (hi + 1 > cur) cur = hi + 1;
  }
  if (cur <= kLtMaxCp) out.push_back({cur, kLtMaxCp});
  return out;
}

// Predefined regex classes as code-point ranges. Must stay in sync with
// RegexConverter::HandleEscape / HandleEscapeInCharClass (the \d \w \s definitions).
const std::vector<Range> kDigit = {{48, 57}};
const std::vector<Range> kWord = {{48, 57}, {65, 90}, {95, 95}, {97, 122}};
const std::vector<Range> kSpace = {{9, 13}, {32, 32}, {0xA0, 0xA0}};  // \t\n\v\f\r, space, U+00A0

// Escaped-metacharacter map matching RegexConverter::HandleCharEscape's CUSTOM_ESCAPE_MAP.
// Passed to the shared ParseNextEscaped so \. \+ \( ... decode to their literal code point,
// exactly as the plain converter does.
const std::unordered_map<char, TCodepoint> kLtEscapeMap = {
    {'^', '^'},
    {'$', '$'},
    {'.', '.'},
    {'*', '*'},
    {'+', '+'},
    {'?', '?'},
    {'\\', '\\'},
    {'(', '('},
    {')', ')'},
    {'[', '['},
    {']', ']'},
    {'{', '{'},
    {'}', '}'},
    {'|', '|'},
    {'/', '/'},
    {'-', '-'}
};

// --- Recursive-descent parser: regex -> AST (xgrammar-supported subset). ---
class LtParser {
 public:
  LtParser(const std::string& regex, int budget) : budget_(budget) {
    cps_ = ParseUTF8(regex.c_str(), false);
    if (!cps_.empty() && cps_[0] == kInvalidUTF8) throw LtFallback{};
    n_ = cps_.size();
    cps_.push_back(0);  // sentinel so ParseNextEscaped can scan safely past the last token
  }

  LtPtr Parse() {
    LtPtr r = Alt();
    if (i_ != n_) throw LtFallback{};
    return r;
  }

 private:
  std::vector<TCodepoint> cps_;
  size_t n_ = 0, i_ = 0;
  int budget_;
  int nodes_ = 0;
  int depth_ = 0;

  TCodepoint Peek() const { return i_ < n_ ? cps_[i_] : kLtNone; }

  // Sole chokepoint for AST memory: every node construction (including Clone)
  // must go through here, so the node budget bounds total AST size even for
  // expansions whose repeated unit contains no kChar atom (e.g. `(?:){9999}`).
  LtPtr Make(LtType t) {
    if (++nodes_ > budget_) throw LtFallback{};
    auto nd = std::make_unique<LtNode>();
    nd->type = t;
    return nd;
  }
  LtPtr MakeChar(std::vector<Range> ranges) {
    auto nd = Make(LtType::kChar);
    nd->ranges = std::move(ranges);
    return nd;
  }
  LtPtr MakeUnary(LtType t, LtPtr child) {
    auto nd = Make(t);
    nd->children.push_back(std::move(child));
    return nd;
  }

  LtPtr Clone(const LtNode* nd) {
    auto out = Make(nd->type);
    out->ranges = nd->ranges;
    for (const auto& c : nd->children) out->children.push_back(Clone(c.get()));
    return out;
  }

  LtPtr Alt() {
    std::vector<LtPtr> opts;
    opts.push_back(Concat());
    while (Peek() == '|') {
      ++i_;
      opts.push_back(Concat());
    }
    if (opts.size() == 1) return std::move(opts[0]);
    auto nd = Make(LtType::kAlt);
    nd->children = std::move(opts);
    return nd;
  }

  LtPtr Concat() {
    std::vector<LtPtr> items;
    while (true) {
      TCodepoint c = Peek();
      if (c == kLtNone || c == '|' || c == ')') break;
      items.push_back(Repeat());
    }
    if (items.empty()) return Make(LtType::kEmpty);
    if (items.size() == 1) return std::move(items[0]);
    auto nd = Make(LtType::kConcat);
    nd->children = std::move(items);
    return nd;
  }

  void SkipLazy() {
    if (Peek() == '?') ++i_;
  }

  LtPtr Repeat() {
    LtPtr node = Atom();
    TCodepoint c = Peek();
    if (c == '*') {
      ++i_;
      node = MakeUnary(LtType::kStar, std::move(node));
      SkipLazy();
    } else if (c == '+') {
      ++i_;
      node = MakeUnary(LtType::kPlus, std::move(node));
      SkipLazy();
    } else if (c == '?') {
      ++i_;
      node = MakeUnary(LtType::kOpt, std::move(node));
      SkipLazy();
    } else if (c == '{' && i_ + 1 < n_ && cps_[i_ + 1] >= '0' && cps_[i_ + 1] <= '9') {
      node = RepeatRange(std::move(node));
      SkipLazy();
    }
    return node;
  }

  // Parses the digit run at i_ as a non-negative int. std::from_chars rejects
  // an overflowing count (e.g. `{99999999999999}`) and an empty run cleanly,
  // both -> fallback. Digits are ASCII, so narrowing to char is safe.
  int ParseRepeatCount() {
    std::string digits;
    while (Peek() >= '0' && Peek() <= '9') digits.push_back(static_cast<char>(cps_[i_++]));
    int value = 0;
    const char* end = digits.data() + digits.size();
    auto [ptr, ec] = std::from_chars(digits.data(), end, value);
    if (ec != std::errc() || ptr != end) throw LtFallback{};
    return value;
  }

  LtPtr RepeatRange(LtPtr child) {
    ++i_;  // skip '{'
    int lo = ParseRepeatCount();
    int hi;  // -1 = unbounded
    if (Peek() == '}') {
      hi = lo;
      ++i_;
    } else if (Peek() == ',') {
      ++i_;
      if (Peek() == '}') {
        hi = -1;
        ++i_;
      } else {
        hi = ParseRepeatCount();
        if (Peek() != '}') throw LtFallback{};
        ++i_;
      }
    } else {
      throw LtFallback{};
    }
    // {3,2}: invalid regex; the expansion below would silently treat it as {3}.
    if (hi >= 0 && hi < lo) throw LtFallback{};
    // Expand: child^lo . (child?)^(hi-lo)  or  child^lo . child*  (hi == -1).
    // A huge count needs no explicit bound: every Clone goes through Make, so
    // the node budget trips after at most budget_ iterations.
    std::vector<LtPtr> items;
    for (int k = 0; k < lo; ++k) items.push_back(Clone(child.get()));
    if (hi < 0) {
      items.push_back(MakeUnary(LtType::kStar, Clone(child.get())));
    } else {
      for (int k = 0; k < hi - lo; ++k) {
        items.push_back(MakeUnary(LtType::kOpt, Clone(child.get())));
      }
    }
    if (items.empty()) return Make(LtType::kEmpty);
    if (items.size() == 1) return std::move(items[0]);
    auto nd = Make(LtType::kConcat);
    nd->children = std::move(items);
    return nd;
  }

  LtPtr Atom() {
    TCodepoint c = Peek();
    if (c == '(') {
      ++i_;
      if (++depth_ > kLtMaxGroupDepth) throw LtFallback{};
      if (Peek() == '?') {
        if (i_ + 1 < n_ && cps_[i_ + 1] == ':') {
          i_ += 2;  // non-capturing group
        } else {
          throw LtFallback{};  // lookahead / named / flags: fall back
        }
      }
      LtPtr inner = Alt();
      if (Peek() != ')') throw LtFallback{};
      ++i_;
      --depth_;
      return inner;
    }
    if (c == '[') return CharClass();
    if (c == '.') {
      ++i_;
      return MakeChar({{0, kLtMaxCp}});
    }
    if (c == '\\') return Escape();
    if (c == '^' || c == '$') {
      ++i_;
      // A quantifier applied directly to a bare anchor (`a^*b`, `a$+b`, `a^{2}b`): the anchor is
      // ε, so quantifying it would build a grammar for a different language than the plain
      // converter (which drops the anchor and attaches the quantifier to the preceding element).
      // Reject, mirroring Repeat()'s quantifier detection, to keep the two paths consistent — the
      // dangling-quantifier fallback below never sees these because the anchor consumes the atom.
      TCodepoint q = Peek();
      if (q == '*' || q == '+' || q == '?' ||
          (q == '{' && i_ + 1 < n_ && cps_[i_ + 1] >= '0' && cps_[i_ + 1] <= '9')) {
        throw LtFallback{};
      }
      return Make(LtType::kEmpty);
    }
    // A quantifier with nothing to repeat (leading `*a`, consecutive `a**`, `a*+`, `a{2}{3}`:
    // the second quantifier lands here after Repeat() consumed the first) or a `{` that Repeat()
    // did not consume as a valid repetition range (`a{x}`, `a{,5}`): invalid regex. The plain
    // converter rejects all of these; treating them as literals would silently build a grammar
    // for a different language.
    if (c == '*' || c == '+' || c == '?' || c == '{') throw LtFallback{};
    if (c == kLtNone) throw LtFallback{};
    ++i_;
    return MakeChar({{c, c}});
  }

  LtPtr Escape() {
    // cps_[i_] == '\\'. Class escapes (\d \w \s ...) expand to a set of ranges; every other
    // escape is decoded to one code point by the shared ParseNextEscaped, so \uHHHH / \xHH /
    // \n / escaped metacharacters match RegexConverter exactly (single source of decoding).
    if (i_ + 1 >= n_) throw LtFallback{};
    TCodepoint e = cps_[i_ + 1];
    switch (e) {
      case 'd':
        i_ += 2;
        return MakeChar(kDigit);
      case 'w':
        i_ += 2;
        return MakeChar(kWord);
      case 's':
        i_ += 2;
        return MakeChar(kSpace);
      case 'D':
        i_ += 2;
        return MakeChar(LtComplement(kDigit));
      case 'W':
        i_ += 2;
        return MakeChar(LtComplement(kWord));
      case 'S':
        i_ += 2;
        return MakeChar(LtComplement(kSpace));
      default:
        break;
    }
    // Non-regular constructs (backreference / word boundary / unicode property) and the two
    // escape forms ParseNextEscaped does not cover (\u{...} braces, \cX control): fall back.
    if ((e >= '1' && e <= '9') || e == 'b' || e == 'B' || e == 'k' || e == 'p' || e == 'P' ||
        e == 'c' || (e == 'u' && i_ + 2 < n_ && cps_[i_ + 2] == '{')) {
      throw LtFallback{};
    }
    auto [cp, len] = ParseNextEscaped(cps_.data() + i_, kLtEscapeMap);
    // The shared decoder does not bound \x hex-digit runs, so an over-long escape (\xFFFFFFFF)
    // can decode to garbage outside the code-point range; also keeps LtComplement's hi + 1 safe.
    if (cp == kInvalidEscape || len <= 0 || cp < 0 || cp > kLtMaxCp) throw LtFallback{};
    i_ += len;
    return MakeChar({{cp, cp}});
  }

  // Returns true and sets *cp for a single code point; returns false and fills
  // *set for an escape class (\d, \w, ...).
  bool ClassAtom(TCodepoint* cp, std::vector<Range>* set) {
    if (cps_[i_] != '\\') {
      *cp = cps_[i_++];
      return true;
    }
    if (i_ + 1 >= n_) throw LtFallback{};
    TCodepoint e = cps_[i_ + 1];
    switch (e) {
      case 'd':
        i_ += 2;
        *set = kDigit;
        return false;
      case 'w':
        i_ += 2;
        *set = kWord;
        return false;
      case 's':
        i_ += 2;
        *set = kSpace;
        return false;
      case 'D':
        i_ += 2;
        *set = LtComplement(kDigit);
        return false;
      case 'W':
        i_ += 2;
        *set = LtComplement(kWord);
        return false;
      case 'S':
        i_ += 2;
        *set = LtComplement(kSpace);
        return false;
      default:
        break;
    }
    // Forms ParseNextEscaped cannot decode (\u{...} braces, \cX control) need no explicit guard:
    // the decoder returns kInvalidEscape for them (\c is unmapped; \u{ fails on the first hex
    // digit), so the check below falls back. Inside a class \b is backspace and is handled by the
    // shared decoder (matching RegexConverter) -- unlike Escape(), which must pre-empt \b before it
    // is silently decoded to backspace instead of taking the word-boundary fallback.
    auto [decoded, len] = ParseNextEscaped(cps_.data() + i_, kLtEscapeMap);
    // Same code-point range check as Escape(): unbounded \x hex runs decode to garbage.
    if (decoded == kInvalidEscape || len <= 0 || decoded < 0 || decoded > kLtMaxCp) {
      throw LtFallback{};
    }
    i_ += len;
    *cp = decoded;
    return true;
  }

  LtPtr CharClass() {
    ++i_;  // skip '['
    bool neg = false;
    if (Peek() == '^') {
      neg = true;
      ++i_;
    }
    std::vector<Range> ranges;
    while (Peek() != ']') {
      if (Peek() == kLtNone) throw LtFallback{};
      TCodepoint lo;
      std::vector<Range> set;
      if (!ClassAtom(&lo, &set)) {
        ranges.insert(ranges.end(), set.begin(), set.end());
        continue;
      }
      if (Peek() == '-' && i_ + 1 < n_ && cps_[i_ + 1] != ']') {
        ++i_;  // skip '-'
        TCodepoint hi;
        std::vector<Range> set2;
        if (!ClassAtom(&hi, &set2)) throw LtFallback{};
        // Reversed range like [z-a]: invalid regex. LtComplement would silently turn its
        // negation into the full code-point set; fall back so the plain path rejects it.
        if (lo > hi) throw LtFallback{};
        ranges.push_back({lo, hi});
      } else {
        ranges.push_back({lo, lo});
      }
    }
    ++i_;  // skip ']'
    if (neg) ranges = LtComplement(std::move(ranges));
    if (ranges.empty()) throw LtFallback{};
    return MakeChar(std::move(ranges));
  }
};

// --- Glushkov: number positions, then compute nullable/first/last/follow. ---
void NumberPositions(LtNode* nd, int* cnt, std::vector<std::vector<Range>>* pos_ranges) {
  if (nd->type == LtType::kChar) {
    nd->pos = (*cnt)++;
    pos_ranges->push_back(nd->ranges);
  } else {
    for (auto& c : nd->children) NumberPositions(c.get(), cnt, pos_ranges);
  }
}

struct NFL {
  bool nullable;
  std::set<int> first, last;
};

// The follow sets can be Θ(positions²) (e.g. a wide alternation under a star), which the
// positions*length state cap does not bound. `*follow_budget` counts every attempted follow
// insertion (an upper bound on Σ|follow|) and trips *before* the work is done, so an
// adversarial pattern cannot make this pass itself consume quadratic time/memory.
void LtChargeFollowBudget(size_t inserted, int64_t* follow_budget) {
  *follow_budget -= static_cast<int64_t>(inserted);
  if (*follow_budget < 0) throw LtFallback{};
}

NFL ComputeNFL(LtNode* nd, std::vector<std::set<int>>* follow, int64_t* follow_budget) {
  switch (nd->type) {
    case LtType::kEmpty:
      return {true, {}, {}};
    case LtType::kChar:
      return {false, {nd->pos}, {nd->pos}};
    case LtType::kConcat: {
      bool nul = true;
      std::set<int> fst, lst;
      for (auto& c : nd->children) {
        NFL x = ComputeNFL(c.get(), follow, follow_budget);
        LtChargeFollowBudget(lst.size() * x.first.size(), follow_budget);
        for (int p : lst) (*follow)[p].insert(x.first.begin(), x.first.end());
        if (nul) fst.insert(x.first.begin(), x.first.end());
        if (x.nullable) {
          lst.insert(x.last.begin(), x.last.end());
        } else {
          lst = x.last;
        }
        nul = nul && x.nullable;
      }
      return {nul, fst, lst};
    }
    case LtType::kAlt: {
      bool nul = false;
      std::set<int> fst, lst;
      for (auto& c : nd->children) {
        NFL x = ComputeNFL(c.get(), follow, follow_budget);
        nul = nul || x.nullable;
        fst.insert(x.first.begin(), x.first.end());
        lst.insert(x.last.begin(), x.last.end());
      }
      return {nul, fst, lst};
    }
    case LtType::kStar:
    case LtType::kPlus: {
      NFL x = ComputeNFL(nd->children[0].get(), follow, follow_budget);
      LtChargeFollowBudget(x.last.size() * x.first.size(), follow_budget);
      for (int p : x.last) (*follow)[p].insert(x.first.begin(), x.first.end());
      return {nd->type == LtType::kStar ? true : x.nullable, x.first, x.last};
    }
    case LtType::kOpt: {
      NFL x = ComputeNFL(nd->children[0].get(), follow, follow_budget);
      return {true, x.first, x.last};
    }
  }
  throw LtFallback{};  // unreachable
}

std::string EmitCodepoint(TCodepoint cp) {
  char buf[16];
  if (cp <= 0xFFFF) {
    std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(cp));
  } else {
    std::snprintf(buf, sizeof(buf), "\\U%08X", static_cast<unsigned>(cp));
  }
  return buf;
}

std::string EmitClass(const std::vector<Range>& ranges) {
  std::string s = "[";
  for (const auto& [lo, hi] : ranges) {
    s += (lo == hi) ? EmitCodepoint(lo) : EmitCodepoint(lo) + "-" + EmitCodepoint(hi);
  }
  return s + "]";
}

}  // namespace

LengthThreadedProduct BuildLengthProduct(const GlushkovAutomaton& nfa, int min_len, int max_len) {
  // State = (pos, k). pos == -1 is the virtual start; k = code points consumed. States are
  // discovered by forward traversal, so every state in the result is accessible. The dense
  // (pos, k) grid is small by construction (the caller caps it), so a flat index array is
  // cheaper than a map.
  const int m = static_cast<int>(nfa.pos_class.size());
  LengthThreadedProduct out;
  std::vector<int32_t> idx(static_cast<size_t>(m + 1) * (static_cast<size_t>(max_len) + 1), -1);
  std::vector<std::pair<int, int>> states;  // id -> (pos, k)
  std::vector<int32_t> frontier;
  auto get_id = [&](int pos, int k) -> int32_t {
    int32_t& slot = idx[static_cast<size_t>(pos + 1) * (static_cast<size_t>(max_len) + 1) + k];
    if (slot < 0) {
      slot = static_cast<int32_t>(states.size());
      states.push_back({pos, k});
      out.edges.emplace_back();
      out.accepting.push_back(0);
      frontier.push_back(slot);
    }
    return slot;
  };

  get_id(-1, 0);  // start state, becomes index 0
  while (!frontier.empty()) {
    int32_t s = frontier.back();
    frontier.pop_back();
    auto [pos, k] = states[s];
    const std::vector<int32_t>& fol = (pos == -1) ? nfa.first : nfa.follow[pos];
    if (pos == -1) {
      out.accepting[s] = nfa.nullable && min_len <= 0;  // 0 <= max_len guaranteed
    } else {
      out.accepting[s] = nfa.last[pos] && (min_len <= k && k <= max_len);
    }
    if (k + 1 <= max_len) {
      for (int32_t q : fol) {
        int32_t t = get_id(q, k + 1);
        out.edges[s].push_back({nfa.pos_class[q], t});
      }
    }
  }
  return out;
}

std::vector<char> ComputeCoaccessible(const LengthThreadedProduct& product) {
  // Reverse BFS from the accepting states: O(V + E).
  const size_t n = product.edges.size();
  XGRAMMAR_DCHECK(product.accepting.size() == n)
      << "accepting and edges must have one entry per state";
  std::vector<std::vector<int32_t>> rev(n);
  for (size_t i = 0; i < n; ++i) {
    for (const auto& [cls, t] : product.edges[i]) {
      XGRAMMAR_DCHECK(t >= 0 && static_cast<size_t>(t) < n) << "edge target out of range";
      rev[t].push_back(static_cast<int32_t>(i));
    }
  }
  std::vector<char> co(n, 0);
  std::vector<int32_t> frontier;
  for (size_t i = 0; i < n; ++i) {
    if (product.accepting[i]) {
      co[i] = 1;
      frontier.push_back(static_cast<int32_t>(i));
    }
  }
  while (!frontier.empty()) {
    int32_t s = frontier.back();
    frontier.pop_back();
    for (int32_t p : rev[s]) {
      if (!co[p]) {
        co[p] = 1;
        frontier.push_back(p);
      }
    }
  }
  return co;
}

LengthThreadedGrammar CompactProduct(
    const LengthThreadedProduct& product, const std::vector<char>& coaccessible
) {
  LengthThreadedGrammar out;
  XGRAMMAR_DCHECK(product.accepting.size() == product.edges.size())
      << "accepting and edges must have one entry per state";
  XGRAMMAR_DCHECK(coaccessible.size() == product.edges.size())
      << "coaccessible must have one flag per state";
  if (product.edges.empty() || !coaccessible[0]) {  // intersection empty
    out.empty = true;
    return out;
  }
  // Keep coaccessible states only, renumbering densely; start stays index 0.
  const size_t n = product.edges.size();
  std::vector<int32_t> remap(n, -1);
  int32_t cnt = 0;
  for (size_t i = 0; i < n; ++i) {
    if (coaccessible[i]) remap[i] = cnt++;
  }
  out.states.resize(cnt);
  for (size_t i = 0; i < n; ++i) {
    if (!coaccessible[i]) continue;
    LengthThreadedState& st = out.states[remap[i]];
    st.accepting = product.accepting[i];
    for (const auto& [cls, t] : product.edges[i]) {
      if (coaccessible[t]) st.edges.push_back({cls, remap[t]});
    }
  }
  return out;
}

LengthThreadedGrammar RegexToLengthThreadedGrammar(
    const std::string& regex, int min_len, int max_len, int max_states
) {
  LengthThreadedGrammar out;
  // The caller handles the unbounded / invalid cases; be defensive and fall back.
  if (max_len < 0 || min_len < 0 || min_len > max_len) {
    out.over_cap = true;
    return out;
  }

  // Parse the regex and derive its Glushkov automaton (code-point-class level).
  GlushkovAutomaton nfa;
  try {
    LtParser parser(regex, max_states);
    LtPtr ast = parser.Parse();
    int m = 0;
    std::vector<std::vector<Range>> pos_ranges;
    NumberPositions(ast.get(), &m, &pos_ranges);
    // Pre-construction soft cap: states <= (positions + 1) * (max_len + 1). The additions are
    // done in 64 bits: max_len can be INT_MAX (from a schema), so max_len + 1 in int is UB.
    if ((static_cast<int64_t>(m) + 1) * (static_cast<int64_t>(max_len) + 1) > max_states) {
      out.over_cap = true;
      return out;
    }
    std::vector<std::set<int>> follow(m);
    int64_t follow_budget = max_states;
    NFL root = ComputeNFL(ast.get(), &follow, &follow_budget);
    // Text budget: the state cap bounds V but not the emitted grammar size. Every edge into a
    // position q carries q's char-class text, whose size is proportional to its range count, so
    // memory / grammar-text / compile cost scale with Σ_edge ranges(target), NOT the edge count.
    // A single class with thousands of ranges is one node and one follow entry yet copies its
    // whole text onto every edge (× max_len) — bounding edge count alone lets that blow past the
    // cap. So weight each edge by ranges(target): start emits |first| edges, and each (p, k) with
    // k < max_len emits |follow(p)| edges, giving Σ_{q∈first} ranges(q) + Σ_p Σ_{q∈follow(p)}
    // ranges(q) * max_len. Exact from the follow sets and pos_ranges (nothing is built yet), so it
    // stays an ex-ante check; when every class is a single range this reduces to the edge bound.
    auto ranges_of = [&](int q) { return static_cast<int64_t>(pos_ranges[q].size()); };
    int64_t first_weight = 0;
    for (int q : root.first) first_weight += ranges_of(q);
    int64_t follow_weight = 0;
    for (const auto& f : follow) {
      for (int q : f) follow_weight += ranges_of(q);
    }
    if (first_weight + follow_weight * static_cast<int64_t>(max_len) > max_states) {
      out.over_cap = true;
      return out;
    }
    nfa.nullable = root.nullable;
    nfa.first.assign(root.first.begin(), root.first.end());
    nfa.pos_class.reserve(m);
    for (const auto& ranges : pos_ranges) nfa.pos_class.push_back(EmitClass(ranges));
    nfa.follow.reserve(m);
    for (const auto& f : follow) nfa.follow.emplace_back(f.begin(), f.end());
    nfa.last.assign(m, 0);
    for (int p : root.last) nfa.last[p] = 1;
  } catch (const LtFallback&) {
    out.over_cap = true;
    return out;
  }

  LengthThreadedProduct product = BuildLengthProduct(nfa, min_len, max_len);
  return CompactProduct(product, ComputeCoaccessible(product));
}

}  // namespace xgrammar
