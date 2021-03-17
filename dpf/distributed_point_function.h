/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DISTRIBUTED_POINT_FUNCTIONS_DPF_DISTRIBUTED_POINT_FUNCTION_H_
#define DISTRIBUTED_POINT_FUNCTIONS_DPF_DISTRIBUTED_POINT_FUNCTION_H_

#include <google/protobuf/util/message_differencer.h>
#include <openssl/cipher.h>

#include <memory>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "dpf/distributed_point_function.pb.h"
#include "dpf/internal/pseudorandom_generator.h"

namespace private_statistics {
namespace dpf {

// Implements key generation and evaluation of distributed point functions.
// A distributed point function (DPF) is parameterized by an index `alpha` and a
// value `beta`. The key generation procedure produces two keys `k_a`, `k_b`.
// Evaluating each key on any point `x` in the DPF domain results in an additive
// secret share of `beta`, if `x == alpha`, and a share of 0 otherwise. This
// class also supports *incremental* DPFs that can additionally be evaluated on
// prefixes of points, resulting in different values `beta_i`for each prefix of
// `alpha`.
class DistributedPointFunction {
 public:
  // Creates a new instance of a distributed point function that can be
  // evaluated only at the output layer.
  //
  // Returns INVALID_ARGUMENT if the parameters are invalid.
  static absl::StatusOr<std::unique_ptr<DistributedPointFunction>> Create(
      const DpfParameters& parameters);

  // Creates a new instance of an *incremental* DPF that can be evaluated at
  // multiple layers. Each parameter set in `parameters` should specify the
  // domain size and element size at one of the layers to be evaluated, in
  // increasing domain size order. Element sizes must be non-decreasing.
  //
  // Returns INVALID_ARGUMENT if the parameters are invalid.
  static absl::StatusOr<std::unique_ptr<DistributedPointFunction>>
  CreateIncremental(absl::Span<const DpfParameters> parameters);

  // DistributedPointFunction is neither copyable nor movable.
  DistributedPointFunction(const DistributedPointFunction&) = delete;
  DistributedPointFunction& operator=(const DistributedPointFunction&) = delete;

  // Generates a pair of keys for a DPF that evaluates to `beta` when evaluated
  // `alpha`. Returns INVALID_ARGUMENT if used on an incremental DPF with more
  // than one set of parameters, or if `alpha` or `beta` are outside of the
  // domains specified at construction.
  absl::StatusOr<std::pair<DpfKey, DpfKey>> GenerateKeys(
      absl::uint128 alpha, absl::uint128 beta) const;

  // Generates a pair of keys for an incremental DPF. For each parameter i
  // passed at construction, the DPF evaluates to `beta[i]` at the first
  // `parameters_[i].log_domain_size()` bits of `alpha`.
  // Returns INVALID_ARGUMENT if `beta.size() != parameters_.size()` or if
  // `alpha` or any element of `beta` are outside of the domains specified at
  // construction.
  absl::StatusOr<std::pair<DpfKey, DpfKey>> GenerateKeysIncremental(
      absl::uint128 alpha, absl::Span<const absl::uint128> beta) const;

  // Returns an `EvaluationContext` for incrementally evaluating the given
  // DpfKey.
  //
  // Returns INVALID_ARGUMENT if `key` doesn't match the parameters given at
  // construction.
  absl::StatusOr<EvaluationContext> CreateEvaluationContext(DpfKey key);

  // Evaluates the next hierarchy level of the DPF under all `prefixes` passed
  // to this function. Each element of `prefixes` must fit in the previous
  // hierarchy level's domain size. On the first call, `prefixes` must be empty.
  // On subsequent calls, `prefixes` may only contain extensions of the prefixes
  // passed in the previous call. For example, in the following sequence of
  // calls, for each element p2 of `prefixes2`, there must be an element p1 of
  // `prefixes1` such that p1 is a prefix of p2:
  //
  //   DPF_ASSIGN_OR_RETURN(std::unique_ptr<EvaluationContext> ctx,
  //                        dpf->CreateEvaluationContext(key));
  //   std::vector<absl::uint128> prefixes1 = ...;
  //   using T1 = ...;
  //   DPF_ASSIGN_OR_RETURN(std::vector<T1> evaluations1,
  //                        dpf->EvaluateNext(prefixes1, *ctx));
  //   ...
  //   std::vector<absl::uint128> prefixes2 = ...;
  //   using T2 = ...;
  //   DPF_ASSIGN_OR_RETURN(std::vector<T2> evaluations2,
  //                        dpf->EvaluateNext(prefixes2, *ctx));
  //
  // The prefixes are read from the lowest-order bits of the corresponding
  // absl::uint128. The number of bits used for each prefix depends on the
  // output domain size of the previous hierarchy level. For example, if `ctx`
  // was last evaluated on a hierarchy level with output domain size 2**20, then
  // the 20 lowest-order bits of each element in `prefixes` are used.
  //
  // Returns `INVALID_ARGUMENT` if
  //   - any element of `prefixes` is larger than the next hierarchy level's
  //     log_domain_size,
  //   - `prefixes` contains elements that are not extensions of previous
  //     prefixes, or
  //   - the bit-size of T doesn't match the next hierarchy level's
  //     element_bitsize.
  template <typename T>
  absl::StatusOr<std::vector<T>> EvaluateNext(
      absl::Span<const absl::uint128> prefixes, EvaluationContext& ctx) const;

