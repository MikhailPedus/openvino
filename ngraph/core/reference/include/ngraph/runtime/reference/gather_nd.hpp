//*****************************************************************************
// Copyright 2017-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#pragma once

#include <algorithm>
#include <cassert>
#include <numeric>

#include "ngraph/coordinate_transform.hpp"

namespace ngraph
{
    namespace runtime
    {
        namespace reference
        {
            namespace
            {
                template <bool check>
                using Required = typename std::enable_if<check, bool>::type;

                template <typename It>
                struct IsRandomAccessIt
                {
                    static constexpr bool value =
                        std::is_same<typename It::iterator_category,
                                     std::random_access_iterator_tag>::value;
                };

                template <typename Iterator, Required<IsRandomAccessIt<Iterator>::value> = true>
                class Span
                {
                public:
                    Span(Iterator begin, Iterator end)
                        : m_begin{begin}
                        , m_end{end}
                    {
                    }

                    Iterator begin() const { return m_begin; }
                    Iterator end() const { return m_end; };
                    typename Iterator::value_type operator[](size_t idx) const
                    {
                        return *next(m_begin, idx);
                    }

                    typename Iterator::difference_type size() const
                    {
                        return std::distance(m_begin, m_end);
                    }

                private:
                    Iterator m_begin;
                    Iterator m_end;
                };

                template <typename Iterator>
                Span<Iterator> span(Iterator begin, Iterator end)
                {
                    return Span<Iterator>{begin, end};
                };

                template <typename Iterator>
                std::vector<size_t> get_indices_offsets(const Iterator beg,
                                                        const Iterator end,
                                                        size_t last_slice_size)
                {
                    auto next_e = beg;
                    auto i = std::distance(beg, end);
                    std::vector<size_t> offsets(i + 1, last_slice_size);
                    while (i-- > 0)
                    {
                        offsets[i] = *next_e * offsets[i + 1];
                        ++next_e;
                    }

                    return offsets;
                }
            } // namespace

            ///
            /// Implementation find maximum length of *slice* of input *params* which might be
            /// copied to *out* index by index.
            /// +-------+--------------+-------+
            /// | batch | indices[:-1] | slice |
            /// | shape |   shape      | shape |
            /// +-------+--------------+-------+
            ///
            template <typename T, typename U>
            void gather_nd(const T* const params,
                           const U* const indices,
                           T* const out,
                           const Shape& params_shape,
                           const Shape& indices_shape,
                           const Shape& out_shape,
                           const int batch_dims = 0)
            {
                using std::begin;
                using std::end;
                using std::next;
                using std::prev;
                const auto rbegin = [](const Shape& s) { // generic since C++14
                    return s.rbegin();
                };

                const Shape batch_shape(begin(params_shape), next(begin(params_shape), batch_dims));
                const auto batch_size = shape_size(batch_shape);

                if (batch_dims && batch_size != out_shape.front())
                {
                    throw std::domain_error{
                        "out_shape should have on first dim multiplication of batch number of first"
                        "dimensions of shape "};
                }

                if (!std::equal(begin(params_shape),
                                next(begin(params_shape), batch_dims),
                                begin(indices_shape)))
                {
                    throw std::domain_error{
                        "dimensions in params and indices have to be equal on batch dimensions"};
                }

                const auto first_slice_index_in_params = batch_dims + indices_shape.back();

                if (!(first_slice_index_in_params <= params_shape.size()))
                {
                    throw std::domain_error{
                        "params_shape should have enough rank to be index by indices"};
                }

                const auto slice_shape =
                    span(next(begin(params_shape), first_slice_index_in_params), end(params_shape));
                const auto slice_size = shape_size(slice_shape);

                const auto dims_begin = next(rbegin(params_shape), slice_shape.size());
                const auto dims_end = next(dims_begin, indices_shape.back() - 1);

                const auto indices_offsets = get_indices_offsets(dims_begin, dims_end, slice_size);

                const auto batch_offset = indices_offsets.front() * params_shape[batch_dims];

                const auto k_1_indices =
                    span(next(begin(indices_shape), batch_dims), prev(end(indices_shape)));

                const auto k_1_params =
                    span(next(begin(params_shape), batch_dims), prev(end(params_shape)));

                const auto number_of_slices_to_copy_in_one_batch = shape_size(k_1_indices);

                const auto coordinates_size = indices_shape.back();

                for (size_t batch = 0; batch != batch_size; ++batch)
                {
                    const auto input_batch_offset = batch * batch_offset;
                    const auto output_batch_offset =
                        batch * number_of_slices_to_copy_in_one_batch * slice_size;
                    const auto coordinates_batch_offset =
                        batch * number_of_slices_to_copy_in_one_batch * coordinates_size;
                    for (size_t slice = 0; slice != number_of_slices_to_copy_in_one_batch; ++slice)
                    {
                        const auto slice_coordinates =
                            next(indices, coordinates_batch_offset + slice * coordinates_size);

                        size_t input_slice_offset = input_batch_offset;
                        for (size_t c = 0; c != coordinates_size; ++c)
                        {
                            const auto i_c = slice_coordinates[c];
                            const auto index = i_c < 0 ? k_1_params[c] + i_c : i_c;
                            input_slice_offset += index * indices_offsets[c];
                        }
                        const auto output_slice_offset = output_batch_offset + slice * slice_size;
                        std::copy(next(params, input_slice_offset),
                                  next(params, input_slice_offset + slice_size),
                                  next(out, output_slice_offset));
                    }
                }
            }

        } // namespace reference
    }     // namespace runtime
} // namespace ngraph
