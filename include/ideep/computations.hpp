/*
 *Copyright (c) 2018 Intel Corporation.
 *
 *Permission is hereby granted, free of charge, to any person obtaining a copy
 *of this software and associated documentation files (the "Software"), to deal
 *in the Software without restriction, including without limitation the rights
 *to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *copies of the Software, and to permit persons to whom the Software is
 *furnished to do so, subject to the following conditions:
 *
 *The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 *THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *THE SOFTWARE.
 *
 */


#ifndef IDEEP_HPP
#define IDEEP_HPP

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#include <assert.h>
#include <stdlib.h>
#include <algorithm>
#include <memory>
#include <vector>
#include <iterator>
#include <string>
#include <cstring>
#include <numeric>
#include <functional>
#include <iostream>
#include <immintrin.h>

#include <ideep/abstract_types.hpp>
#include <ideep/fast_math.hpp>
#include <ideep/tensor.hpp>
#include <ideep/lru_cache.hpp>
#include <ideep/scope_guard.hpp>
#endif

namespace ideep {

struct reorder: public c_wrapper<mkldnn_primitive_t>,
  public utils::computation_cache<reorder> {
  struct descriptor : public c_wrapper<mkldnn_primitive_desc_t> {
    descriptor(const c_wrapper<mkldnn_primitive_desc_t> &input
        , const tensor::descriptor &output) {
      // TODO: check to make sure primitive_desc is memory/view
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_reorder_primitive_desc_create(
            &result, input.get(), output.get()),
          "could not create a reorder primitive descriptor");
      reset(result);
    }
  };

  reorder() = default;

  void init(const tensor::descriptor& src_desc,
      const tensor::descriptor& dst_desc) {
    mkldnn_primitive_desc_t desc;
    error::wrap_c_api(mkldnn_reorder_primitive_desc_create(
          &desc, src_desc.get(), dst_desc.get()),
        "could not create a reorder primitive descriptor");
    in.init(src_desc, invalid_buffer);
    out.init(dst_desc, invalid_buffer);

    mkldnn_primitive_t result;
    mkldnn_primitive_at_t inputs[] = { {in.get(), 0} };
    const_mkldnn_primitive_t outputs[] = { out.get() };
    error::wrap_c_api(mkldnn_primitive_create(&result, desc, inputs, outputs),
        "could not create a reorder primitive");
    reset(result);
  }

  void init(const tensor::view& view, const tensor::descriptor& src_desc,
      const tensor::descriptor& dst_desc) {
    mkldnn_primitive_desc_t desc;
    error::wrap_c_api(mkldnn_reorder_primitive_desc_create(
          &desc, view.get(), dst_desc.get()),
        "could not create a reorder primitive descriptor");
    in.init(src_desc, invalid_buffer);
    out.init(dst_desc, invalid_buffer);

    mkldnn_primitive_t result;
    mkldnn_primitive_at_t inputs[] = { {in.get(), 0} };
    const_mkldnn_primitive_t outputs[] = { out.get() };
    error::wrap_c_api(mkldnn_primitive_create(&result, desc, inputs, outputs),
        "could not create a reorder primitive");
    reset(result);
  }

  void init(const tensor::descriptor& src_desc, const tensor::view& view,
      const tensor::descriptor& dst_desc) {
    mkldnn_primitive_desc_t desc;
    error::wrap_c_api(mkldnn_reorder_primitive_desc_create(
          &desc, src_desc.get(), view.get()),
        "could not create a reorder primitive descriptor");
    in.init(src_desc, invalid_buffer);
    out.init(dst_desc, invalid_buffer);

    mkldnn_primitive_t result;
    mkldnn_primitive_at_t inputs[] = { {in.get(), 0} };
    const_mkldnn_primitive_t outputs[] = { out.get() };
    error::wrap_c_api(mkldnn_primitive_create(&result, desc, inputs, outputs),
        "could not create a reorder primitive");
    reset(result);
  }

  template<typename... Ts>
  reorder(Ts&&... args) {
    init(std::forward<Ts>(args)...);
  }

  void operator() (const tensor &input, const tensor &output) {
    assert(input.get_descriptor() == in.get_descriptor()
        && output.get_descriptor() == out.get_descriptor());
    in.set_data_handle(input.get_data_handle());
    out.set_data_handle(output.get_data_handle());

    std::vector<mkldnn_primitive_t> execution_sequence = {get()};
    mkldnn_primitive_t c_api_error_primitive;

    error::wrap_c_api(
        mkldnn_stream_submit(stream::default_stream().get(),
          execution_sequence.size(), &execution_sequence[0],
          &c_api_error_primitive),
        "could not execute reorder", &c_api_error_primitive);
  }

  static const tensor::descriptor compute(
      const tensor& input, const tensor& output) {
    const auto input_desc = input.get_descriptor();
    const auto output_desc = output.get_descriptor();

    auto key = utils::create_key(input.get_dims(), input.get_data_type(),
        input.get_internal_format(), output.get_dims(), output.get_data_type(),
        output.get_internal_format());

    auto op = fetch_or_create(key, input_desc, output_desc);
    auto sg = utils::make_guard([&key, &op]() {
        release(key, std::move(op));
        });

    op(input, output);

    return output_desc;
  }

  // static void split(const tensor& input, const tensor::view& subregion,
  //     const tensor output) {
  // }
protected:
  param in, out;
};

using direct_copy = reorder;
using spliter = reorder;

template<>
inline tensor::data_type tensor::descriptor::type_to_id<float>() {
  return tensor::data_type::f32;
}

template<>
inline tensor::data_type tensor::descriptor::type_to_id<int>() {
  return tensor::data_type::s32;
}

template<>
inline tensor::data_type tensor::descriptor::type_to_id<unsigned char>() {
  return tensor::data_type::u8;
}

template<>
inline tensor::data_type tensor::descriptor::type_to_id<signed char>() {
  return tensor::data_type::s8;
}

/// Descriptor group, create relative descriptors all in one
class descriptor_group: public c_wrapper_complex<mkldnn_primitive_desc_t> {
  friend class primitive_group;

protected:
  std::vector<const_mkldnn_primitive_desc_t> cpp_to_c(
      const std::vector<tensor::descriptor> &inputs) {
    std::vector<const_mkldnn_primitive_desc_t> c_api_inputs;
    c_api_inputs.reserve(inputs.size());

    auto convert_to_c = [](const tensor::descriptor &d) {
      return d.get();
    };

    std::transform(inputs.begin(), inputs.end(),
        std::back_inserter(c_api_inputs), convert_to_c);

    return c_api_inputs;
  }

public:
  descriptor_group()
    : c_wrapper_complex() {}

  tensor::descriptor expected_descriptor_of(mkldnn::query q
      , int index = 0) const {
    mkldnn_primitive_desc_t cdesc;
    const_mkldnn_primitive_desc_t const_cdesc =
        mkldnn_primitive_desc_query_pd(get()
            , mkldnn::convert_to_c(q), index);
    error::wrap_c_api(mkldnn_primitive_desc_clone(&cdesc
          , const_cdesc)
        , "could not clone a src primititve descriptor");
    return param::descriptor(cdesc);
  }

  tensor::descriptor expected_input_descriptor(int index) const {
    return expected_descriptor_of(mkldnn::input_pd, index);
  }

  tensor::descriptor expected_output_descriptor(int index) const {
    return expected_descriptor_of(mkldnn::output_pd, index);
  }

  tensor::descriptor expected_src_descriptor() const {
    return expected_descriptor_of(mkldnn::src_pd);
  }

  tensor::descriptor expected_weights_descriptor() const {
    return expected_descriptor_of(mkldnn::weights_pd);
  }

  tensor::descriptor expected_bias_descriptor() const {
    return expected_descriptor_of(mkldnn::weights_pd, 1);
  }

  tensor::descriptor expected_dst_descriptor() const {
    return expected_descriptor_of(mkldnn::dst_pd, 0);
  }

  tensor::descriptor expected_workspace_descriptor() const {
    return expected_descriptor_of(mkldnn::workspace_pd, 0);
  }

  tensor::descriptor expected_gradx_descriptor() const {
    return expected_descriptor_of(mkldnn::diff_src_pd, 0);
  }

  tensor::descriptor expected_grady_descriptor() const {
    return expected_descriptor_of(mkldnn::diff_dst_pd, 0);
  }

  tensor::descriptor expected_gradw_descriptor() const {
    return expected_descriptor_of(mkldnn::diff_weights_pd, 0);
  }

  tensor::descriptor expected_gradb_descriptor() const {
    return expected_descriptor_of(mkldnn::diff_weights_pd, 1);
  }

  int num_of_inputs() const {
      return mkldnn_primitive_desc_query_s32(get()
          , mkldnn::convert_to_c(mkldnn::num_of_inputs_s32), 0);
  }

  int num_of_outputs() const {
      return mkldnn_primitive_desc_query_s32(get()
          , mkldnn::convert_to_c(mkldnn::num_of_outputs_s32), 0);
  }

protected:
  void create_reorder_pds(std::vector<tensor::descriptor> descriptors) {
    for (unsigned i = 0; i < descriptors.size(); i ++) {
      assert((int)i < num_of_inputs());
      auto &provided = descriptors[i];
      auto expected = expected_input_descriptor((int)i);
      if (expected != provided) {
        mkldnn_primitive_desc_t result;
        error::wrap_c_api(mkldnn_reorder_primitive_desc_create(
              &result, provided.get(), expected.get()),
            "could not create reorder primitive descriptor");
        auxiliaries_[i].reset(result);
      }
    }
  }
};

class primitive_group: public c_wrapper_complex<mkldnn_primitive_t> {
public:
  primitive_group()
    : c_wrapper_complex() {}

  /// Returns the internal structure of primitive descriptor.
  const_mkldnn_primitive_desc_t get_mkldnn_primitive_desc_t() const {
    const_mkldnn_primitive_desc_t cdesc;
    error::wrap_c_api(mkldnn_primitive_get_primitive_desc(get(),
                &cdesc),
            "could not get primitive descriptor from a memory primitive");
    return cdesc;
  }

  tensor::descriptor expected_descriptor_of(mkldnn::query q,
      int index = 0) const {
    mkldnn_primitive_desc_t cdesc;
    const_mkldnn_primitive_desc_t const_cdesc =
        mkldnn_primitive_desc_query_pd(get_mkldnn_primitive_desc_t(),
            mkldnn::convert_to_c(q), index);
    error::wrap_c_api(mkldnn_primitive_desc_clone(&cdesc
          , const_cdesc)
        , "could not clone a src primititve descriptor");
    return tensor::descriptor(cdesc);
  }

protected:
  void create_reorder_for(unsigned index
      , const descriptor_group &g, param& in, param& out) {
    mkldnn_primitive_t result;
    mkldnn_primitive_at_t inputs[] = { {in.get(), 0} };
    const_mkldnn_primitive_t outputs[] = { out.get() };

    error::wrap_c_api(mkldnn_primitive_create(&result
          , g.auxiliaries_[index].get(), inputs, outputs),
        "could not create a reorder");

    auxiliaries_[index].reset(result);
  }

  tensor::descriptor expected_input_descriptor(int index) const {
    return expected_descriptor_of(mkldnn::input_pd, index);
  }