 private:
  // Private constructor, called by `CreateIncremental`.
  DistributedPointFunction(std::vector<DpfParameters> parameters,
                           int tree_levels_needed,
                           absl::flat_hash_map<int, int> tree_to_hierarchy,
                           std::vector<int> hierarchy_to_tree,
                           dpf_internal::PseudorandomGenerator prg_left,
                           dpf_internal::PseudorandomGenerator prg_right,
                           dpf_internal::PseudorandomGenerator prg_value);

  // Computes the value correction for the given `tree_level`, `seeds`, index
  // `alpha` and value `beta`. If `invert` is true, the individual values in the
  // returned block are multiplied element-wise by -1. Expands `seeds` using
  // `cipher`, then calls ComputeValueCorrectionFor<T> for the right type
  // depending on `element_bitsize`. Returns INTERNAL in case the PRG expansion
  // fails, and UNIMPLEMENTED if `element_bitsize` is not supported.
  absl::StatusOr<absl::uint128> ComputeValueCorrection(
      int tree_level, int element_bitsize,
      absl::Span<const absl::uint128> seeds, absl::uint128 alpha,
      absl::uint128 beta, bool invert) const;

  // Expands the PRG seeds at the next `tree_level` for an incremental DPF with
  // index `alpha` and values `beta`, updates `seeds` and `control_bits`, and
  // writes the next correction word to `keys`. Called from
  // `GenerateKeysIncremental`.
  absl::Status GenerateNext(int tree_level, absl::uint128 alpha,
                            absl::Span<const absl::uint128> beta,
                            absl::Span<absl::uint128> seeds,
                            absl::Span<bool> control_bits,
                            absl::Span<DpfKey> keys) const;

  // Checks if the parameters of `ctx` are compatible with this DPF. Returns OK
  // if that's the case, and INVALID_ARGUMENT otherwise.
  absl::Status CheckContextParameters(const EvaluationContext& ctx) const;

  // Seeds and control bits resulting from a DPF expansion. This type is
  // returned by `ExpandSeeds` and `ExpandAndUpdateContext`.
  struct DpfExpansion {
    std::vector<absl::uint128> seeds;
    // Faster than std::vector<bool>.
    absl::InlinedVector<bool, 256> control_bits;
  };

  // Performs DPF expansion of the given `partial_evaluations` using
  // prg_ctx_left_ and prg_ctx_right_, and the given `correction_words`.
  // In more detail, each of the partial evaluations is subjected to a full
  // subtree expansion of correction_words.size() levels, and the concatenated
  // result is provided in the response. The result contains
  // (partial_evaluations.size() * (2^correction_words.size()) evaluations in a
  // single `DpfExpansion`.
  //
  // Returns INTERNAL in case of OpenSSL errors.
  absl::StatusOr<DpfExpansion> ExpandSeeds(
      const DpfExpansion& partial_evaluations,
      absl::Span<const CorrectionWord* const> correction_words) const;

  // Extracts the seeds for the given `prefixes` from `ctx` and expands them as
  // far as needed for the next hierarchy level. Returns the result as a
  // `DpfExpansion`. Called by `EvaluateNext`, where the expanded seeds are
  // corrected to obtain output values.
  // After expansion, `ctx.hierarchy_level()` is increased. If this isn't the
  // last expansion, the expanded seeds are also saved in `ctx` for the next
  // expansion.
  //
  // Returns INVALID_ARGUMENT if any element of `prefixes` is not found in
  // `ctx.partial_evaluations()`.
  absl::StatusOr<DpfExpansion> ExpandAndUpdateContext(
      absl::Span<const absl::uint128> prefixes, EvaluationContext& ctx) const;

  // DP parameters passed to the factory function. Contains the domain size and
  // element size for hierarchy level of the incremental DPF.
  const std::vector<DpfParameters> parameters_;

  // Number of levels in the evaluation tree. This is always less than or equal
  // to the largest log_domain_size in parameters_.
  const int tree_levels_needed_;

  // Maps levels of the FSS evaluation tree to hierarchy levels (i.e., elements
  // of parameters_).
  const absl::flat_hash_map<int, int> tree_to_hierarchy_;

  // The inverse of tree_to_hierarchy_.
  const std::vector<int> hierarchy_to_tree_;

  // Pseudorandom generators for seed expansion (left and right), and value
  // correction.
  const dpf_internal::PseudorandomGenerator prg_left_;
  const dpf_internal::PseudorandomGenerator prg_right_;
  const dpf_internal::PseudorandomGenerator prg_value_;
};

}  // namespace dpf
}  // namespace private_statistics

#endif  // DISTRIBUTED_POINT_FUNCTIONS_DPF_DISTRIBUTED_POINT_FUNCTION_H_
