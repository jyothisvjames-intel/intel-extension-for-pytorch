#ifndef IDEEP_OPERATORS_DECONV_HPP
#define IDEEP_OPERATORS_DECONV_HPP

namespace ideep {

struct convolution_transpose_forward : public dnnl::deconvolution_forward {
  using super = dnnl::deconvolution_forward;

  static void compute(
      const tensor& src,
      const tensor& weights, // dim: {i, o[, d], h, w}
      const tensor& bias,
      const dims& dst_dims,
      tensor& dst,
      const dims& strides,
      const dims& padding_l,
      const dims& padding_r,
      const dims& dilates = {1, 1},
      int groups = 1,
      const attr_t& attr = attr_t(),
      algorithm aalgorithm = algorithm::deconvolution_direct,
      prop_kind aprop_kind = prop_kind::forward,
      const engine& aengine = engine::cpu_engine()) {
    compute_impl</*with_bias=*/true>(
        src,
        weights,
        bias,
        dst_dims,
        dst,
        strides,
        dilates,
        padding_l,
        padding_r,
        groups,
        attr,
        aalgorithm,
        aprop_kind,
        aengine);
  }

  static void compute(
      const tensor& src,
      const tensor& weights, // dim: {i, o[, d], h, w}
      const dims& dst_dims,
      tensor& dst,
      const dims& strides,
      const dims& padding_l,
      const dims& padding_r,
      const dims& dilates = {1, 1},
      int groups = 1,
      const attr_t& attr = attr_t(),
      algorithm aalgorithm = algorithm::deconvolution_direct,
      prop_kind aprop_kind = prop_kind::forward,
      const engine& aengine = engine::cpu_engine()) {
    static tensor dummy_bias;
    compute_impl</*with_bias=*/false>(
        src,
        weights,
        dummy_bias,
        dst_dims,
        dst,
        strides,
        dilates,
        padding_l,
        padding_r,
        groups,
        attr,
        aalgorithm,
        aprop_kind,
        aengine);
  }

  template <bool channels_last = false>
  static tensor::desc expected_weights_desc(
      const dims& weights_dims, // [i, o, ...]
      data_type dtype = data_type::f32,
      const dims& strides = {1, 1},
      const dims& padding_l = {0, 0},
      const dims& padding_r = {0, 0},
      const dims& dilates = {1, 1},
      int groups = 1,
      algorithm aalgorithm = algorithm::deconvolution_direct,
      prop_kind aprop_kind = prop_kind::forward,
      const dims& src_dims = dims(),
      const attr_t& attr = attr_t(),
      const engine& aengine = engine::cpu_engine()) {
    auto src_size =
        weights_dims.size(); // weights_dims is 4 for conv2d and 5 for conv3d
    auto grouped = groups > 1;
    auto weights_dims_g =
        grouped ? utils::group_dims(weights_dims, groups) : weights_dims;
    // (g)iohw -> (g)oihw
    std::swap(weights_dims_g[grouped + 0], weights_dims_g[grouped + 1]);
    auto weights_desc = tensor::desc(weights_dims_g, dtype);

    auto dims_in = weights_desc.get_dims();
    auto ndims = dims_in.size();
    auto g = grouped ? dims_in[0] : 1;
    auto dilates_ = utils::get_compatible_dilates(dilates);

    dims x_dims, y_dims, kernel_size;
    auto ic = g * dims_in[1 + grouped];
    auto oc = g * dims_in[0 + grouped];
    if (5 == src_size) {
      kernel_size.push_back(dims_in[ndims - 3]);
    }
    kernel_size.push_back(dims_in[ndims - 2]);
    kernel_size.push_back(dims_in[ndims - 1]);
    if (src_dims.empty()) {
      // Construct a dummy case
      x_dims.push_back(1);
      x_dims.push_back(ic);
      y_dims.push_back(1);
      y_dims.push_back(oc);
      auto valid_x_dim = [=](int idx) {
        return std::max(
            (padding_l[idx] + padding_r[idx] -
             (1 + (kernel_size[idx] - 1) * dilates[idx])) /
                    strides[idx] +
                2,
            2 * kernel_size[idx]);
      };
      if (4 == src_size) {
        x_dims.push_back(valid_x_dim(0));
        x_dims.push_back(valid_x_dim(1));
      } else {
        x_dims.push_back(valid_x_dim(0));
        x_dims.push_back(valid_x_dim(1));
        x_dims.push_back(valid_x_dim(2));
      }
    } else {
      // Use the real data
      for (auto i = 0; i < src_size; ++i) {
        x_dims.push_back(src_dims[i]);
      }
      y_dims.push_back(src_dims[0]);
      y_dims.push_back(oc);
    }
    for (auto d = 2; d < src_size; ++d) {
      auto out_size = (x_dims[d] - 1) * strides[d - 2] +
          (1 + (kernel_size[d - 2] - 1) * (dilates[d - 2])) - padding_l[d - 2] -
          padding_r[d - 2];
      y_dims.push_back(out_size);
    }
    auto x_dtype = (dtype != data_type::s8) ? dtype : data_type::u8;
    auto y_dtype = (dtype != data_type::s8) ? dtype : data_type::s32;

    tensor::desc src_desc(x_dims, x_dtype);
    tensor::desc dst_desc(y_dims, y_dtype);

    if (channels_last) {
      src_desc = src_desc.to_format(5 == src_size ? tag::ndhwc : tag::nhwc);
      dst_desc = dst_desc.to_format(5 == src_size ? tag::ndhwc : tag::nhwc);
    }

    auto pd = get_primitive_desc</*with_bias=*/false>(
        src_desc,
        weights_desc,
        tensor::desc(),
        dst_desc,
        strides,
        dilates_,
        padding_l,
        padding_r,
        attr_t(),
        aalgorithm,
        aprop_kind);

    // embed group info into weights_desc
    if (grouped) {
      // [g, o, i/g, ...] -> [g, i/g, o, ...]
      return tensor::desc(pd.weights_desc(), groups);
    } else {
      // [o, i, ...] -> [i, o, ...]
      return tensor::desc(pd.weights_desc(), groups);
    }
  }