  tensor::descriptor expected_output_descriptor(int index) const {
    return expected_descriptor_of(mkldnn::output_pd, index);
  }

  tensor::descriptor expected_src_descriptor() const {
    return expected_descriptor_of(mkldnn::src_pd);
  }

  tensor::descriptor expected_weights_descriptor() const {
    return expected_descriptor_of(mkldnn::weights_pd);
  }

  tensor::descriptor expected_bias_descriptor() const {
    return expected_descriptor_of(mkldnn::weights_pd, 1);
  }

  tensor::descriptor expected_dst_descriptor() const {
    return expected_descriptor_of(mkldnn::dst_pd, 0);
  }

  tensor::descriptor expected_workspace_descriptor() const {
    return expected_descriptor_of(mkldnn::workspace_pd, 0);
  }

  tensor::descriptor expected_gradx_descriptor() const {
    return expected_descriptor_of(mkldnn::diff_src_pd, 0);
  }

  tensor::descriptor expected_grady_descriptor() const {
    return expected_descriptor_of(mkldnn::diff_dst_pd, 0);
  }

  tensor::descriptor expected_gradw_descriptor() const {
    return expected_descriptor_of(mkldnn::diff_weights_pd, 0);
  }

  tensor::descriptor expected_gradb_descriptor() const {
    return expected_descriptor_of(mkldnn::diff_weights_pd, 1);
  }

  void execute(stream &parallel_control) {
    std::vector<mkldnn_primitive_t> execution_sequence;
    mkldnn_primitive_t c_api_error_primitive;

    // TODO: varadic needed
    if (need_reorder_input(0))
      execution_sequence.push_back(auxiliaries_[0].get());

    if (need_reorder_input(1))
      execution_sequence.push_back(auxiliaries_[1].get());

    // Operator
    execution_sequence.push_back(get());

    // if (need_reorder_input(3))
    //   execution_sequence.push_back(auxiliaries_[3].get());

    error::wrap_c_api(
        mkldnn_stream_submit(parallel_control.get()
          , execution_sequence.size(), &execution_sequence[0]
          , &c_api_error_primitive)
        , "could not execute the computation"
        , &c_api_error_primitive);
  }
};

struct computation : public primitive_group {
  computation() = default;

  void connect_reorder_for(const descriptor_group& adesc,
      const std::vector<tensor::descriptor>& args) {
    for (int i = 0; (unsigned)i < args.size(); i ++) {
      connect_reorder_for(i, adesc, args[(unsigned)i]);
    }
  }

  template <typename... Ts>
  void connect_reorder_for(int index, const descriptor_group &adesc,
      const tensor::descriptor& first, const Ts&... rest) {
    connect_reorder_for(index, adesc, first);
    connect_reorder_for(index + 1, adesc, rest...);
  }

  void connect_reorder_for(int index, const descriptor_group &adesc,
      const tensor::descriptor &desc) {
    if (adesc.need_reorder_input(index)) {
      inouts_[index] = param { desc, invalid_buffer };
      create_reorder_for(
          (unsigned)index, adesc, inouts_[(unsigned)index],
          primitive_inputs_[(unsigned)index]);
    }
  }

  inline void init_internal(
      const descriptor_group &adesc, int n_inputs, int n_outputs) {
    // init contents
    primitive_inputs_ = std::vector<param>((unsigned)n_inputs);
    inouts_ = std::vector<param>((unsigned)(n_inputs + n_outputs));

    mkldnn_primitive_at_t inputs[n_inputs];
    for (int i =0; i < n_inputs; i ++) {
      primitive_inputs_[i] = {
        adesc.expected_input_descriptor(i), invalid_buffer };
      // connect real inputs and primitive inputs
      inouts_[i] = primitive_inputs_[i];
      inputs[i] = { primitive_inputs_[i].get(), 0 };
    }

    const_mkldnn_primitive_t outputs[n_outputs];
    for (int i = 0; i < n_outputs; i ++) {
      inouts_[i + n_inputs] = {
        adesc.expected_output_descriptor(i), invalid_buffer };
      outputs[i] = inouts_[i + n_inputs].get();
    }

    mkldnn_primitive_t result;
    error::wrap_c_api(mkldnn_primitive_create(&result,
          adesc.get(), inputs, outputs),
        "could not create a computation primitive");

    reset(result);
  }

  void init(const descriptor_group& adesc,
      const std::vector<tensor::descriptor> &args) {
    assert(adesc.num_of_inputs() == (int)args.size());
    auto n_inputs = (int)args.size();
    auto n_outputs = adesc.num_of_outputs();
    init_internal(adesc, n_inputs, n_outputs);
    connect_reorder_for(adesc, args);
  }

  template <typename... Ts>
  void init(const descriptor_group &adesc, const Ts&... args) {
    auto n_inputs = adesc.num_of_inputs();
    auto n_outputs = adesc.num_of_outputs();
    init_internal(adesc, n_inputs, n_outputs);
    connect_reorder_for(0, adesc, args...);
  }

  void connect_handle_for(int index, const param& atensor) {
    if ((unsigned)index < primitive_inputs_.size() &&
        inouts_[index] != primitive_inputs_[index]) {
      // Connect inputs
      if (inouts_.at((unsigned)index).get_descriptor()
          == atensor.get_descriptor()) {
        inouts_[(unsigned)index].set_data_handle(atensor.get_data_handle());
        primitive_inputs_[(unsigned)index].materialize();
      } else if(primitive_inputs_.at((unsigned)index).get_descriptor()
          == atensor.get_descriptor()) {
        // Destructional move, assume we never change back
        primitive_inputs_[(unsigned)index].dematerialize();
        primitive_inputs_[(unsigned)index].set_data_handle(
            atensor.get_data_handle());

        // We throw the reorder away.
        auxiliaries_[index].reset(nullptr);
      } else
        throw error(mkldnn_runtime_error, "Cannot accept incompatible input");
    } else {
      // Connect outputs
      assert(inouts_.at((unsigned)index).get_descriptor()
          == atensor.get_descriptor());
      inouts_.at((unsigned)index).set_data_handle(atensor.get_data_handle());
    }
  }

  void connect_handle_for(const std::vector<tensor>& inputs,
      const param& output) {
    int i = 0;
    for(; (unsigned)i < inputs.size(); i++){
      connect_handle_for(i, inputs[(unsigned)i]);
    }
    connect_handle_for(i, output);
  }

  template <typename ...Params>
  void connect_handle_for(int index, const param& first,
      const Params&... rest) {
    connect_handle_for(index, first);
    connect_handle_for(index + 1, rest...);
  }

  void execute (const std::vector<tensor>& inputs, const tensor& outputs) {
    connect_handle_for(inputs, outputs);
    stream parallel_control = stream::default_stream();
    primitive_group::execute(parallel_control);
  }

  template<typename ...Params>
  void execute(const param& arg0, const Params&... args) {
    connect_handle_for(0, arg0, args...);
    stream parallel_control = stream::default_stream();
    primitive_group::execute(parallel_control);
  }

  int num_of_inputs() const {
    return primitive_inputs_.size();
  }

  int num_of_outputs() const {
    return inouts_.size() - primitive_inputs_.size();
  }

private:
  // outputs after inputs
  std::vector<param> inouts_;
  std::vector<param> primitive_inputs_;
};

