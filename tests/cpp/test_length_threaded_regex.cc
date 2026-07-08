/*!
 *  Copyright (c) 2026 by Contributors
 * \file tests/cpp/test_length_threaded_regex.cc
 * \brief Unit tests for RegexToLengthThreadedGrammar: builds a right-linear
 *        grammar for a regex intersected with a code-point length interval.
 *        Covers structure, unsat (empty intersection), and the soft cap
 *        (over_cap).
 */

#include <gtest/gtest.h>
#include <xgrammar/xgrammar.h>

#include <cstdio>
#include <limits>
#include <string>

#include "regex_converter.h"

using namespace xgrammar;

namespace {

// Render a LengthThreadedGrammar to a standalone EBNF script (state 0 = root),
// mirroring what the JSON-schema wiring will emit via EBNFScriptCreator.
std::string Render(const LengthThreadedGrammar& g) {
  auto name = [](int i) { return i == 0 ? std::string("root") : "N" + std::to_string(i); };
  std::string out;
  for (size_t i = 0; i < g.states.size(); ++i) {
    const auto& st = g.states[i];
    std::string body;
    bool first = true;
    for (const auto& [cls, target] : st.edges) {
      if (!first) body += " | ";
      body += cls + " " + name(target);
      first = false;
    }
    if (st.accepting) {
      if (!first) body += " | ";
      body += "\"\"";
    }
    out += name(static_cast<int>(i)) + " ::= " + body + "\n";
  }
  return out;
}

}  // namespace

// The canonical multi-element example (§11.2): [a-z]+[0-9]+ ∩ [1,3].
// Structurally identical to the validated Python form-2 output (5 rules).
TEST(LengthThreadedRegexTest, CanonicalMultiElement) {
  auto g = RegexToLengthThreadedGrammar("[a-z]+[0-9]+", 1, 3);
  EXPECT_FALSE(g.empty);
  EXPECT_FALSE(g.over_cap);
  ASSERT_EQ(g.states.size(), 5u);

  const std::string expected =
      "root ::= [\\u0061-\\u007A] N1\n"
      "N1 ::= [\\u0061-\\u007A] N2 | [\\u0030-\\u0039] N3\n"
      "N2 ::= [\\u0030-\\u0039] N4\n"
      "N3 ::= [\\u0030-\\u0039] N4 | \"\"\n"
      "N4 ::= \"\"\n";
  EXPECT_EQ(Render(g), expected);

  // The rendered grammar must be well-formed EBNF.
  EXPECT_NO_THROW(Grammar::FromEBNF(Render(g)));
}

// Unsat: (cat|dog) has min length 3, so ∩ [0,2] is empty.
TEST(LengthThreadedRegexTest, EmptyIntersection) {
  auto g = RegexToLengthThreadedGrammar("(cat|dog)", 0, 2);
  EXPECT_TRUE(g.empty);
  EXPECT_TRUE(g.states.empty());
}

// Same pattern with room ([0,3]) is satisfiable.
TEST(LengthThreadedRegexTest, SatisfiableAlternation) {
  auto g = RegexToLengthThreadedGrammar("(cat|dog)", 0, 3);
  EXPECT_FALSE(g.empty);
  EXPECT_FALSE(g.over_cap);
  EXPECT_NO_THROW(Grammar::FromEBNF(Render(g)));
}

// Soft cap: positions * (max_len+1) over max_states -> over_cap, nothing built.
TEST(LengthThreadedRegexTest, OverCapFallsBack) {
  auto g = RegexToLengthThreadedGrammar("[a-z]", 0, 100000, /*max_states=*/50000);
  EXPECT_TRUE(g.over_cap);
  EXPECT_FALSE(g.empty);
  EXPECT_TRUE(g.states.empty());
}

// Unsupported constructs (backreference) fall back via over_cap, never crash.
TEST(LengthThreadedRegexTest, UnsupportedFallsBack) {
  auto g = RegexToLengthThreadedGrammar("(a)\\1", 0, 5);
  EXPECT_TRUE(g.over_cap);
}

// Invalid regex constructs must fall back (so the plain path rejects them
// consistently), never silently build a wrong grammar.
TEST(LengthThreadedRegexTest, ReversedClassRangeFallsBack) {
  EXPECT_TRUE(RegexToLengthThreadedGrammar("[z-a][0-9]", 2, 2).over_cap);
  // Negated form: the complement of a reversed range must not silently become
  // the full code-point set.
  EXPECT_TRUE(RegexToLengthThreadedGrammar("[^z-a][0-9]", 2, 2).over_cap);
}