  template <bool with_bias>
  static primitive_desc get_primitive_desc(
      const tensor::desc& src_desc,
      const tensor::desc& weights_desc,
      const tensor::desc& bias_desc,
      const tensor::desc& dst_desc,
      const dims& strides,
      const dims& dilates,
      const dims& padding_l,
      const dims& padding_r,
      const attr_t& attr = attr_t(),
      algorithm aalgorithm = algorithm::deconvolution_direct,
      prop_kind aprop_kind = prop_kind::forward,
      const engine& aengine = engine::cpu_engine()) {
    // For nhwc path, weight uses format_tag::any,
    // while activation uses format_tag::nhwc
    bool is_nhwc = src_desc.is_nhwc() || weights_desc.is_nhwc();
    bool is_ndhwc = src_desc.is_ndhwc() || weights_desc.is_ndhwc();
    auto format_tag = is_nhwc ? tag::nhwc : (is_ndhwc ? tag::ndhwc : tag::any);
    auto src_desc_query = src_desc.to_format(format_tag);
    auto weights_desc_query = weights_desc.to_format_any();
    auto bias_desc_query =
        with_bias ? bias_desc.to_format_any() : tensor::desc();
    auto dst_desc_query = dst_desc.to_format(format_tag);

    if (with_bias) {
      return primitive_desc(
          {aprop_kind,
           aalgorithm,
           src_desc_query,
           weights_desc_query,
           bias_desc_query,
           dst_desc_query,
           strides,
           dilates,
           padding_l,
           padding_r},
          attr,
          aengine);
    } else {
      return primitive_desc(
          {aprop_kind,
           aalgorithm,
           src_desc_query,
           weights_desc_query,
           dst_desc_query,
           strides,
           dilates,
           padding_l,
           padding_r},
          attr,
          aengine);
    }
  }

