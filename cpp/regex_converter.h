/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/regex_converter.h
 * \brief Convert a regex string to EBNF grammar string.
 */

#ifndef XGRAMMAR_REGEX_CONVERTER_H_
#define XGRAMMAR_REGEX_CONVERTER_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xgrammar {

/*!
 * \brief Convert a regex string to EBNF grammar string.
 */
std::string RegexToEBNF(const std::string& regex, bool with_rule_name = true);

/*!
 * \brief One state of a length-threaded right-linear grammar (code-point layer, no dependency on
 * the grammar builder / EBNFScriptCreator). Produced by RegexToLengthThreadedGrammar.
 *
 * A state corresponds to a pair (Glushkov position, code-points consumed so far). Each edge
 * consumes exactly one code point (matched by \c char_class) and moves to \c target_state. An
 * \c accepting state may additionally match the empty string (the length constraint is satisfied
 * there).
 */
struct LengthThreadedState {
  /*! \brief Edges: (single-code-point EBNF element, index into LengthThreadedGrammar::states). */
  std::vector<std::pair<std::string, int32_t>> edges;
  /*! \brief Whether this state can end the string (emit ""), i.e. minLen <= k <= maxLen at a
   * pattern-accepting position. */
  bool accepting = false;
};

/*!
 * \brief A right-linear grammar realizing L(regex) intersected with [min_len, max_len] code
 * points. \c states[0] is the start rule. All non-coaccessible (dead) states are pruned, so any
 * reachable state can reach an accepting state (avoids empty-mask dead ends).
 */
struct LengthThreadedGrammar {
  std::vector<LengthThreadedState> states;
  /*! \brief The intersection is empty (unsatisfiable): no string of length in [min_len, max_len]
   * matches the pattern. \c states is empty when this is set. */
  bool empty = false;
  /*! \brief No grammar was built and the caller should fall back to the plain pattern
   * (over-accepting). Set when the estimated size exceeds the budget (states
   * (positions + 1) * (max_len + 1) or edges |first| + sum|follow| * max_len over \c max_states)
   * or when the regex cannot be handled (parse failure / unsupported construct). Mutually
   * exclusive with \c empty. */
  bool over_cap = false;
};

/*!
 * \brief The Glushkov position automaton of a regex at the code-point-class level. Position i
 * matches exactly the class \c pos_class[i] (one code point). Input to BuildLengthProduct.
 */
struct GlushkovAutomaton {
  /*! \brief Whether the regex matches the empty string. */
  bool nullable = false;
  /*! \brief Single-code-point EBNF character class matched at each position. */
  std::vector<std::string> pos_class;
  /*! \brief Positions reachable from the virtual start (ascending). */
  std::vector<int32_t> first;
  /*! \brief Positions reachable from position i (ascending). */
  std::vector<std::vector<int32_t>> follow;
  /*! \brief Whether position i can end a match of the regex. */
  std::vector<char> last;
};

/*!
 * \brief Raw product automaton (regex positions x consumed length) before coaccessibility
 * pruning. State 0 is the start state; states are indexed densely.
 */
struct LengthThreadedProduct {
  /*! \brief Per-state edges: (single-code-point EBNF element, target state index). */
  std::vector<std::vector<std::pair<std::string, int32_t>>> edges;
  /*! \brief Per-state: can end the string here (length constraint satisfied at an accepting
   * position). */
  std::vector<char> accepting;
};

/*!
 * \brief Product construction: intersect the Glushkov automaton with the code-point length
 * interval [min_len, max_len] (both >= 0, min_len <= max_len, max_len finite). Reachable states
 * only; dead (non-coaccessible) states may remain.
 */
LengthThreadedProduct BuildLengthProduct(const GlushkovAutomaton& nfa, int min_len, int max_len);

/*!
 * \brief Coaccessible pruning: flag every state of \c product that can reach an accepting state
 * (reverse BFS from accepting states). Returns one flag per state.
 */
std::vector<char> ComputeCoaccessible(const LengthThreadedProduct& product);

/*!
 * \brief Compact \c product to its coaccessible states, renumbering densely (start stays index
 * 0). If the start state is not coaccessible the intersection is empty and \c empty is set.
 */
LengthThreadedGrammar CompactProduct(
    const LengthThreadedProduct& product, const std::vector<char>& coaccessible
);

/*!
 * \brief Build a length-threaded right-linear grammar for \c regex intersected with the code-point
 * length interval [min_len, max_len]. \c max_len must be finite (>= 0); the caller handles the
 * unbounded case. \c max_states is a soft performance guard checked before construction: it caps
 * the state count (positions + 1) * (max_len + 1), the Glushkov follow-set size, and the product
 * edge count |first| + sum|follow| * max_len. Over any of these budgets, returns
 * \c over_cap = true without building.
 *
 * Pipeline: parse regex -> Glushkov automaton -> BuildLengthProduct -> ComputeCoaccessible ->
 * CompactProduct.
 */
LengthThreadedGrammar RegexToLengthThreadedGrammar(
    const std::string& regex, int min_len, int max_len, int max_states = 50000
);

}  // namespace xgrammar

#endif  // XGRAMMAR_REGEX_CONVERTER_H_