TEST(LengthThreadedRegexTest, InvertedRepetitionFallsBack) {
  // {3,2} would otherwise be silently expanded as {3}.
  EXPECT_TRUE(RegexToLengthThreadedGrammar("a{3,2}b", 1, 9).over_cap);
}

TEST(LengthThreadedRegexTest, DanglingQuantifiersFallBack) {
  // Consecutive or leading quantifiers and malformed {n,m} shapes are invalid regex; treating
  // the extra quantifier as a literal would silently accept e.g. "a*". All must fall back so
  // the plain path decides.
  for (const char* pat :
       {"a**", "a*+", "a???", "a{2}{3}", "(a|b)++", "*a", "+", "a{x}", "a{,5}", "a{"}) {
    EXPECT_TRUE(RegexToLengthThreadedGrammar(pat, 0, 9).over_cap) << pat;
  }
  // Lazy quantifiers stay supported: the '?' after '*' is the non-greedy marker, not a
  // second quantifier.
  EXPECT_FALSE(RegexToLengthThreadedGrammar("a*?b", 0, 9).over_cap);
}

TEST(LengthThreadedRegexTest, OverlongHexEscapeFallsBack) {
  // The shared escape decoder does not bound \x hex digit runs; the decoded garbage code point
  // must not silently reach the emitter (it printed invalid ￿... classes).
  EXPECT_TRUE(RegexToLengthThreadedGrammar("a\\xFFFFFFFF", 1, 2).over_cap);
  EXPECT_TRUE(RegexToLengthThreadedGrammar("[a\\xFFFFFFFF]", 1, 2).over_cap);
}

// The state cap bounds positions * length, but the follow sets / product edges are quadratic
// in the alternation width. Both explosion shapes must trip the edge budget (over_cap) before
// building anything.
TEST(LengthThreadedRegexTest, WideAlternationOverCap) {
  // Follow-set shape: (a|a|...)* -- estimated states are tiny (max_len = 0).
  std::string wide = "(a";
  for (int i = 0; i < 4000; ++i) wide += "|a";
  wide += ")*";
  EXPECT_TRUE(RegexToLengthThreadedGrammar(wide, 0, 0).over_cap);

  // Product-edge shape: (a|b|...)(a|b|...) with max_len = 2.
  std::string alt = "(a";
  for (int i = 0; i < 2000; ++i) alt += "|a";
  alt += ")";
  EXPECT_TRUE(RegexToLengthThreadedGrammar(alt + alt, 0, 2).over_cap);
}

// A single char class with thousands of ranges is one node / one follow entry, so an edge-count
// cap lets it through; but its full text is copied onto every edge (x max_len), so the budget must
// weight edges by class size. A giant class x a modest max_len must trip over_cap, not emit MBs.
TEST(LengthThreadedRegexTest, GiantCharClassOverCap) {
  std::string cls = "[";
  for (int i = 0; i < 2000; ++i) {
    cls += "\\U";
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08X", 0x3000 + 2 * i);
    cls += buf;
  }
  cls += "]*d";
  // One position (the class) + one for 'd': the edge *count* is tiny, but the class text is huge.
  EXPECT_TRUE(RegexToLengthThreadedGrammar(cls, 0, 1500).over_cap);
  // A single-range class of the same shape stays well within budget.
  EXPECT_FALSE(RegexToLengthThreadedGrammar("[a-z]*d", 0, 1500).over_cap);
}

// A quantifier applied to a bare anchor (^ or $) must fall back, not quantify the anchor's
// epsilon into a different language than the plain converter (which drops the anchor).
TEST(LengthThreadedRegexTest, QuantifiedAnchorFallsBack) {
  EXPECT_TRUE(RegexToLengthThreadedGrammar("a^*b", 0, 4).over_cap);
  EXPECT_TRUE(RegexToLengthThreadedGrammar("a$+b", 0, 4).over_cap);
  EXPECT_TRUE(RegexToLengthThreadedGrammar("a^{2}b", 0, 4).over_cap);
  // A bare anchor without a following quantifier is fine (epsilon).
  EXPECT_FALSE(RegexToLengthThreadedGrammar("^ab$", 0, 4).over_cap);
}

// max_len = INT_MAX must not overflow the size estimate (max_len + 1 in int is UB); it simply
// exceeds the cap.
TEST(LengthThreadedRegexTest, IntMaxLengthOverCap) {
  auto g = RegexToLengthThreadedGrammar("ab", 0, std::numeric_limits<int>::max());
  EXPECT_TRUE(g.over_cap);
  EXPECT_FALSE(g.empty);
}