struct convolution_forward: public computation,
  public utils::computation_cache<convolution_forward> {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &src_desc,
        const tensor::descriptor &weights_desc,
        const tensor::descriptor &bias_desc,
        const tensor::descriptor &dst_desc,
        const tensor::dims strides,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        prop_kind aprop_kind = prop_kind::forward,
        const padding_kind apadding_kind = padding_kind::zero) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_convolution_desc_t data;
      mkldnn_memory_desc_t src_data = src_desc.format_any();
      mkldnn_memory_desc_t weights_data = weights_desc.format_any();
      mkldnn_memory_desc_t bias_data = bias_desc.format_any();
      mkldnn_memory_desc_t dst_data = dst_desc.format_any();

      error::wrap_c_api(mkldnn_convolution_forward_desc_init(&data,
                  mkldnn::convert_to_c(aprop_kind),
                  convert_to_c(aalgorithm),
                  &src_data, &weights_data, &bias_data,
                  &dst_data, &strides[0], &padding_l[0],
                  &padding_r[0],
                  mkldnn::convert_to_c(apadding_kind)),
              "could not create a convolution forward descriptor");

      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), nullptr)
          , "could not create a convolution forward primitive descriptor");

      reset(result);
      create_reorder_pds({src_desc, weights_desc});
    }
    descriptor(const tensor::descriptor &src_desc,
        const tensor::descriptor &weights_desc,
        const tensor::descriptor &dst_desc,
        const tensor::dims strides,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        prop_kind aprop_kind = prop_kind::forward,
        const padding_kind apadding_kind = padding_kind::zero) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_convolution_desc_t data;
      mkldnn_memory_desc_t src_data = src_desc.format_any();
      mkldnn_memory_desc_t weights_data = weights_desc.format_any();
      mkldnn_memory_desc_t dst_data = dst_desc.format_any();

      error::wrap_c_api(mkldnn_convolution_forward_desc_init(&data,
                  mkldnn::convert_to_c(aprop_kind),
                  convert_to_c(aalgorithm),
                  &src_data, &weights_data, nullptr,
                  &dst_data, &strides[0], &padding_l[0],
                  &padding_r[0],
                  mkldnn::convert_to_c(apadding_kind)),
              "could not create a convolution forward descriptor");

      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), nullptr),
            "could not create a convolution forward primitive descriptor");

      reset(result);
      create_reorder_pds({src_desc, weights_desc});
    }
    descriptor(const tensor::descriptor &src_desc,
        const tensor::descriptor &weights_desc,
        const tensor::descriptor &bias_desc,
        const tensor::descriptor &dst_desc,
        const tensor::dims strides,
        const tensor::dims dilates,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        prop_kind aprop_kind = prop_kind::forward,
        const padding_kind apadding_kind = padding_kind::zero) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(dilates);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_convolution_desc_t data;
      mkldnn_memory_desc_t src_data = src_desc.format_any();
      mkldnn_memory_desc_t weights_data = weights_desc.format_any();
      mkldnn_memory_desc_t bias_data = bias_desc.format_any();
      mkldnn_memory_desc_t dst_data = dst_desc.format_any();
      error::wrap_c_api(
          mkldnn_dilated_convolution_forward_desc_init(&data,
              mkldnn::convert_to_c(aprop_kind), convert_to_c(aalgorithm),
                  &src_data, &weights_data, &bias_data,
                  &dst_data, &strides[0], &dilates[0],
                  &padding_l[0], &padding_r[0],
                  mkldnn::convert_to_c(apadding_kind)),
              "could not create a dilated convolution forward descriptor");

      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
        &result, &data, engine::cpu_engine().get(), nullptr),
          "could not create a convolution forward primitive descriptor");
      reset(result);
      create_reorder_pds({src_desc, weights_desc});
    }
    descriptor(const tensor::descriptor &src_desc,
        const tensor::descriptor &weights_desc,
        const tensor::descriptor &dst_desc,
        const tensor::dims strides,
        const tensor::dims dilates,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        prop_kind aprop_kind = prop_kind::forward,
        const padding_kind apadding_kind = padding_kind::zero) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(dilates);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_convolution_desc_t data;
      mkldnn_memory_desc_t src_data = src_desc.format_any();
      mkldnn_memory_desc_t weights_data = weights_desc.format_any();
      mkldnn_memory_desc_t dst_data = dst_desc.format_any();
      error::wrap_c_api(
        mkldnn_dilated_convolution_forward_desc_init(&data,
            mkldnn::convert_to_c(aprop_kind), convert_to_c(aalgorithm),
                &src_data, &weights_data, nullptr,
                &dst_data, &strides[0], &dilates[0],
                &padding_l[0], &padding_r[0],
                mkldnn::convert_to_c(apadding_kind)),
            "could not create a dilated convolution forward descriptor");

      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
        &result, &data, engine::cpu_engine().get(), nullptr),
        "could not create a convolution forward primitive descriptor");

      reset(result);
      create_reorder_pds({src_desc, weights_desc});
    }
  };

 public:
  using computation::expected_input_descriptor;
  using computation::expected_dst_descriptor;
  using computation::expected_weights_descriptor;

  template <typename T, typename ...Ts,
           typename = typename std::enable_if<
             std::is_same<T, tensor::descriptor>::value>::type>
  void init(const tensor::descriptor &src_desc,
      const tensor::descriptor &weights_desc,
      const tensor::descriptor &bias, const T &dst, Ts&&... args) {
    descriptor forward_descriptor(
        src_desc, weights_desc, bias, dst, std::forward<Ts>(args)...);

    computation::init(forward_descriptor, src_desc, weights_desc, bias);
  }

  template <typename T, typename ...Ts,
           typename  = typename std::enable_if<
             std::is_same<T, tensor::dims>::value>::type>
  void init(const tensor::descriptor &src_desc,
      const tensor::descriptor &weights_desc,
      const tensor::descriptor &dst, const T something,
      Ts&&... args) {
    descriptor forward_descriptor(src_desc, weights_desc, dst,
        something, std::forward<Ts>(args)...);

    computation::init(forward_descriptor, src_desc, weights_desc);
  }

  convolution_forward() = default;

  template <typename T, typename ...Ts>
  convolution_forward(T arg, Ts&&... args) {
    init(arg, std::forward<Ts>(args)...);
  }

  void execute(const tensor& src, const tensor& weights, const tensor& dst) {
    computation::execute(src, weights, dst);
  }

  void execute(const tensor& src, const tensor& weights, const tensor& bias,
      const tensor& dst) {
    computation::execute(src, weights, bias, dst);
  }

  template <typename ...Ts>
  static tensor::descriptor compute_impl(const tensor& src,
      const tensor& weights, const tensor& bias,
      const tensor::dims& result_dims, void *result, Ts&&... args) {
    tensor::descriptor result_desc(result_dims, src.get_data_type());
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        weights.get_dims(), bias.get_dims(), result_dims, args...);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        weights.get_descriptor(), bias.get_descriptor(),
        result_desc, std::forward<Ts>(args)...);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    // XXX: Performance evaluation
    // TODO: Custom allocator support
    auto src_in = src;
    auto weights_in = weights;
    if (src.get_descriptor() != comp.expected_src_descriptor()) {
      src_in.init(comp.expected_src_descriptor());
      reorder::compute(src, src_in);
    }
    if (weights.get_descriptor() != comp.expected_weights_descriptor()) {
      weights_in.init(comp.expected_weights_descriptor());
      reorder::compute(weights, weights_in);
    }

    tensor dst(comp.expected_dst_descriptor(), result);
    comp.execute(src_in, weights_in, bias, dst);
    return comp.expected_dst_descriptor();
  }

  template <typename ...Ts>
  static tensor::descriptor compute_impl(const tensor& src,
      const tensor& weights, const tensor::dims& result_dims,
      void *result, Ts&&... args) {
    tensor::descriptor result_desc(result_dims, src.get_data_type());
    std::string key = utils::to_string(src.get_data_type(), src.get_dims(),
        weights.get_dims(), result_dims, args...);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        weights.get_descriptor(), result_desc, std::forward<Ts>(args)...);
    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    // Performance evaluation
    auto src_in = src;
    auto weights_in = weights;
    if (src.get_descriptor() != comp.expected_src_descriptor()) {
      src_in.init(comp.expected_src_descriptor());
      reorder::compute(src, src_in);
    }
    if (weights.get_descriptor() != comp.expected_weights_descriptor()) {
      weights_in.init(comp.expected_weights_descriptor());
      reorder::compute(weights, weights_in);
    }

    tensor dst(comp.expected_dst_descriptor(), result);
    comp.execute(src_in, weights_in, dst);
    return comp.expected_dst_descriptor();
  }

  static tensor::descriptor compute(const tensor &src, const tensor& weights,
      const tensor::dims result_dims, void *result, const tensor::dims strides,
      const tensor::dims dilateds, const tensor::dims padding_l,
      const tensor::dims padding_r,
      algorithm aalogorithm = algorithm::convolution_direct,
      prop_kind aprop_kind = prop_kind::forward,
      const padding_kind appading_kind = padding_kind::zero) {
    return compute_impl(src, weights, result_dims, result, strides, dilateds,
        padding_l, padding_r, aalogorithm, aprop_kind, appading_kind);
  }

  static tensor::descriptor compute(const tensor &src, const tensor& weights,
      const tensor& bias, const tensor::dims result_dims,
      void *result, const tensor::dims strides,
      const tensor::dims dilateds, const tensor::dims padding_l,
      const tensor::dims padding_r,
      algorithm aalogorithm = algorithm::convolution_direct,
      prop_kind aprop_kind = prop_kind::forward,
      const padding_kind appading_kind = padding_kind::zero) {
    return compute_impl(src, weights, bias, result_dims, result, strides,
        dilateds, padding_l, padding_r, aalogorithm, aprop_kind, appading_kind);
  }

  static tensor::descriptor compute(const tensor &src, const tensor& weights,
      const tensor::dims result_dims, void *result, const tensor::dims strides,
      const tensor::dims padding_l, const tensor::dims padding_r,
      algorithm aalogorithm = algorithm::convolution_direct,
      prop_kind aprop_kind = prop_kind::forward,
      const padding_kind appading_kind = padding_kind::zero) {
    return compute_impl(src, weights, result_dims, result, strides, padding_l,
        padding_r, aalogorithm, aprop_kind, appading_kind);
  }

  static tensor::descriptor compute(const tensor &src, const tensor& weights,
      const tensor& bias, const tensor::dims result_dims, void *result,
      const tensor::dims strides, const tensor::dims padding_l,
      const tensor::dims padding_r,
      algorithm aalogorithm = algorithm::convolution_direct,
      prop_kind aprop_kind = prop_kind::forward,
      const padding_kind appading_kind = padding_kind::zero) {
    return compute_impl(src, weights, bias, result_dims, result, strides,
        padding_l, padding_r, aalogorithm, aprop_kind, appading_kind);
  }
};

struct convolution_backward_data : public computation,
  public utils::computation_cache<convolution_backward_data> {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &grady_desc,
        const tensor::descriptor &weights_desc,
        const tensor::descriptor &gradx_desc,
        const tensor::dims strides,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        const padding_kind apadding_kind = padding_kind::zero)
      : hint_(gradx_desc, weights_desc, grady_desc,
          strides, padding_l, padding_r)  {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_memory_desc_t diff_src_any = gradx_desc.format_any();
      mkldnn_memory_desc_t weights_any = weights_desc.format_any();
      mkldnn_memory_desc_t diff_dst_any = grady_desc.format_any();

      mkldnn_convolution_desc_t data;
      error::wrap_c_api(mkldnn_convolution_backward_data_desc_init(&data,
            convert_to_c(aalgorithm), &diff_src_any,
            &weights_any, &diff_dst_any,
            &strides[0], &padding_l[0], &padding_r[0],
            mkldnn::convert_to_c(apadding_kind)),
          "could not create a convolution backward data descriptor");

      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(&result,
            &data, engine::cpu_engine().get(), hint_.get()),
      "could not create a convolution backward data primitive descriptor");
      reset(result);
      create_reorder_pds({grady_desc, weights_desc});
    }

    descriptor(const tensor::descriptor &grady_desc,
        const tensor::descriptor &weights_desc,
        const tensor::descriptor &gradx_desc,
        const tensor::dims strides,
        const tensor::dims dilates,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        const padding_kind apadding_kind = padding_kind::zero)
      : hint_(gradx_desc, weights_desc, grady_desc,
          strides, dilates, padding_l, padding_r)  {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(dilates);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_convolution_desc_t data;
      mkldnn_memory_desc_t diff_src_any = gradx_desc.format_any();
      mkldnn_memory_desc_t weights_any = weights_desc.format_any();
      mkldnn_memory_desc_t diff_dst_any = grady_desc.format_any();
      error::wrap_c_api(mkldnn_dilated_convolution_backward_data_desc_init(
            &data, convert_to_c(aalgorithm), &diff_src_any,
            &weights_any, &diff_dst_any, &strides[0], &dilates[0],
            &padding_l[0], &padding_r[0],
            mkldnn::convert_to_c(apadding_kind)),
          "could not create a convolution backward data descriptor");

      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(&result,
            &data, engine::cpu_engine().get(), hint_.get()),
      "could not create a convolution backward data primitive descriptor");
      reset(result);
      create_reorder_pds({grady_desc, weights_desc});
    }
  private:
    convolution_forward::descriptor hint_;
  };
public:
  using computation::computation;
  using computation::expected_gradx_descriptor;

  template<typename ...Ts>
  void init(const tensor::descriptor &grady_desc,
      const tensor::descriptor &weights_desc,
      const tensor::descriptor &gradx_desc, Ts&&... args) {
    descriptor backward_data_descriptor(grady_desc, weights_desc,
        gradx_desc, std::forward<Ts>(args)...);
    computation::init(backward_data_descriptor, grady_desc, weights_desc);
  }

  convolution_backward_data () = default;

  template <typename T, typename ...Ts>
  convolution_backward_data (T arg, Ts&&... args) {
    init(arg, std::forward<Ts>(args)...);
  }

  void execute(const tensor& grady, const tensor& weights,
      const tensor& gradx) {
    computation::execute(grady, weights, gradx);
  }

  template <typename ...Ts>
  static tensor::descriptor compute_impl(const tensor& grady,
      const tensor& weights, const tensor::dims& gradx_dims, void *gradx_r,
      Ts&&... args) {
    tensor::descriptor result_desc(gradx_dims, grady.get_data_type());
    auto key = utils::create_key(grady.get_data_type(), grady.get_dims(),
        weights.get_dims(), gradx_dims, args...);

    auto comp = fetch_or_create(key, grady.get_descriptor(),
        weights.get_descriptor(), result_desc, std::forward<Ts>(args)...);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    // XXX: Performance evaluation
    // TODO: Custom allocator support
    auto grady_in = grady;
    auto weights_in = weights;
    if (grady.get_descriptor() != comp.expected_grady_descriptor()) {
      grady_in.init(comp.expected_grady_descriptor());
      reorder::compute(grady, grady_in);
    }
    if (weights.get_descriptor() != comp.expected_weights_descriptor()) {
      weights_in.init(comp.expected_weights_descriptor());
      reorder::compute(weights, weights_in);
    }

    tensor gradx(comp.expected_gradx_descriptor(), gradx_r);
    comp.execute(grady_in, weights_in, gradx);
    return comp.expected_gradx_descriptor();
  }

  static tensor::descriptor compute(const tensor& grady, const tensor& weights,
      const tensor::dims& gradx_dims, void *result, const tensor::dims strides,
      const tensor::dims padding_l, const tensor::dims padding_r,
      algorithm aalgorithm = algorithm::convolution_direct,
      const padding_kind apadding_kind = padding_kind::zero) {
    return compute_impl(grady, weights, gradx_dims, result, strides, padding_l,
        padding_r, aalgorithm, apadding_kind);
  }

  static tensor::descriptor compute(const tensor& grady, const tensor& weights,
      const tensor::dims& gradx_dims, void *result, const tensor::dims strides,
      const tensor::dims dilates, const tensor::dims padding_l,
      const tensor::dims padding_r,
      algorithm aalgorithm = algorithm::convolution_direct,
      const padding_kind apadding_kind = padding_kind::zero) {
    return compute_impl(grady, weights, gradx_dims, result, strides, dilates,
        padding_l, padding_r, aalgorithm, apadding_kind);
  }
};