 private:
  template <bool with_bias>
  static void compute_impl(
      const tensor& src,
      const tensor& weights,
      const tensor& bias,
      const dims& dst_dims,
      tensor& dst,
      const dims& strides,
      const dims& dilates,
      const dims& padding_l,
      const dims& padding_r,
      int groups,
      const attr_t& attr,
      algorithm aalgorithm,
      prop_kind aprop_kind,
      const engine& aengine) {
    attr_t op_attr = attr;
    // make weights and dilates compatible with DNNL
    auto weights_ = weights.make_grouped_weights(groups, true);
    auto dilates_ = utils::get_compatible_dilates(dilates);

    tensor::desc dst_desc(dst_dims, src.get_data_type());

    // Use user mode scratchpad
    op_attr.set_scratchpad_mode(dnnl::scratchpad_mode::user);

    auto pd = get_primitive_desc<with_bias>(
        src.get_desc(),
        weights_.get_desc(),
        bias.get_desc(),
        dst_desc,
        strides,
        dilates_,
        padding_l,
        padding_r,
        op_attr,
        aalgorithm,
        aprop_kind,
        aengine);

    tensor scratchpad(pd.scratchpad_desc());
    auto expected_src = src.reorder_if_differ_in(pd.src_desc());
    auto expected_weights = weights_.reorder_if_differ_in(pd.weights_desc());
    dst.reinit_if_possible(pd.dst_desc());

    if (with_bias) {
      auto expected_bias = bias.reorder_if_differ_in(pd.bias_desc());
      super(pd).execute(
          stream::default_stream(),
          {{DNNL_ARG_SRC, expected_src},
           {DNNL_ARG_WEIGHTS, expected_weights},
           {DNNL_ARG_BIAS, expected_bias},
           {DNNL_ARG_DST, dst},
           {DNNL_ARG_SCRATCHPAD, scratchpad}});
    } else {
      super(pd).execute(
          stream::default_stream(),
          {{DNNL_ARG_SRC, expected_src},
           {DNNL_ARG_WEIGHTS, expected_weights},
           {DNNL_ARG_DST, dst},
           {DNNL_ARG_SCRATCHPAD, scratchpad}});
    }
  }
};

struct convolution_transpose_backward_data
    : public dnnl::deconvolution_backward_data {
  using super = dnnl::deconvolution_backward_data;

  static void compute(
      const tensor& diff_dst,
      const tensor& weights, // dim: {i, o[, d], h, w}
      const dims& diff_src_dims,
      tensor& diff_src,
      const dims& strides,
      const dims& padding_l,
      const dims& padding_r,
      const dims& dilates = {1, 1},
      const int groups = 1,
      algorithm aalgorithm = algorithm::deconvolution_direct,
      const engine& aengine = engine::cpu_engine()) {
    // make weights and dilates compatible with DNNL
    auto weights_ = weights.make_grouped_weights(groups, true);
    auto dilates_ = utils::get_compatible_dilates(dilates);

    bool is_nhwc = diff_dst.get_desc().is_nhwc();
    bool is_ndhwc = diff_dst.get_desc().is_ndhwc();
    auto format_tag = is_nhwc ? tag::nhwc : (is_ndhwc ? tag::ndhwc : tag::any);
    auto diff_dst_desc = diff_dst.get_desc().to_format(format_tag);
    auto weights_desc = weights_.get_desc().to_format_any();

    tensor::desc diff_src_desc(
        diff_src_dims, diff_dst_desc.get_data_type(), format_tag);

    auto forward_hints =
        convolution_transpose_forward::get_primitive_desc</*with_bias=*/false>(
            diff_src_desc,
            weights_desc,
            tensor::desc(),
            diff_dst_desc,
            strides,
            dilates_,
            padding_l,
            padding_r);

    // Use user mode scratchpad
    auto op_attr = dnnl::primitive_attr();
    op_attr.set_scratchpad_mode(dnnl::scratchpad_mode::user);

    auto pd = primitive_desc(
        {aalgorithm,
         diff_src_desc,
         weights_desc,
         diff_dst_desc,
         strides,
         dilates_,
         padding_l,
         padding_r},
        op_attr,
        aengine,
        forward_hints);

    auto expected_diff_dst = diff_dst.reorder_if_differ_in(pd.diff_dst_desc());
    auto expected_weights = weights_.reorder_if_differ_in(pd.weights_desc());
    diff_src.reinit_if_possible(pd.diff_src_desc());
    tensor scratchpad(pd.scratchpad_desc());

    super(pd).execute(
        stream::default_stream(),
        {{DNNL_ARG_DIFF_DST, expected_diff_dst},
         {DNNL_ARG_WEIGHTS, expected_weights},
         {DNNL_ARG_DIFF_SRC, diff_src},
         {DNNL_ARG_SCRATCHPAD, scratchpad}});
  }
};

struct convolution_transpose_backward_weights
    : public dnnl::deconvolution_backward_weights {
  using super = dnnl::deconvolution_backward_weights;

  static void compute(
      const tensor& src,
      const tensor& diff_dst,
      const dims& diff_weights_dims,
      tensor& diff_weights,
      tensor& diff_bias,
      const dims& strides,
      const dims& padding_l,
      const dims& padding_r,
      const dims& dilates = {1, 1},
      const int groups = 1,
      algorithm aalgorithm = algorithm::deconvolution_direct,
      const engine& aengine = engine::cpu_engine()) {
    compute_impl</*with_diff_bias=*/true>(
        src,
        diff_dst,
        diff_weights_dims,
        diff_weights,
        diff_bias,
        strides,
        dilates,
        padding_l,
        padding_r,
        groups,
        aalgorithm,
        aengine);
  }

  static void compute(
      const tensor& src,
      const tensor& diff_dst,
      const dims& diff_weights_dims,
      tensor& diff_weights,
      const dims& strides,
      const dims& padding_l,
      const dims& padding_r,
      const dims& dilates = {1, 1},
      const int groups = 1,
      algorithm aalgorithm = algorithm::deconvolution_direct,
      const engine& aengine = engine::cpu_engine()) {
    static tensor dummy_diff_bias;
    compute_impl</*with_diff_bias=*/false>(
        src,
        diff_dst,
        diff_weights_dims,
        diff_weights,
        dummy_diff_bias,
        strides,
        dilates,
        padding_l,
        padding_r,
        groups,
        aalgorithm,
        aengine);
  }

 private:
  template <bool with_diff_bias>
  static void compute_impl(
      const tensor& src,
      const tensor& diff_dst,
      const dims& diff_weights_dims, // [i, o, ...]
      tensor& diff_weights,
      tensor& diff_bias,
      const dims& strides,
      const dims& dilates,
      const dims& padding_l,
      const dims& padding_r,
      const int groups,
      algorithm aalgorithm,
      const engine& aengine) {
    // make diff_weights and dilates compatible with DNNL
    auto dilates_ = utils::get_compatible_dilates(dilates);

    // dim: [i, o, ...]
    auto diff_weights_desc =
        tensor::desc(diff_weights_dims, diff_dst.get_data_type(), tag::any);

    if (groups > 1) {
      // dim: [g, o, i/g, ...]
      diff_weights_desc = diff_weights_desc.to_grouped(groups).transpose(1, 2);
    } else {
      // dim: [o, i, ...]
      diff_weights_desc = diff_weights_desc.transpose(0, 1);
    }

    bool is_nhwc = diff_dst.get_desc().is_nhwc();
    bool is_ndhwc = diff_dst.get_desc().is_ndhwc();
    auto format_tag = is_nhwc ? tag::nhwc : (is_ndhwc ? tag::ndhwc : tag::any);
    auto diff_dst_desc = diff_dst.get_desc().to_format(format_tag);
    auto src_desc = src.get_desc().to_format(format_tag);

    auto diff_bias_desc = with_diff_bias
        ? tensor::desc({diff_dst.get_dim(1)}, diff_dst.get_data_type())
              .to_format_any()
        : tensor::desc();

    auto forward_hints =
        convolution_transpose_forward::get_primitive_desc<with_diff_bias>(
            src_desc,
            diff_weights_desc,
            diff_bias_desc,
            diff_dst_desc,
            strides,
            dilates_,
            padding_l,
            padding_r,
            attr_t(),
            aalgorithm,
            prop_kind::forward,
            aengine);

    // Use user mode scratchpad
    auto op_attr = dnnl::primitive_attr();
    op_attr.set_scratchpad_mode(dnnl::scratchpad_mode::user);

    auto pd = with_diff_bias ? primitive_desc(
                                   {aalgorithm,
                                    src_desc,
                                    diff_weights_desc,
                                    diff_bias_desc,
                                    diff_dst_desc,
                                    strides,
                                    dilates_,
                                    padding_l,
                                    padding_r},
                                   op_attr,
                                   aengine,
                                   forward_hints)
                             : primitive_desc(
                                   {aalgorithm,
                                    src_desc,
                                    diff_weights_desc,
                                    diff_dst_desc,
                                    strides,
                                    dilates_,
                                    padding_l,
                                    padding_r},
                                   op_attr,
                                   aengine,
                                   forward_hints);

    auto expected_diff_dst = diff_dst.reorder_if_differ_in(pd.diff_dst_desc());
    auto expected_src = src.reorder_if_differ_in(pd.src_desc());
    // embed group info into diff_weights_desc
    auto expected_diff_weights_desc =
        tensor::desc(pd.diff_weights_desc(), groups);

    tensor expected_diff_weights;
    expected_diff_weights.init(expected_diff_weights_desc);
    tensor scratchpad(pd.scratchpad_desc());

    if (with_diff_bias) {
      diff_bias.reinit_if_possible(pd.diff_bias_desc());
      super(pd).execute(
          stream::default_stream(),
          {{DNNL_ARG_DIFF_DST, expected_diff_dst},
           {DNNL_ARG_SRC, expected_src},
           {DNNL_ARG_DIFF_WEIGHTS, expected_diff_weights},
           {DNNL_ARG_DIFF_BIAS, diff_bias},
           {DNNL_ARG_SCRATCHPAD, scratchpad}});
    } else {
      super(pd).execute(
          stream::default_stream(),
          {{DNNL_ARG_DIFF_DST, expected_diff_dst},
           {DNNL_ARG_SRC, expected_src},
           {DNNL_ARG_DIFF_WEIGHTS, expected_diff_weights},
           {DNNL_ARG_SCRATCHPAD, scratchpad}});
    }

    diff_weights.feed_from(expected_diff_weights);

    // recover output dims to align with pytorch
    if (groups > 1) {
      // [g, o, i/g, ...] -> [g, i/g, o, ...]
      diff_weights.transpose_(1, 2);
    } else {
      // [o, i, ...] -> [i, o, ...]
      diff_weights.transpose_(0, 1);
    }
  }
};
} // namespace ideep

#endif