// Adversarial nesting depth: parsing recurses per group, so without a depth
// bound this overflows the stack (SIGSEGV) instead of falling back.
TEST(LengthThreadedRegexTest, DeepNestingFallsBackWithoutCrash) {
  std::string pat(100000, '(');
  pat += 'a';
  pat.append(100000, ')');
  auto g = RegexToLengthThreadedGrammar(pat, 1, 2);
  EXPECT_TRUE(g.over_cap);
  // Moderate nesting stays supported.
  EXPECT_FALSE(RegexToLengthThreadedGrammar("((((a|b))))", 1, 2).over_cap);
}

// Non-ASCII: length is counted in code points, not bytes. '你' is one code
// point, so [你]∩[1,1] accepts it; the grammar must be well-formed.
TEST(LengthThreadedRegexTest, NonAsciiCodePoint) {
  auto g = RegexToLengthThreadedGrammar("你+", 1, 2);
  EXPECT_FALSE(g.empty);
  EXPECT_FALSE(g.over_cap);
  EXPECT_NO_THROW(Grammar::FromEBNF(Render(g)));
}

// ---------------------------------------------------------------------------
// Phase tests: BuildLengthProduct / ComputeCoaccessible / CompactProduct.
// ---------------------------------------------------------------------------

namespace {

// Glushkov automaton of "AB" (two positions in sequence): first={0},
// follow: 0->{1}, 1->{}, last={1}, not nullable.
GlushkovAutomaton MakeSequenceAB() {
  GlushkovAutomaton nfa;
  nfa.nullable = false;
  nfa.pos_class = {"A", "B"};
  nfa.first = {0};
  nfa.follow = {{1}, {}};
  nfa.last = {0, 1};
  return nfa;
}

// Glushkov automaton of "A+" (one self-looping position).
GlushkovAutomaton MakePlusA() {
  GlushkovAutomaton nfa;
  nfa.nullable = false;
  nfa.pos_class = {"A"};
  nfa.first = {0};
  nfa.follow = {{0}};
  nfa.last = {1};
  return nfa;
}

}  // namespace

// Product of "AB" with [0,2]: exactly the chain start -A-> (0,1) -B-> (1,2),
// and only the final state (pattern-accepting position, 0 <= 2 <= 2) accepts.
TEST(LengthThreadedProductTest, SequenceChain) {
  auto p = BuildLengthProduct(MakeSequenceAB(), 0, 2);
  ASSERT_EQ(p.edges.size(), 3u);
  ASSERT_EQ(p.accepting.size(), 3u);

  ASSERT_EQ(p.edges[0].size(), 1u);  // start -A-> (0,1)
  EXPECT_EQ(p.edges[0][0].first, "A");
  int32_t s1 = p.edges[0][0].second;
  ASSERT_EQ(p.edges[s1].size(), 1u);  // (0,1) -B-> (1,2)
  EXPECT_EQ(p.edges[s1][0].first, "B");
  int32_t s2 = p.edges[s1][0].second;
  EXPECT_TRUE(p.edges[s2].empty());  // k+1 > max_len: no outgoing edges

  EXPECT_FALSE(p.accepting[0]);   // not nullable
  EXPECT_FALSE(p.accepting[s1]);  // position 0 is not a last position
  EXPECT_TRUE(p.accepting[s2]);
}

// Product truncated by max_len: "AB" with [0,1] still builds the dead state
// (0,1) — the edge is cut by max_len, and pruning is a separate phase.
TEST(LengthThreadedProductTest, MaxLenCutsEdges) {
  auto p = BuildLengthProduct(MakeSequenceAB(), 0, 1);
  ASSERT_EQ(p.edges.size(), 2u);  // start and (0,1); (1,2) is never created
  EXPECT_TRUE(p.edges[1].empty());
  EXPECT_FALSE(p.accepting[0]);
  EXPECT_FALSE(p.accepting[1]);  // dead, but present before pruning
}

// min_len threads through the product: "A+" with [2,3] accepts at k=2 and
// k=3 but not k=1, even though position 0 is pattern-accepting everywhere.
TEST(LengthThreadedProductTest, MinLenGatesAcceptance) {
  auto p = BuildLengthProduct(MakePlusA(), 2, 3);
  // States: start=(-1,0), (0,1), (0,2), (0,3) discovered in k order.
  ASSERT_EQ(p.edges.size(), 4u);
  EXPECT_FALSE(p.accepting[0]);
  EXPECT_FALSE(p.accepting[1]);
  EXPECT_TRUE(p.accepting[2]);
  EXPECT_TRUE(p.accepting[3]);
  EXPECT_TRUE(p.edges[3].empty());  // k = max_len
}