struct convolution_backward_weights : public computation,
  public utils::computation_cache<convolution_backward_weights> {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &x_desc,
        const tensor::descriptor &grady_desc,
        const tensor::descriptor &gradw_desc,
        const tensor::descriptor &gradb_desc,
        const tensor::dims strides,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        const padding_kind apadding_kind = padding_kind::zero)
      : hint_(x_desc, gradw_desc, gradb_desc,
          grady_desc, strides, padding_l, padding_r) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_convolution_desc_t data;
      mkldnn_memory_desc_t src_any = x_desc.format_any();
      mkldnn_memory_desc_t diff_weights_any = gradw_desc.format_any();
      mkldnn_memory_desc_t diff_bias_any = gradb_desc.format_any();
      mkldnn_memory_desc_t diff_dst_any = grady_desc.format_any();

      error::wrap_c_api(mkldnn_convolution_backward_weights_desc_init(
            &data, convert_to_c(aalgorithm), &src_any,
            &diff_weights_any, &diff_bias_any,
            &diff_dst_any, &strides[0], &padding_l[0], &padding_r[0],
            mkldnn::convert_to_c(apadding_kind)),
          "could not create a convolution backward weights descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), hint_.get()),
          "could not create a convolution backward weights primitive descriptor");
      reset(result);
      create_reorder_pds({x_desc, grady_desc});
    }
    descriptor(const tensor::descriptor &x_desc,
        const tensor::descriptor &grady_desc,
        const tensor::descriptor &gradw_desc,
        const tensor::dims strides,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        const padding_kind apadding_kind = padding_kind::zero)
      : hint_(x_desc, gradw_desc, grady_desc, strides, padding_l, padding_r) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_convolution_desc_t data;
      mkldnn_memory_desc_t src_any = x_desc.format_any();
      mkldnn_memory_desc_t diff_weights_any = gradw_desc.format_any();
      mkldnn_memory_desc_t diff_dst_any = grady_desc.format_any();
      error::wrap_c_api(mkldnn_convolution_backward_weights_desc_init(
            &data, convert_to_c(aalgorithm), &src_any,
            &diff_weights_any, nullptr, &diff_dst_any,
            &strides[0], &padding_l[0], &padding_r[0],
            mkldnn::convert_to_c(apadding_kind)),
          "could not create a convolution backward weights descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), hint_.get()),
          "could not create a convolution backward weights primitive descriptor");
      reset(result);
      create_reorder_pds({x_desc, grady_desc});
    }
    descriptor(const tensor::descriptor &x_desc,
        const tensor::descriptor &grady_desc,
        const tensor::descriptor &gradw_desc,
        const tensor::descriptor &gradb_desc,
        const tensor::dims strides,
        const tensor::dims dilates,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        const padding_kind apadding_kind = padding_kind::zero)
      : hint_(x_desc, gradw_desc, gradb_desc, grady_desc,
          strides, dilates, padding_l, padding_r) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(dilates);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_convolution_desc_t data;
      mkldnn_memory_desc_t src_any = x_desc.format_any();
      mkldnn_memory_desc_t diff_weights_any = gradw_desc.format_any();
      mkldnn_memory_desc_t diff_bias_any = gradb_desc.format_any();
      mkldnn_memory_desc_t diff_dst_any = grady_desc.format_any();
      error::wrap_c_api(
          mkldnn_dilated_convolution_backward_weights_desc_init(
            &data, convert_to_c(aalgorithm), &src_any,
            &diff_weights_any, &diff_bias_any,
            &diff_dst_any, &strides[0], &dilates[0],
            &padding_l[0], &padding_r[0],
            mkldnn::convert_to_c(apadding_kind)),
          "could not create a convolution backward weights descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), hint_.get()),
          "could not create a convolution backward weights primitive descriptor");
      reset(result);
      create_reorder_pds({x_desc, grady_desc});
    }
    descriptor(const tensor::descriptor &x_desc,
        const tensor::descriptor &grady_desc,
        const tensor::descriptor &gradw_desc,
        const tensor::dims strides,
        const tensor::dims dilates,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm = algorithm::convolution_direct,
        const padding_kind apadding_kind = padding_kind::zero)
      :hint_(x_desc, gradw_desc, grady_desc,
          strides, dilates, padding_l, padding_r) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(dilates);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_convolution_desc_t data;
      mkldnn_memory_desc_t src_any = x_desc.format_any();
      mkldnn_memory_desc_t diff_weights_any = gradw_desc.format_any();
      mkldnn_memory_desc_t diff_dst_any = grady_desc.format_any();
      error::wrap_c_api(
          mkldnn_dilated_convolution_backward_weights_desc_init(
            &data, convert_to_c(aalgorithm), &src_any,
            &diff_weights_any, nullptr, &diff_dst_any,
            &strides[0], &dilates[0],  &padding_l[0], &padding_r[0],
            mkldnn::convert_to_c(apadding_kind)),
          "could not create a convolution backward weights descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), hint_.get()),
          "could not create a convolution backward weights primitive descriptor");
      reset(result);
      create_reorder_pds({x_desc, grady_desc});
    }
  private:
    convolution_forward::descriptor hint_;
  };
public:
  using computation::expected_gradw_descriptor;
  using computation::expected_gradb_descriptor;

  template <typename ...Ts>
  void init(const tensor::descriptor &x_desc,
      const tensor::descriptor &grady_desc,
      const tensor::descriptor &gradw_desc, Ts&&... args) {
    descriptor backward_weights_descriptor(x_desc, grady_desc, gradw_desc,
        std::forward<Ts>(args)...);
    computation::init(backward_weights_descriptor, x_desc, grady_desc);
  }

  convolution_backward_weights () = default;

  template <typename T, typename ...Ts>
  convolution_backward_weights (T arg, Ts&&... args) {
    init(arg, std::forward<Ts>(args)...);
  }

  void execute(const tensor& src, const tensor& grady, const tensor& gradw,
      const tensor& grad_bias) {
    computation::execute(src, grady, gradw, grad_bias);
  }

  void execute(const tensor& src, const tensor& grady, const tensor& gradw) {
    computation::execute(src, grady, gradw);
  }

  /*
   * This interface require MKL-DNN fixed beyoned
   * https://github.com/intel/mkl-dnn/commit/86f152b614c947b87633062a182c57775856a348
   */
  template <typename V, typename ...Ts,
           typename = typename std::enable_if<
             std::is_same<V, void>::value>::type>
  static tensor::descriptor compute_impl(const tensor& src, const tensor& grady,
      const tensor::dims& gradw_dims, void *gradw_r, V *gbias_r,
      Ts&&... args) {
    tensor::descriptor gradw_desc(gradw_dims, src.get_data_type());
    tensor::descriptor gradb_desc(
        tensor::dims {grady.get_dim(1)}, src.get_data_type());

    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        grady.get_dims(), gradw_dims, grady.get_dim(1), args...);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        grady.get_descriptor(), gradw_desc, gradb_desc,
        std::forward<Ts>(args)...);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    // XXX: Performance evaluation
    // TODO: Custom allocator support
    auto src_in = src;
    auto grady_in = grady;
    if (src_in.get_descriptor() != comp.expected_src_descriptor()) {
      src_in.init(comp.expected_src_descriptor());
      reorder::compute(src, src_in);
    }
    if (grady.get_descriptor() != comp.expected_grady_descriptor()) {
      grady_in.init(comp.expected_grady_descriptor());
      reorder::compute(grady, grady_in);
    }

    tensor gradw(comp.expected_gradw_descriptor(), gradw_r);
    tensor gbias(comp.expected_gradb_descriptor(), gbias_r);
    comp.execute(src_in, grady_in, gradw, gbias);
    return comp.expected_gradw_descriptor();
  }

  template <typename ...Ts>
  static tensor::descriptor compute_impl(const tensor& src, const tensor& grady,
      const tensor::dims& gradw_dims, void *gradw_r, Ts&&... args) {
    tensor::descriptor gradw_desc(gradw_dims, src.get_data_type());

    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        grady.get_dims(), gradw_dims, args...);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        grady.get_descriptor(), gradw_desc, std::forward<Ts>(args)...);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    // XXX: Performance evaluation
    // TODO: Custom allocator support
    auto src_in = src;
    auto grady_in = grady;
    if (src_in.get_descriptor() != comp.expected_src_descriptor()) {
      src_in.init(comp.expected_src_descriptor());
      reorder::compute(src, src_in);
    }
    if (grady.get_descriptor() != comp.expected_grady_descriptor()) {
      grady_in.init(comp.expected_grady_descriptor());
      reorder::compute(grady, grady_in);
    }

    tensor gradw(comp.expected_gradw_descriptor(), gradw_r);
    comp.execute(src_in, grady_in, gradw);
    return comp.expected_gradw_descriptor();
  }

  static tensor::descriptor compute(const tensor& src, const tensor& grady,
      const tensor::dims& gradw_dims, void *gradw_r,
      const tensor::dims strides, const tensor::dims dilates,
      const tensor::dims padding_l, const tensor::dims padding_r,
      algorithm aalgorithm = algorithm::convolution_direct,
      const padding_kind apadding_kind = padding_kind::zero) {
    return compute_impl(src, grady, gradw_dims, gradw_r, strides, dilates,
        padding_l, padding_r, aalgorithm, apadding_kind);
  }

  static tensor::descriptor compute(const tensor& src, const tensor& grady,
      const tensor::dims& gradw_dims, void *gradw_r, void *gradb_r,
      const tensor::dims strides, const tensor::dims dilates,
      const tensor::dims padding_l, const tensor::dims padding_r,
      algorithm aalgorithm = algorithm::convolution_direct,
      const padding_kind apadding_kind = padding_kind::zero) {
    return compute_impl(src, grady, gradw_dims, gradw_r, gradb_r, strides,
        dilates, padding_l, padding_r, aalgorithm, apadding_kind);
  }

  static tensor::descriptor compute(const tensor& src, const tensor& grady,
      const tensor::dims& gradw_dims, void *gradw_r,
      const tensor::dims strides, const tensor::dims padding_l,
      const tensor::dims padding_r,
      algorithm aalgorithm = algorithm::convolution_direct,
      const padding_kind apadding_kind = padding_kind::zero) {
    return compute_impl(src, grady, gradw_dims, gradw_r, strides,
        padding_l, padding_r, aalgorithm, apadding_kind);
  }

  static tensor::descriptor compute(const tensor& src, const tensor& grady,
      const tensor::dims& gradw_dims, void *gradw_r, void *gradb_r,
      const tensor::dims strides, const tensor::dims padding_l,
      const tensor::dims padding_r,
      algorithm aalgorithm = algorithm::convolution_direct,
      const padding_kind apadding_kind = padding_kind::zero) {
    return compute_impl(src, grady, gradw_dims, gradw_r, gradb_r, strides,
        padding_l, padding_r, aalgorithm, apadding_kind);
  }
};

struct lrn_forward : public computation,
  public utils::computation_cache<lrn_forward> {
  struct descriptor : public descriptor_group {
    descriptor (const tensor::descriptor &x_desc,
        int local_size, float alpha, float beta, float k = 1.0,
        algorithm aalgorithm = algorithm::lrn_across_channels,
        prop_kind aprop_kind = prop_kind::forward) {
      mkldnn_lrn_desc_t data;
      auto src_data = x_desc.get_mkldnn_memory_desc_t();
      error::wrap_c_api(mkldnn_lrn_forward_desc_init(&data,
          mkldnn::convert_to_c(aprop_kind), convert_to_c(aalgorithm),
          src_data, local_size, alpha, beta, k),
          "could not create a lrn forward descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
              &result, &data, engine::cpu_engine().get(), nullptr),
          "could not create a lrn forward primitive descriptor");
      reset(result);
      // create_reorder_pds({x_desc});
    }
  };
public:
  using computation::expected_dst_descriptor;
  using computation::expected_workspace_descriptor;

  template <typename ...Ts>
  void init(const tensor::descriptor &x_desc, Ts&&... args) {
    descriptor forward_descriptor(x_desc, std::forward<Ts>(args)...);
    computation::init(forward_descriptor, x_desc);
  }

  lrn_forward() = default;

  template <typename T, typename ...Ts>
  lrn_forward(T arg, Ts&&... args) {
    init(arg, std::forward<Ts>(args)...);
  }

  void execute(const tensor &src, const tensor& dst, const tensor& workspace) {
    computation::execute(src, dst, workspace);
  }

  void execute(const tensor &src, tensor& dst) {
    if (dst.has_extra())
      computation::execute(src, dst, *dst.get_extra());
    else
      computation::execute(src, dst);
  }

  static tensor compute(const tensor& src, void* dst_r, int local_size,
      float alpha, float beta, float k = 1.0,
      algorithm aalgorithm = algorithm::lrn_across_channels,
      prop_kind aprop_kind = prop_kind::forward_training) {
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        src.get_internal_format(), local_size, alpha, beta, k,
        aalgorithm, aprop_kind);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        local_size, alpha, beta, k, aalgorithm, aprop_kind);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    bool with_workspace = aprop_kind == prop_kind::forward_training;

    // TODO: scratch allocator support
    tensor dst =  with_workspace ? tensor { comp.expected_dst_descriptor(),
      dst_r, comp.expected_workspace_descriptor() } :
        tensor { comp.expected_dst_descriptor(), dst_r };

    comp.execute(src, dst);
    return dst;
  }
};

struct lrn_backward : public computation,
 public utils::computation_cache<lrn_backward> {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &x_desc,
        const tensor::descriptor &gx_desc,
        int local_size, float alpha, float beta, float k = 1.0,
        algorithm aalgorithm = algorithm::lrn_across_channels)
      : hint_(x_desc, local_size, alpha, beta, k, aalgorithm) {
      mkldnn_lrn_desc_t data;
      error::wrap_c_api(mkldnn_lrn_backward_desc_init(&data,
            convert_to_c(aalgorithm), gx_desc.get_mkldnn_memory_desc_t(),
            x_desc.get_mkldnn_memory_desc_t(), local_size, alpha, beta, k),
          "could not create a lrn backward descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(),
            hint_.get()),
          "could not create a backward lrn primitive descriptor");
      reset(result);
    }

  private:
    lrn_forward::descriptor hint_;
  };
public:
  using computation::expected_gradx_descriptor;

  template<typename ...Ts>
  void init(const tensor::descriptor &x_desc,
      const tensor::descriptor &grady_desc, Ts&&... args) {
    descriptor backward_data_descriptor(x_desc, grady_desc,
        std::forward<Ts>(args)...);
    computation::init(backward_data_descriptor, x_desc, grady_desc);
  }

  lrn_backward() = default;

  template<typename T, typename ...Ts>
  lrn_backward(T arg, Ts&&... args) {
    init(arg, std::forward<Ts>(args)...);
  }

  void execute(const tensor& x, const tensor& grady, const tensor& y,
      const tensor& gradx) {
    if (num_of_inputs() == 2)
      computation::execute(x, grady, gradx);
    else
      computation::execute(x, grady, *y.get_extra(), gradx);
  }

  static tensor compute(const tensor& x, const tensor& grady, const tensor& y,
      void* gradx_r, int local_size, float alpha, float beta, float k = 1.0,
      algorithm aalgorithm = algorithm::lrn_across_channels) {
    auto key = utils::create_key(x.get_data_type(), x.get_dims(),
        x.get_internal_format(), local_size, alpha, beta, k, aalgorithm);

    auto comp = fetch_or_create(key, x.get_descriptor(),
        grady.get_descriptor(), local_size, alpha, beta, k, aalgorithm);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    tensor gradx(comp.expected_gradx_descriptor(), gradx_r);
    comp.execute(x, grady, y, gradx);
    return gradx;
  }
};

struct pooling_forward : public computation,
  public utils::computation_cache<pooling_forward> {
  struct descriptor : descriptor_group {
    descriptor(
        const tensor::descriptor &x_desc,
        const tensor::descriptor &y_desc,
        const tensor::dims strides,
        const tensor::dims kernel,
        const tensor::dims padding_l,
        const tensor::dims padding_r,
        algorithm aalgorithm,
        prop_kind aprop_kind = prop_kind::forward,
        const padding_kind apadding_kind = padding_kind::zero) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(kernel);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      auto src_data = x_desc.get_mkldnn_memory_desc_t();
      auto dst_data = y_desc.format_any();
      mkldnn_pooling_desc_t data;
      error::wrap_c_api(mkldnn_pooling_forward_desc_init(&data,
            mkldnn::convert_to_c(aprop_kind),
            convert_to_c(aalgorithm),
            src_data, &dst_data,
            &strides[0], &kernel[0],
            &padding_l[0], &padding_r[0],
            mkldnn::convert_to_c(apadding_kind)),
          "could not init a forward pooling descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), nullptr),
          "could not create a forward pooling primitive descriptor");
      reset(result);
    }
  };
public:
  using computation::expected_dst_descriptor;
  using computation::expected_workspace_descriptor;

  template <typename ...Ts>
  void init(const tensor::descriptor &x_desc, Ts&&... args) {
    descriptor forward_descriptor(x_desc, std::forward<Ts>(args)...);
    computation::init(forward_descriptor, x_desc);
  }

  pooling_forward() = default;

  template <typename T, typename ...Ts>
  pooling_forward(T arg, Ts&&... args) {
    init(std::forward<T>(arg), std::forward<Ts>(args)...);
  }

  void execute(const tensor& src, const tensor& dst, const tensor &workspace) {
    computation::execute(src, dst, workspace);
  }

  void execute(const tensor& src, tensor& dst) {
    if (dst.has_extra())
      computation::execute(src, dst, *dst.get_extra());
    else
      computation::execute(src, dst);
  }

  static tensor compute(const tensor &src,
      const tensor::dims dst_dims, void *dst_r,
      const tensor::dims strides, const tensor::dims kernel,
      const tensor::dims padding_l, const tensor::dims padding_r,
      algorithm aalgorithm, prop_kind aprop_kind = prop_kind::forward,
      const padding_kind apadding_kind = padding_kind::zero) {
    tensor::descriptor dst_desc(dst_dims, src.get_data_type());
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        src.get_internal_format(), dst_dims, strides, kernel, padding_l,
        padding_r, aalgorithm, aprop_kind, apadding_kind);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        dst_desc, strides, kernel, padding_l, padding_r, aalgorithm,
        aprop_kind, apadding_kind);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    bool with_workspace = true
        && aprop_kind == prop_kind::forward_training
        && aalgorithm == mkldnn::pooling_max;

    // TODO: scratch allocator support
    tensor dst = with_workspace ? tensor(comp.expected_dst_descriptor(),
      dst_r, comp.expected_workspace_descriptor()) :
      tensor(comp.expected_dst_descriptor(), dst_r);

    comp.execute(src, dst);
    return dst;
  }
};

struct pooling_backward : public computation {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &gradx_desc,
            const tensor::descriptor &grady_desc,
            const tensor::dims &strides,
            const tensor::dims &kernel,
            const tensor::dims &padding_l,
            const tensor::dims &padding_r,
            algorithm aalgorithm,
            const padding_kind apadding_kind = padding_kind::zero)
      : hint_(gradx_desc, grady_desc, strides, kernel,
          padding_l, padding_r, aalgorithm) {
      mkldnn::memory::validate_dims(strides);
      mkldnn::memory::validate_dims(kernel);
      mkldnn::memory::validate_dims(padding_l);
      mkldnn::memory::validate_dims(padding_r);
      mkldnn_memory_desc_t diff_src_data = gradx_desc.format_any();
      mkldnn_pooling_desc_t data;
      error::wrap_c_api(mkldnn_pooling_backward_desc_init(&data,
            convert_to_c(aalgorithm),
            &diff_src_data,
            grady_desc.get_mkldnn_memory_desc_t(),
            &strides[0], &kernel[0],
            &padding_l[0], &padding_r[0],
            mkldnn::convert_to_c(apadding_kind)),
          "could not init a backward pooling descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
                  &result, &data, engine::cpu_engine().get(),
                  hint_.get()),
              "could not create a backward pooling primitive descriptor");
      reset(result);
    }
  private:
    pooling_forward::descriptor hint_;
  };
public:
  using computation::computation;
  using computation::expected_gradx_descriptor;

  template <typename ...Ts>
  void init(const tensor::descriptor &gradx_desc,
      const tensor::descriptor &grady_desc, Ts&&... args) {
    descriptor backward_weights_descriptor(gradx_desc, grady_desc,
        std::forward<Ts>(args)...);
    computation::init(backward_weights_descriptor, gradx_desc, grady_desc);
  }

  void execute(const tensor& grady, const tensor& gradx) {
    computation::execute(grady, gradx);
  }

  void execute(const tensor& grady, const tensor& y, const tensor& gradx) {
    computation::execute(grady, *y.get_extra(), gradx);
  }
};

struct eltwise_forward : public computation,
  public utils::computation_cache<eltwise_forward> {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &x_desc,
        float alpha = 0.0, float beta = 0.0,
        algorithm alg_kind = algorithm::eltwise_relu,
        prop_kind aprop_kind = prop_kind::forward) {
      mkldnn_eltwise_desc_t data;
      error::wrap_c_api(mkldnn_eltwise_forward_desc_init(&data,
            mkldnn::convert_to_c(aprop_kind),
            mkldnn::convert_to_c(alg_kind),
            x_desc.get_mkldnn_memory_desc_t(),
            alpha, beta),
              "could not create a eltwise forward descriptor");

      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
                &result, &data, engine::cpu_engine().get(), nullptr)
        , "could not create a eltwise forward primitive descriptor");
      reset(result);
    }
  };