// A nullable pattern with min_len == 0 accepts at the start state itself.
TEST(LengthThreadedProductTest, NullableStartAccepts) {
  GlushkovAutomaton nfa = MakePlusA();
  nfa.nullable = true;  // i.e. "A*"
  auto p = BuildLengthProduct(nfa, 0, 1);
  EXPECT_TRUE(p.accepting[0]);
  auto p2 = BuildLengthProduct(nfa, 1, 1);
  EXPECT_FALSE(p2.accepting[0]);  // min_len > 0 gates the empty string
}

// Coaccessibility: 0 -> 1 (dead end) and 0 -> 2 -> 3 (accepting). Only the
// path to the accepting state is kept, transitively.
TEST(LengthThreadedCoaccessibleTest, DeadBranchPruned) {
  LengthThreadedProduct p;
  p.edges = {{{"A", 1}, {"B", 2}}, {}, {{"C", 3}}, {}};
  p.accepting = {0, 0, 0, 1};
  auto co = ComputeCoaccessible(p);
  ASSERT_EQ(co.size(), 4u);
  EXPECT_TRUE(co[0]);
  EXPECT_FALSE(co[1]);
  EXPECT_TRUE(co[2]);  // reaches 3 transitively
  EXPECT_TRUE(co[3]);
}

// An accepting state is coaccessible by itself; with no accepting states
// nothing is coaccessible (including states with edges / cycles).
TEST(LengthThreadedCoaccessibleTest, NoAcceptingStates) {
  LengthThreadedProduct p;
  p.edges = {{{"A", 1}}, {{"B", 0}}};  // cycle 0 <-> 1
  p.accepting = {0, 0};
  auto co = ComputeCoaccessible(p);
  EXPECT_FALSE(co[0]);
  EXPECT_FALSE(co[1]);

  p.accepting = {0, 1};
  co = ComputeCoaccessible(p);
  EXPECT_TRUE(co[0]);
  EXPECT_TRUE(co[1]);
}

// Compaction drops dead states and their incoming edges, renumbering densely
// while start stays index 0.
TEST(LengthThreadedCompactTest, RemapsAndDropsDeadEdges) {
  LengthThreadedProduct p;
  p.edges = {{{"A", 1}, {"B", 2}}, {}, {{"C", 3}}, {}};
  p.accepting = {0, 0, 0, 1};
  auto g = CompactProduct(p, ComputeCoaccessible(p));
  EXPECT_FALSE(g.empty);
  ASSERT_EQ(g.states.size(), 3u);  // state 1 dropped; 2 -> 1, 3 -> 2

  ASSERT_EQ(g.states[0].edges.size(), 1u);  // the "A" edge into the dead state is gone
  EXPECT_EQ(g.states[0].edges[0], (std::pair<std::string, int32_t>{"B", 1}));
  ASSERT_EQ(g.states[1].edges.size(), 1u);
  EXPECT_EQ(g.states[1].edges[0], (std::pair<std::string, int32_t>{"C", 2}));
  EXPECT_FALSE(g.states[0].accepting);
  EXPECT_FALSE(g.states[1].accepting);
  EXPECT_TRUE(g.states[2].accepting);
}

// If the start state cannot reach an accepting state, the intersection is
// empty and no states are emitted.
TEST(LengthThreadedCompactTest, EmptyWhenStartDead) {
  LengthThreadedProduct p;
  p.edges = {{{"A", 1}}, {}};
  p.accepting = {0, 0};
  auto g = CompactProduct(p, ComputeCoaccessible(p));
  EXPECT_TRUE(g.empty);
  EXPECT_TRUE(g.states.empty());
}

// The three phases composed equal the public entry point ("AB" here models
// "ab", max_len cuts the second char, so ∩ [0,1] is empty).
TEST(LengthThreadedCompactTest, PhasesComposeLikePublicApi) {
  auto p = BuildLengthProduct(MakeSequenceAB(), 0, 1);
  auto g = CompactProduct(p, ComputeCoaccessible(p));
  EXPECT_TRUE(g.empty);

  auto g2 = RegexToLengthThreadedGrammar("ab", 0, 1);
  EXPECT_TRUE(g2.empty);
}