public:
  using computation::computation;
  using computation::expected_dst_descriptor;

  template<typename ...Ts>
  void init(const tensor::descriptor &x_desc, Ts&&... args) {
    descriptor forward_descriptor(x_desc, std::forward<Ts>(args)...);
    computation::init(forward_descriptor, x_desc);
  }

  eltwise_forward() = default;

  template <typename T, typename ...Ts>
  eltwise_forward(T arg, Ts&&... args) {
    init(std::forward<T>(arg), std::forward<Ts>(args)...);
  }

  void execute(const tensor& x, const tensor& y) {
    computation::execute(x, y);
  }

  template<typename ...Ts>
  static tensor::descriptor compute_impl(const tensor& src,
      void *result, Ts&&... args) {
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        src.get_internal_format(), args...);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        std::forward<Ts>(args)...);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    tensor dst(src.get_descriptor(), result);
    comp.execute(src, dst);
    return dst.get_descriptor();
  }

  static tensor::descriptor compute(const tensor &src, void *result,
      algorithm aalogorithm = algorithm::eltwise_relu,
      prop_kind aprop_kind = prop_kind::forward,
      float alpha = 0.0, float beta = 0.0) {
    return compute_impl(src, result, alpha, beta, aalogorithm, aprop_kind);
  }
};

struct eltwise_backward : public computation,
  public utils::computation_cache<eltwise_backward> {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &grady_desc,
        const tensor::descriptor &x_desc,
        float alpha = 0.0, float beta = 0.0,
        algorithm alg_kind = algorithm::eltwise_relu)
      : hint_(x_desc, alg_kind) {
      mkldnn_eltwise_desc_t data;
      error::wrap_c_api(mkldnn_eltwise_backward_desc_init(&data,
            mkldnn::convert_to_c(alg_kind),
            grady_desc.get_mkldnn_memory_desc_t(),
            x_desc.get_mkldnn_memory_desc_t(),
            static_cast<float>(alpha),
            static_cast<float>(beta)),
          "could not create a eltwise backward descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), hint_.get()),
          "could not create a eltwise backward primitive descriptor");
      reset(result);
    }
  private:
    eltwise_forward::descriptor hint_;
  };
public:
  using computation::computation;
  using computation::expected_gradx_descriptor;

  template <typename ...Ts>
  void init(const tensor::descriptor &grady_desc,
      const tensor::descriptor &x_desc, Ts&&... args) {
    descriptor backward_descriptor(
        grady_desc, x_desc, std::forward<Ts>(args)...);
    computation::init(backward_descriptor, grady_desc, x_desc);
  }

  eltwise_backward() = default;

  template <typename T, typename ...Ts>
  eltwise_backward(T grady_desc, T src_desc, Ts&&... args) {
    init(std::forward<T>(grady_desc), std::forward<T>(src_desc),
        std::forward<Ts>(args)...);
  }

  void execute(const tensor& x, const tensor& grady, const tensor& gradx) {
    computation::execute(x, grady, gradx);
  }

  template<typename ...Ts>
  static tensor::descriptor compute_impl(const tensor& src, const tensor& grady,
      void *result, Ts&&... args) {
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        src.get_internal_format(), grady.get_internal_format(), args...);

    auto comp = fetch_or_create(key, grady.get_descriptor(),
        src.get_descriptor(), std::forward<Ts>(args)...);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    tensor gradx(comp.expected_gradx_descriptor(), result);
    comp.execute(src, grady, gradx);
    return gradx.get_descriptor();
  }

  static tensor::descriptor compute(const tensor &src, const tensor &grady,
      void *result, algorithm aalogorithm = algorithm::eltwise_relu,
      float alpha = 0.0, float beta = 0.0) {
    return compute_impl(src, grady, result, alpha, beta, aalogorithm);
  }
};

class sum : public computation,
  public utils::computation_cache<sum> {
  struct descriptor : public descriptor_group {
    descriptor(const std::vector<float> &scales,
        const std::vector<tensor::descriptor> &inputs,
        const tensor::descriptor *output = nullptr) {
      mkldnn_primitive_desc_t result;
      auto c_api_inputs = cpp_to_c(inputs);
      error::wrap_c_api(mkldnn_sum_primitive_desc_create(
              &result, output ? output->get_mkldnn_memory_desc_t(): nullptr,
              (int)c_api_inputs.size(),
              &scales[0], &c_api_inputs[0]),
          "could not create a sum primitive descriptor");
      reset(result);
    }
  };
public:
  using computation::execute;
  using computation::expected_dst_descriptor;

  void init(const std::vector<float> &scales,
      const std::vector<tensor::descriptor> &inputs,
      const tensor::descriptor *output = nullptr) {
    auto forward_descriptor = descriptor(scales, inputs, output);
    computation::init(forward_descriptor, inputs);
  }

  sum() = default;

  sum(const std::vector<float> &scales,
      const std::vector<tensor::descriptor> &inputs_desc,
      const tensor::descriptor *output_desc = nullptr) {
    init(scales, inputs_desc, output_desc);
  }

  void execute(const std::vector<tensor>& inputs, const tensor& output) {
    computation::execute(inputs, output);
  }

  static tensor::descriptor compute_impl(const std::vector<float> &scales,
      const std::vector<tensor> &inputs, void *raw_out,
      const tensor::descriptor *out_desc) {
    std::vector<tensor::descriptor> inputs_desc;
    for_each(inputs.begin(), inputs.end(), [&inputs_desc](tensor in) {
        inputs_desc.push_back(in.get_descriptor());
        });

    auto comp = sum(scales, inputs_desc, out_desc);
    auto _out_desc = out_desc ? *out_desc :
        comp.expected_dst_descriptor();

    comp.execute(inputs, tensor(_out_desc, raw_out));
    return _out_desc;
  }

  static tensor::descriptor compute(const std::vector<float> &scales,
      const std::vector<tensor> &inputs, void *raw_out,
      const tensor::descriptor *out_desc = nullptr) {
    return compute_impl(scales, inputs, raw_out, out_desc);
  }
};

class concat : public computation {
  struct descriptor : public descriptor_group {
    descriptor(int concat_dimension,
        const std::vector<tensor::descriptor> &inputs) {
      mkldnn_primitive_desc_t result;
      auto c_api_inputs = cpp_to_c(inputs);
      error::wrap_c_api(mkldnn_concat_primitive_desc_create(
              &result, nullptr,
              (int)c_api_inputs.size(),
              concat_dimension, &c_api_inputs[0]),
          "could not create a concat primitive descriptor");
      reset(result);
    }
  };
public:
  using computation::execute;
  using computation::expected_dst_descriptor;

  void init(int concat_dimension,
      const std::vector<tensor::descriptor> &inputs) {
    descriptor forward_descriptor (concat_dimension, inputs);
    computation::init(forward_descriptor, inputs);
  }

  void execute(const std::vector<tensor>& inputs, const tensor& output) {
    computation::execute(inputs, output);
  }
};

struct softmax_forward : public computation {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &x_desc, int softmax_axis,
        prop_kind aprop_kind = prop_kind::forward) {
      mkldnn_softmax_desc_t data;
      error::wrap_c_api(mkldnn_softmax_forward_desc_init(&data,
            mkldnn::convert_to_c(aprop_kind), x_desc.get_mkldnn_memory_desc_t(),
            softmax_axis),
          "could not create a softmax forward descriptor");

      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
              &result, &data, engine::cpu_engine().get(), nullptr),
          "could not create a softmax forward primitive descriptor");
      reset(result);
    }
  };

public:
  using computation::expected_dst_descriptor;

  template<typename ...Ts>
  void init(const tensor::descriptor& src_desc,
      const tensor::descriptor& dst_desc, Ts&&... args) {
    descriptor softmax_descriptor(src_desc, std::forward<Ts>(args)...);
    computation::init(softmax_descriptor, src_desc, dst_desc);
  }

  void execute(const tensor& src, const tensor& dst) {
    computation::execute(src, dst);
  }
};

struct batch_norm_forward_base : public computation {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &src_desc, float epsilon,
        unsigned flags, prop_kind aprop_kind) {
      mkldnn_batch_normalization_desc_t data;
      error::wrap_c_api(
          mkldnn_batch_normalization_forward_desc_init(&data,
            mkldnn::convert_to_c(aprop_kind), src_desc.get_mkldnn_memory_desc_t(),
            epsilon, flags),
          "could not create a batch normalization forward descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
          &result, &data, engine::cpu_engine().get(), nullptr),
      "could not create a batch normalization forward primitive descriptor");
      reset(result);
    }
  };

public:
  using computation::expected_dst_descriptor;

  template<typename... Ts>
  void init(float epsilon, unsigned flags, prop_kind aprop_kind,
      const tensor::descriptor &src_desc, Ts&... rest) {
    descriptor batch_norm_forward(src_desc, epsilon, flags, aprop_kind);
    init(batch_norm_forward, src_desc, rest...);
  }

  /// Execute interface for (1, 0) (stats_is_src, use_scaleshift)
  void execute(const tensor& src, const tensor& mean, const tensor& variance,
      const tensor& dst) {
    computation::execute(src, mean, variance, dst);
  }

  /// Execute interface for (1, 1)
  void execute(const tensor& src, const tensor& mean, const tensor& variance,
      const tensor& weights, const tensor&dst) {
    computation::execute(src, mean, variance, weights, dst);
  }
};

struct batch_normalization_forward_inference : public batch_norm_forward_base,
  public utils::computation_cache<batch_normalization_forward_inference> {
public:
  using batch_norm_forward_base::execute;

  /// Execute interface for  (0, 0)
  void execute(const tensor& src, const tensor& dst) {
    computation::execute(src, dst);
  }

  /// Execute interface for  (0, 1)
  void execute(const tensor& src, const tensor& weights, const tensor& dst) {
    computation::execute(src, weights, dst);
  }

public:
  void init(const tensor::descriptor& src_desc, const tensor::descriptor& scale,
      const tensor::descriptor& shift, float epsilon) {
    assert(scale.ndims() == 1 && shift.ndims() == 1);
    descriptor batch_norm_forward(
        src_desc, epsilon, batch_normalization_flag::use_scale_shift,
        prop_kind::forward_scoring);
    weights_.init(batch_norm_forward.expected_weights_descriptor());
    computation::init(batch_norm_forward, src_desc, weights_.get_descriptor());
  }

  void init(const tensor::descriptor& src_desc, const tensor::descriptor& mean,
      const tensor::descriptor& variance, const tensor::descriptor& scale,
      const tensor::descriptor& shift, float epsilon) {
    assert(scale.ndims() == 1 && shift.ndims() == 1);
    descriptor batch_norm_forward(src_desc, epsilon,
        batch_normalization_flag::use_global_stats
        | batch_normalization_flag::use_scale_shift,
        prop_kind::forward_scoring);
    weights_.init(batch_norm_forward.expected_weights_descriptor());
    computation::init(
      batch_norm_forward, src_desc, mean, variance, weights_.get_descriptor());
  }

  void init(const tensor::descriptor& src_desc, float epsilon,
      unsigned flag = batch_normalization_flag::use_global_stats |
      batch_normalization_flag::use_scale_shift) {
    descriptor batch_norm_forward(
        src_desc, epsilon, flag, prop_kind::forward_scoring);
    weights_.init(batch_norm_forward.expected_weights_descriptor());
    computation::init(batch_norm_forward, src_desc);
  }

  batch_normalization_forward_inference () = default;

  template <typename T, typename ...Ts>
  batch_normalization_forward_inference (T arg, Ts&&... args) {
    init(arg, std::forward<Ts>(args)...);
  }

  /// More functionality in this interface
  void execute(const tensor& src, const tensor& scale, const tensor& shift,
      const tensor& dst) {
    // Small amount of buffer, car is good
    std::memcpy(weights_.get_data_handle(),
        scale.get_data_handle(), scale.get_size());
    std::memcpy((char *)weights_.get_data_handle() + scale.get_size(),
        shift.get_data_handle(), shift.get_size());
    computation::execute(src, weights_, dst);
  }

  /// More functionality in this interface
  void execute(const tensor& src, const tensor& mean, const tensor& variance,
      const tensor& scale, const tensor& shift, const tensor& dst) {
    // Small amount of buffer, car is good
    std::memcpy(weights_.get_data_handle(),
        scale.get_data_handle(), scale.get_size());
    std::memcpy((char *)weights_.get_data_handle() + scale.get_size(),
        shift.get_data_handle(), shift.get_size());
    computation::execute(src, mean, variance, weights_, dst);
  }

  using computation::expected_dst_descriptor;

  static tensor compute(const tensor& src, const tensor& scale,
      const tensor& shift, void *dst_r, float epsilon) {
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        src.get_internal_format(), 3, epsilon);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        batch_normalization_flag::use_scale_shift, epsilon);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    tensor dst(comp.expected_dst_descriptor(), dst_r);
    comp.execute(src, scale, shift, dst);
    return dst;
  }

  static tensor compute(const tensor& src, const tensor& mean,
      const tensor& variance, const tensor& scale,
      const tensor& shift, void *dst_r, float epsilon) {
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        src.get_internal_format(), 5, epsilon);

    auto comp = fetch_or_create(key, src.get_descriptor(), epsilon);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    tensor dst(comp.expected_dst_descriptor(), dst_r);
    comp.execute(src, mean, variance, scale, shift, dst);
    return dst;
  }

private:
  param weights_;
};

struct batch_normalization_forward_training : public batch_norm_forward_base,
  public utils::computation_cache<batch_normalization_forward_training> {
  float get_epsilon() const {
    const mkldnn_batch_normalization_desc_t *p_desc;
    error::wrap_c_api(mkldnn_primitive_desc_query(get_mkldnn_primitive_desc_t(),
        static_cast<mkldnn_query_t>(query::batch_normalization_d),
        0, (void *)&p_desc),
      "could not query batch normalization descriptor");
    return p_desc->batch_norm_epsilon;
  }
public:
  using batch_norm_forward_base::execute;

  void init(const tensor::descriptor& src_desc, const tensor::descriptor& scale,
      const tensor::descriptor& shift, float momentum, float epsilon,
      unsigned flags = batch_normalization_flag::use_scale_shift) {
    assert(scale.ndims() == 1 && shift.ndims() == 1);
    descriptor batch_norm_forward(src_desc, epsilon, flags,
        prop_kind::forward_training);
    weights_.init(batch_norm_forward.expected_weights_descriptor());
    computation::init(batch_norm_forward, src_desc, weights_.get_descriptor());

    // We borrown scale and bias for the shape of mean and variance
    sum_.init({momentum, 1.f - momentum}, {scale, shift});
  }

  batch_normalization_forward_training () = default;

  template <typename T, typename... Ts>
  batch_normalization_forward_training (T arg, Ts&&... args) {
    init(arg, std::forward<Ts>(args)...);
  }

  /// Execute interface for (0, 0)
  void execute(const tensor& src, const tensor& dst, const tensor& mean,
      const tensor& variance) {
    computation::execute(src, dst, mean, variance);
  }

  /// Execute interface for (0, 1)
  void execute(const tensor& src, const tensor& weights, const tensor& dst,
      const tensor& mean, const tensor& variance) {
    computation::execute(src, weights, dst, mean, variance);
  }

  void execute(const tensor& src, const tensor& scale, const tensor& shift,
      const tensor& dst, const tensor& mean, const tensor& variance) {
    // Small amount of buffer, car is good
    std::memcpy(weights_.get_data_handle(),
        scale.get_data_handle(), scale.get_size());
    std::memcpy((char *)weights_.get_data_handle() + scale.get_size(),
        shift.get_data_handle(), shift.get_size());
    computation::execute(src, weights_, dst, mean, variance);
  }

  void running_statistic(const tensor& mean, const tensor& variance,
      const tensor& running_mean, const tensor& running_var) {
    // TODO: provide accelerated version
    std::vector<tensor> inputs_for_mean {running_mean, mean};
    std::vector<tensor> inputs_for_var {running_var, variance};
    sum_.execute(inputs_for_mean, running_mean);
    sum_.execute(inputs_for_var, running_var);
  }

  // TODO: deprecates these two
  tensor::descriptor expected_mean_descriptor() const {
    return expected_descriptor_of(query::dst_pd, 1);
  }

  tensor::descriptor expected_variance_descriptor() const {
    return expected_descriptor_of(query::dst_pd, 2);
  }

  // TODO: this is good one
  tensor::descriptor expected_statistic_descriptor() const {
    return expected_descriptor_of(query::dst_pd, 1);
  }

  using computation::expected_dst_descriptor;

  tensor compute(const tensor& src, const tensor& scale, const tensor& shift,
      void *dst_r, void *mean_r, void *variance_r,
      float momentum, float epsilon) {
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        src.get_internal_format(), epsilon);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        scale.get_descriptor(), shift.get_descriptor(), momentum, epsilon);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    tensor dst(comp.expected_dst_descriptor(), dst_r);
    tensor mean(comp.expected_statistic_descriptor(), mean_r);
    tensor variance(comp.expected_statistic_descriptor(), variance_r);

    comp.execute(src, scale, shift, dst, mean, variance);
    return dst;
  }

  static tensor compute(const tensor& src, const tensor& scale,
      const tensor& shift, void *dst_r, void *mean_r,
      void *variance_r, void *running_mean_r,
      void *running_var_r, float momentum, float epsilon) {
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        src.get_internal_format(), epsilon);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        scale.get_descriptor(), shift.get_descriptor(), momentum, epsilon);

    auto sg = utils::make_guard([&key, &comp]() {
        release(key, std::move(comp));
        });

    // TODO: Substitue running statistics calculation with lighter version
    tensor dst(comp.expected_dst_descriptor(), dst_r);
    tensor mean(comp.expected_statistic_descriptor(), mean_r);
    tensor variance(comp.expected_statistic_descriptor(), variance_r);
    tensor running_mean(comp.expected_statistic_descriptor(), running_mean_r);
    tensor running_var(comp.expected_statistic_descriptor(), running_var_r);

    comp.execute(src, scale, shift, dst, mean, variance);
    comp.running_statistic(mean, variance, running_mean, running_var);
    return dst;
  }

private:
  param weights_;
  sum sum_;
};

struct batch_normalization_backward : public computation,
  public utils::computation_cache<batch_normalization_backward> {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &gradx_desc,
        const tensor::descriptor &x_desc,
        float epsilon, unsigned flags, prop_kind aprop_kind)
      : hint_(x_desc, epsilon, flags, prop_kind::forward_training) {

      mkldnn_batch_normalization_desc_t data;
      error::wrap_c_api(
          mkldnn_batch_normalization_backward_desc_init(&data,
            mkldnn::convert_to_c(aprop_kind),
            gradx_desc.get_mkldnn_memory_desc_t(),
            x_desc.get_mkldnn_memory_desc_t(),
            static_cast<float>(epsilon), flags),
          "could not create a batch normalization backward descriptor");

      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(
          &result, &data, engine::cpu_engine().get(),
          hint_.get()),
        "could not create a batch normalization backward primitive descriptor");
      reset(result);
    }
  private:
    batch_normalization_forward_training::descriptor hint_;
  };

  float get_epsilon() const {
    const mkldnn_batch_normalization_desc_t *p_desc;
    error::wrap_c_api(mkldnn_primitive_desc_query(get_mkldnn_primitive_desc_t(),
        static_cast<mkldnn_query_t>(query::batch_normalization_d),
        0, (void *)&p_desc),
      "could not query batch normalization descriptor");
    return p_desc->batch_norm_epsilon;
  }

public:
  using computation::expected_gradx_descriptor;
  tensor::descriptor expected_grad_scale_descriptor() const {
    return expected_descriptor_of(query::src_pd, 2);
  }
  tensor::descriptor expected_grad_shift_descriptor() const {
    return expected_descriptor_of(query::src_pd, 1);
  }
  tensor::descriptor expected_statistic_descriptor() const {
    return expected_descriptor_of(query::src_pd, 1);
  }

  prop_kind get_prop_kind() const {
    const mkldnn_batch_normalization_desc_t *p_desc;
    error::wrap_c_api(mkldnn_primitive_desc_query(get_mkldnn_primitive_desc_t(),
        static_cast<mkldnn_query_t>(query::batch_normalization_d),
        0, (void *)&p_desc),
      "could not query batch normalization descriptor");
    return static_cast<prop_kind>(p_desc->prop_kind);
  }

  template <typename... Ts>
  void init(float epsilon, unsigned flags, prop_kind aprop_kind,
      const tensor::descriptor& gradx_desc, const tensor::descriptor& src_desc,
      const Ts&... input_descs) {
    descriptor batch_norm_backward(gradx_desc, src_desc, epsilon,
        flags, aprop_kind);
    init(batch_norm_backward, src_desc, input_descs...);
    weights_.init(batch_norm_backward.expected_weights_descriptor());
    gradw_.init(batch_norm_backward.expected_gradw_descriptor());
  }

  void init(const tensor::descriptor& gradx_desc,
      const tensor::descriptor& src_desc, const tensor::descriptor& mean_desc,
      const tensor::descriptor& variance_desc, const tensor::descriptor& grady_desc,
      float epsilon, unsigned flags = batch_normalization_flag::use_scale_shift,
      prop_kind aprop_kind=prop_kind::backward) {
    descriptor batch_norm_backward(gradx_desc, src_desc, epsilon,
        flags, aprop_kind);
    auto weights_desc = batch_norm_backward.expected_weights_descriptor();
    weights_.init(weights_desc);
    gradw_.init(batch_norm_backward.expected_gradw_descriptor());
    computation::init(batch_norm_backward, src_desc, mean_desc, variance_desc,
        grady_desc, weights_desc);
  }

  void init(const tensor::descriptor& gradx_desc,
      const tensor::descriptor& src_desc, float epsilon,
      unsigned flags = batch_normalization_flag::use_scale_shift,
      prop_kind aprop_kind=prop_kind::backward) {
    descriptor batch_norm_backward(gradx_desc, src_desc, epsilon,
        flags, aprop_kind);
    auto weights_desc = batch_norm_backward.expected_weights_descriptor();
    weights_.init(weights_desc);
    gradw_.init(batch_norm_backward.expected_gradw_descriptor());
    computation::init(batch_norm_backward, gradx_desc, src_desc);
  }

  batch_normalization_backward () = default;

  template <typename T, typename ...Ts>
  batch_normalization_backward(T arg, Ts&&... args) {
    init(arg, std::forward<Ts>(args)...);
  }

  void execute(const tensor& src, const tensor& mean, const tensor& variance,
      const tensor& grady, const tensor& scale, const tensor& gradx,
      const tensor& gradw) {
    // We can sure that only scale is matter at this place
    std::memcpy(
        weights_.get_data_handle(), scale.get_data_handle(), scale.get_size());
    computation::execute(src, mean, variance, grady, weights_, gradx, gradw);
  }

  void execute(const tensor& src, const tensor& mean, const tensor& variance,
      const tensor& grady, const tensor& scale, const tensor& gradx,
      const tensor& grad_scale, const tensor& grad_shift) {
    // protect API integraty, should we use solid check instead of assert?
    assert(get_prop_kind() == prop_kind::backward);
    // We can sure that only scale is matter at this place
    // And single thread of memcpy should be fast enough
    std::memcpy(
        weights_.get_data_handle(), scale.get_data_handle(), scale.get_size());
    computation::execute(src, mean, variance, grady, weights_, gradx, gradw_);
    std::memcpy(grad_scale.get_data_handle(),
        gradw_.get_data_handle(), grad_scale.get_size());
    std::memcpy(grad_shift.get_data_handle(),
        (char *)gradw_.get_data_handle() + grad_scale.get_size(),
        grad_shift.get_size());
  }

  void execute(const tensor& src, const tensor& mean, const tensor& variance,
      const tensor& grady, const tensor& scale, const tensor& gradx) {
    assert(get_prop_kind() == prop_kind::backward_data);
    std::memcpy(
        weights_.get_data_handle(), scale.get_data_handle(), scale.get_size());
    computation::execute(src, mean, variance, grady, weights_, gradx);
  }

  static tensor compute(const tensor& src, const tensor& mean,
      const tensor& variance, const tensor& grady, const tensor& scale,
      void *gradx_r, void *grad_scale_r, void *grad_shift_r, float epsilon) {
    auto key = utils::create_key(src.get_data_type(), src.get_dims(),
        src.get_internal_format(), epsilon);

    auto comp = fetch_or_create(key, src.get_descriptor(),
        src.get_descriptor(), epsilon);

    tensor gradx(comp.expected_gradx_descriptor(), gradx_r);
    tensor grad_scale(mean.get_descriptor(), grad_scale_r);
    tensor grad_shift(mean.get_descriptor(), grad_shift_r);
    comp.execute(
        src, mean, variance, grady, scale, gradx, grad_scale, grad_shift);

    return gradx;
  }

private:
  tensor weights_, gradw_;
};

struct inner_product_forward: public computation {
  struct descriptor: public descriptor_group {
    descriptor(const tensor::descriptor &src_desc,
            const tensor::descriptor &weights_desc,
            const tensor::descriptor &bias_desc,
            const tensor::descriptor &dst_desc,
            prop_kind aprop_kind = prop_kind::forward) {
      mkldnn_inner_product_desc_t data;
      mkldnn_memory_desc_t src_data = src_desc.format_any();
      mkldnn_memory_desc_t weights_data = weights_desc.format_any();
      mkldnn_memory_desc_t bias_data = bias_desc.format_any();
      mkldnn_memory_desc_t dst_data = dst_desc.format_any();

      error::wrap_c_api(
          mkldnn_inner_product_forward_desc_init(&data,
            mkldnn::convert_to_c(aprop_kind), &src_data, &weights_data,
            &bias_data, &dst_data),
          "could not create a inner product forward descriptor");

      mkldnn_primitive_desc_t result;

      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), nullptr),
          "could not create a inner product forward primitive descriptor");
      reset(result);
      create_reorder_pds({src_desc, weights_desc});
    }

    descriptor(const tensor::descriptor &src_desc,
            const tensor::descriptor &weights_desc,
            const tensor::descriptor &dst_desc,
            prop_kind aprop_kind = prop_kind::forward) {
      mkldnn_inner_product_desc_t data;
      mkldnn_memory_desc_t src_data = src_desc.format_any();
      mkldnn_memory_desc_t weights_data = weights_desc.format_any();
      mkldnn_memory_desc_t dst_data = dst_desc.format_any();

      error::wrap_c_api(
          mkldnn_inner_product_forward_desc_init(&data,
            mkldnn::convert_to_c(aprop_kind), &src_data, &weights_data,
            nullptr, &dst_data),
          "could not create a inner product forward descriptor");

      mkldnn_primitive_desc_t result;

      error::wrap_c_api(mkldnn_primitive_desc_create(
            &result, &data, engine::cpu_engine().get(), nullptr),
          "could not create a inner product forward primitive descriptor");
      reset(result);
      create_reorder_pds({src_desc, weights_desc});
    }
  };
 public:
  using computation::computation;
  using computation::init;
  using computation::execute;
  using computation::expected_dst_descriptor;

  void init(const tensor::descriptor &src_desc,
      const tensor::descriptor &weights_desc,
      const tensor::descriptor &dst_desc) {
    descriptor forward_descriptor(src_desc, weights_desc, dst_desc);
    init(forward_descriptor, src_desc, weights_desc);
  }

  void init(const tensor::descriptor &src_desc,
      const tensor::descriptor &weights_desc,
      const tensor::descriptor &bias_desc,
      const tensor::descriptor &dst_desc) {
    descriptor forward_descriptor(
        src_desc, weights_desc, bias_desc, dst_desc);
    init(forward_descriptor, src_desc, weights_desc, bias_desc);
  }
};

struct inner_product_backward_data: public computation {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &gradx_desc,
        const tensor::descriptor &weights_desc,
        const tensor::descriptor &grady_desc)
      : hint_(gradx_desc, weights_desc, grady_desc) {
      auto diff_src_data = gradx_desc.format_any();
      auto weights_data = weights_desc.format_any();
      auto diff_dst_data = grady_desc.format_any();
      mkldnn_inner_product_desc_t data;
      error::wrap_c_api(
          mkldnn_inner_product_backward_data_desc_init(&data,
            &diff_src_data, &weights_data,
            &diff_dst_data),
          "could not create a inner product backward data descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(&result,
              &data, engine::cpu_engine().get(), hint_.get()),
    "cld not create a inner product backward data primitive descriptor");
      reset(result);
    }
  private:
    inner_product_forward::descriptor hint_;
  };
public:
  using computation::computation;
  using computation::expected_gradx_descriptor;

  template <typename ...Ts>
  void init(const tensor::descriptor &x_desc,
      const tensor::descriptor &grady_desc,
      const tensor::descriptor &gradw_desc, Ts&&... args) {
    descriptor backward_weights_descriptor(x_desc, grady_desc, gradw_desc,
        std::forward<Ts>(args)...);
    computation::init(backward_weights_descriptor, x_desc, grady_desc);
  }

  void execute(const tensor& grady, const tensor& weights, const tensor& gradx) {
    computation::execute(grady, weights, gradx);
  }
};

struct inner_product_backward_weights : public computation {
  struct descriptor : public descriptor_group {
    descriptor(const tensor::descriptor &x_desc,
        const tensor::descriptor &gradw_desc,
        const tensor::descriptor &gradb_desc,
        const tensor::descriptor &grady_desc)
      : hint_(x_desc, gradw_desc, gradb_desc, grady_desc) {
      mkldnn_inner_product_desc_t data;
      auto src_data = x_desc.format_any();
      auto diff_dst_data = grady_desc.format_any();
      auto diff_weights_data = gradw_desc.format_any();
      auto diff_bias_data = gradb_desc.format_any();
      error::wrap_c_api(
          mkldnn_inner_product_backward_weights_desc_init(
            &data, &src_data, &diff_weights_data,
            &diff_bias_data, &diff_dst_data),
          "could not create a inner product backward weights descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(&result,
            &data, engine::cpu_engine().get(), hint_.get()),
    "cld not create a inner product backward weights primitive descriptor");
      reset(result);
    }

    descriptor(const tensor::descriptor &x_desc,
        const tensor::descriptor &gradw_desc,
        const tensor::descriptor &grady_desc)
    : hint_(x_desc, gradw_desc, grady_desc) {
      mkldnn_inner_product_desc_t data;
      auto src_data = x_desc.format_any();
      auto diff_dst_data = grady_desc.format_any();
      auto diff_weights_data = gradw_desc.format_any();
      error::wrap_c_api(
          mkldnn_inner_product_backward_weights_desc_init(
          &data, &src_data, &diff_weights_data,
          nullptr, &diff_dst_data),
          "could not create a inner product backward weights descriptor");
      mkldnn_primitive_desc_t result;
      error::wrap_c_api(mkldnn_primitive_desc_create(&result,
            &data, engine::cpu_engine().get(), hint_.get()),
    "cld not create a inner product backward weights primitive descriptor");
      reset(result);
    }
  private:
    inner_product_forward::descriptor hint_;
  };
public:
  using computation::computation;
  using computation::expected_gradw_descriptor;
  using computation::expected_gradb_descriptor;

  template <typename ...Ts>
  void init(const tensor::descriptor &x_desc,
      const tensor::descriptor &grady_desc,
      const tensor::descriptor &gradw_desc, Ts&&... args) {
    descriptor backward_weights_descriptor(x_desc, grady_desc, gradw_desc,
        std::forward<Ts>(args)...);
    computation::init(backward_weights_descriptor, x_desc, grady_desc);
  }

  void execute(const tensor& x, const tensor& grady, const tensor& gradw) {
    computation::execute(x, grady, gradw);
  }

  void execute(const tensor& x, const tensor& grady, const tensor& gradw
      , const tensor& gradb) {
    computation::execute(x, grady, gradw, gradb);
  }
};

struct eltwise_binary {
public:
  enum eltwise_binary_op {
    ELTWISE_ADD,
    ELTWISE_MUL,
    ELTWISE_DIV,
  };

  eltwise_binary() = default;

  static void compute(eltwise_binary_op op, tensor &inputA, tensor &inputB,
      tensor &outputC) {
    assert(inputA.ndims() >= inputB.ndims());
    assert(inputA.get_descriptor() == outputC.get_descriptor());
    if (inputA.get_dims() == inputB.get_dims()) {
      auto* inputB_data = inputB.get_data_handle();
      tensor scratch_tensor;
      if (inputA.get_internal_format() != inputB.get_internal_format()) {
        scratch_tensor.init(inputA.get_descriptor());
        reorder::compute(inputB, scratch_tensor);
        inputB_data = scratch_tensor.get_data_handle();
      }
      switch (op) {
      case ELTWISE_ADD:
        utils::fast_math<utils::cpu_isa_t::avx2>::add<float>(
            static_cast<float*>(outputC.get_data_handle()),
            static_cast<float*>(inputA.get_data_handle()),
            static_cast<float*>(inputB_data),
            static_cast<unsigned>(inputA.get_nelems()));
        return;
      case ELTWISE_MUL:
      case ELTWISE_DIV:
      default:
        throw error(mkldnn_unimplemented, "Not implemented!");
      }
    } else {
      throw error(mkldnn_runtime_error, "Not implemented!");
    }
  }
};

} // namespace mkldnn

#endif
