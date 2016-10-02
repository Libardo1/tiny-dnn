/*
    Copyright (c) 2013, Taiga Nomi
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once
#include <algorithm>
#include <memory>
#include <vector>
#include <map>
#include <string>
#include <limits>
#include <unordered_map>

#include "caffe.pb.h"

#include "tiny_dnn/layers/convolutional_layer.h"
#include "tiny_dnn/layers/deconvolutional_layer.h"
#include "tiny_dnn/layers/fully_connected_layer.h"
#include "tiny_dnn/layers/average_pooling_layer.h"
#include "tiny_dnn/layers/max_pooling_layer.h"
#include "tiny_dnn/layers/linear_layer.h"
#include "tiny_dnn/layers/lrn_layer.h"
#include "tiny_dnn/layers/dropout_layer.h"
#include "tiny_dnn/io/caffe/layer_manager.h"

typedef tiny_dnn::shape3d shape_t;

#ifdef _MSC_VER
#define _NOMINMAX
#include <io.h>
#include <fcntl.h>
#define CNN_OPEN_BINARY(filename) open(filename, _O_RDONLY|_O_BINARY)
#define CNN_OPEN_TXT(filename) open(filename, _O_RDONLY)
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#define CNN_OPEN_BINARY(filename) open(filename, O_RDONLY)
#define CNN_OPEN_TXT(filename) open(filename, O_RDONLY)
#endif

namespace tiny_dnn {
namespace detail {

inline void read_proto_from_text(const std::string& prototxt,
                                 google::protobuf::Message *message) {
    int fd = CNN_OPEN_TXT(prototxt.c_str());
    if (fd == -1) {
        throw nn_error("file not fonud: " + prototxt);
    }

    google::protobuf::io::FileInputStream input(fd);
    input.SetCloseOnDelete(true);

    if (!google::protobuf::TextFormat::Parse(&input, message)) {
        throw nn_error("failed to parse");
    }
}

inline void read_proto_from_binary(const std::string& protobinary,
                                   google::protobuf::Message *message) {
    int fd = CNN_OPEN_BINARY(protobinary.c_str());
    google::protobuf::io::FileInputStream rawstr(fd);
    google::protobuf::io::CodedInputStream codedstr(&rawstr);

    rawstr.SetCloseOnDelete(true);
    codedstr.SetTotalBytesLimit(std::numeric_limits<int>::max(),
                                std::numeric_limits<int>::max() / 2);

    if (!message->ParseFromCodedStream(&codedstr)) {
        throw nn_error("failed to parse");
    }
}

inline std::shared_ptr<weight_init::function>
create_filler(const std::string& filler) {
    if (filler == "xavier") {
        return std::make_shared<weight_init::xavier>();
    } else if (filler == "constant") {
        return std::make_shared<weight_init::constant>();
    } else if (filler == "gaussian") {
        return std::make_shared<weight_init::gaussian>();
    } else {
        throw nn_error("unsupported filler type");
    }
}

template <typename param>
inline bool get_kernel_size_2d(const param& p, layer_size_t *kernel) {
    if (p.has_kernel_w() && p.has_kernel_w()) {
        if (p.kernel_w() != p.kernel_h()) {
            throw nn_error("unsupported kernel shape");
        }
        *kernel = p.kernel_w();
        return true;
    }
    return false;
}

inline layer_size_t get_kernel_size_2d(const caffe::ConvolutionParameter& p) {
    layer_size_t window_size;
    if (!get_kernel_size_2d(p, &window_size)) {
        if (p.kernel_size_size() > 1) {
            throw nn_error("unsupported kernel shape");
        }
        window_size = p.kernel_size(0);
    }
    return window_size;
}

inline std::shared_ptr<layer> create_max_pool(int pool_size,
                                              int stride,
                                              const caffe::LayerParameter& layer,
                                              caffe_layer_manager& layer_manager) {
    using max_pool = max_pooling_layer<activation::identity>;

    auto bottom_shape = layer_manager.bottom_shape(layer);

    auto mp = std::make_shared<max_pool>(bottom_shape.width_,
                                         bottom_shape.height_,
                                         bottom_shape.depth_,
                                         pool_size, stride);

    layer_manager.register_top_shape(layer, mp->out_shape()[0]);

    mp->init_weight();

    return mp;
}

inline std::shared_ptr<layer> create_ave_pool(int pool_size,
                                              int stride,
                                              const caffe::LayerParameter& layer,
                                              caffe_layer_manager& layer_manager) {
    using ave_pool = average_pooling_layer<activation::identity>;

    auto bottom_shape = layer_manager.bottom_shape(layer);

    auto ap = std::make_shared<ave_pool>(bottom_shape.width_,
                                         bottom_shape.height_,
                                         bottom_shape.depth_,
                                         pool_size, stride);

    // tiny-dnn has trainable parameter in average-pooling layer
    float_t weight = float_t(1) / sqr(pool_size);

    vec_t& w = *ap->weights()[0];
    vec_t& b = *ap->weights()[1];

    std::fill(w.begin(), w.end(), weight);
    std::fill(b.begin(), b.end(), float_t(0));

    layer_manager.register_top_shape(layer, ap->out_shape()[0]);

    ap->init_weight();

    return ap;
}

template <typename Activation>
std::shared_ptr<layer> create_activation(const caffe::LayerParameter& layer,
                                      caffe_layer_manager& layer_manager) {
    auto bottom_shape = layer_manager.bottom_shape(layer);
    auto act = std::make_shared<linear_layer<Activation>>(bottom_shape.size());

    act->init_weight();
    layer_manager.register_top_shape(layer, bottom_shape);

    return act;
}

inline
std::shared_ptr<layer> create_pooling(const caffe::LayerParameter& layer,
                                      caffe_layer_manager& layer_manager) {
    if (!layer.has_pooling_param()) {
        throw nn_error("pool param missing");
    }

    auto pool_param = layer.pooling_param();

    layer_size_t h_stride = 0;
    layer_size_t w_stride = 0;
    layer_size_t pool_size = 0;

    if (!get_kernel_size_2d(pool_param, &pool_size)) {
        pool_size = pool_param.kernel_size();
    }

    if (pool_param.has_stride() || pool_param.has_stride_h()) {
        h_stride = pool_param.has_stride() ?
                   pool_param.stride() : pool_param.stride_h();
    }

    if (pool_param.has_stride() || pool_param.has_stride_w()) {
        w_stride = pool_param.has_stride() ?
                   pool_param.stride() : pool_param.stride_w();
    }

    if (h_stride != w_stride) {  // || h_stride != pool_size)
        throw nn_error("unsupported pool shape");
    }

    if (pool_param.has_pool()) {
        auto type = pool_param.pool();

        switch (type) {
            case caffe::PoolingParameter_PoolMethod_MAX:
                return create_max_pool(pool_size, h_stride, layer, layer_manager);
            case caffe::PoolingParameter_PoolMethod_AVE:
                return create_ave_pool(pool_size, h_stride, layer, layer_manager);
            default:
                throw nn_error("unsupported layer type");
        }
    }

    // default: max-pool
    return create_max_pool(pool_size, h_stride, layer, layer_manager);
}

inline std::shared_ptr<layer> create_batchnorm(const caffe::LayerParameter& layer,
                                               caffe_layer_manager& layer_manager) {
    using bn_layer = batch_normalization_layer;

    float_t eps = 1e-5;
    float_t momentum = 0.999;

    if (layer.has_batch_norm_param()) {
        auto bn_param = layer.batch_norm_param();

        if (bn_param.has_eps()) {
            eps = bn_param.eps();
        }
        if (bn_param.has_moving_average_fraction()) {
            momentum = bn_param.moving_average_fraction();
        }
    }

    auto bottom_shape = layer_manager.bottom_shape(layer);

    auto bn = std::make_shared<bn_layer>(bottom_shape.area(), bottom_shape.depth_, eps, momentum, net_phase::test);

    layer_manager.register_top_shape(layer, bottom_shape);

    // weight
    if (layer.blobs_size() > 0) {
        auto global_stats = layer.blobs();
        if (global_stats.size() != 3) {
            throw std::runtime_error("unexpected bn stored statistics");
        }       

        float_t scale_factor = global_stats.Get(2).data(0) == 0 ? 0 : 1 / global_stats.Get(2).data(0);
        vec_t mean(bottom_shape.depth_);
        vec_t variance(bottom_shape.depth_);

        for (size_t i = 0; i < mean.size(); i++) {
            mean[i]     = global_stats.Get(0).data(i) * scale_factor;
            variance[i] = global_stats.Get(1).data(i) * scale_factor;
        }
        bn->set_mean(mean);
        bn->set_variance(variance);
    }

    return bn;
}


inline void load_weights_fullyconnected(const caffe::LayerParameter& src,
                                        layer *dst) {
    auto weights = src.blobs(0);
    int curr = 0;

    if (dst->out_size() * dst->in_size() !=
        static_cast<cnn_size_t>(weights.data_size())) {
        throw nn_error(
            std::string("layer size mismatch!") +
            "caffe(" + src.name() + "):" + to_string(weights.data_size()) + "\n" +
            "tiny-dnn(" + dst->layer_type() + "):" + to_string(dst->weights().size()));
    }

    vec_t& w = *dst->weights()[0];
    vec_t& b = *dst->weights()[1];

    // fill weights
    for (size_t o = 0; o < dst->out_size(); o++) {
        for (size_t i = 0; i < dst->in_size(); i++) {
            // TODO: how to access to weights?
            //dst->weight()[i * dst->out_size() + o] = weights.data(curr++); // transpose
            w[i * dst->out_size() + o] = weights.data(curr++); // transpose
        }
    }

    // fill bias
    if (src.inner_product_param().bias_term()) {
        auto biases = src.blobs(1);
        for (size_t o = 0; o < dst->out_size(); o++) {
            // TODO: how to access to biases?
            //dst->bias()[o] = biases.data(o);
            b[o] = biases.data(o);
        }
    }
}

inline std::shared_ptr<layer> create_fullyconnected(
        const caffe::LayerParameter& layer,
        caffe_layer_manager& layer_manager) {
    using fc_layer = fully_connected_layer<activation::identity>;

    if (!layer.has_inner_product_param()) {
        throw nn_error("inner-product param missing");
    }

    layer_size_t dim_input = 0, dim_output = 0;
    bool has_bias = true;

    auto ip_param = layer.inner_product_param();
    has_bias = ip_param.bias_term();

    dim_output = ip_param.num_output();
    dim_input = layer_manager.bottom_shape(layer).size();

    auto ip = std::make_shared<fc_layer>(dim_input, dim_output, has_bias);

    // filler
    if (ip_param.has_weight_filler()) {
        ip->weight_init(create_filler(ip_param.weight_filler().type()));
    }

    if (ip_param.has_bias_filler()) {
        ip->bias_init(create_filler(ip_param.bias_filler().type()));
    }

    // weight
    if (layer.blobs_size() > 0) {
        load_weights_fullyconnected(layer, ip.get());
    }

    layer_manager.register_top_shape(layer, ip->out_shape()[0]);
    return ip;
}

inline void load_weights_conv(const caffe::LayerParameter& src, layer *dst) {
    // fill weight
    auto weights = src.blobs(0);

    //TODO: check if it works
    //int out_channels = dst->out_shape().depth_;
    //int in_channels = dst->in_shape().depth_;
    int out_channels = dst->out_data_shape()[0].depth_;
    int in_channels = dst->in_data_shape()[0].depth_;

    connection_table table;
    auto conv_param = src.convolution_param();
    int dst_idx = 0;
    int src_idx = 0;
    int window_size = get_kernel_size_2d(conv_param);

    if (conv_param.has_group()) {
        table = connection_table(conv_param.group(), in_channels, out_channels);
    }

    vec_t& w = *dst->weights()[0];
    vec_t& b = *dst->weights()[1];

    // fill weights
    for (int o = 0; o < out_channels; o++) {
        for (int i = 0; i < in_channels; i++) {
            if (!table.is_connected(o, i)) {
                dst_idx += window_size * window_size;
                continue;
            }
            for (int x = 0; x < window_size * window_size; x++) {
                //TODO
                //dst->weight()[dst_idx++] = weights.data(src_idx++);
                w[dst_idx++] =  weights.data(src_idx++);
            }
        }
    }

    // fill bias
    if (conv_param.bias_term()) {
        auto biases = src.blobs(1);
        for (int o = 0; o < out_channels; o++) {
            //TODO
            //dst->bias()[o] = biases.data(o);
            b[o] = biases.data(o);
        }
    }
}

inline void load_weights_deconv(const caffe::LayerParameter& src, layer *dst) {
    // fill weight
    auto weights = src.blobs(0);

    //TODO: check if it works
    //int out_channels = dst->out_shape().depth_;
    //int in_channels = dst->in_shape().depth_;
    int out_channels = dst->out_data_shape()[0].depth_;
    int in_channels = dst->in_data_shape()[0].depth_;

    connection_table table;
    auto deconv_param = src.convolution_param();
    int dst_idx = 0;
    int src_idx = 0;
    int window_size = get_kernel_size_2d(deconv_param);

    if (deconv_param.has_group()) {
        table = connection_table(deconv_param.group(), in_channels, out_channels);
    }

    vec_t& w = *dst->weights()[0];
    vec_t& b = *dst->weights()[1];

    // fill weights
    for (int o = 0; o < out_channels; o++) {
        for (int i = 0; i < in_channels; i++) {
            if (!table.is_connected(o, i)) {
                dst_idx += window_size * window_size;
                continue;
            }
            for (int x = 0; x < window_size * window_size; x++) {
                //TODO
                //dst->weight()[dst_idx++] = weights.data(src_idx++);
                w[dst_idx++] =  weights.data(src_idx++);
            }
        }
    }

    // fill bias
    if (deconv_param.bias_term()) {
        auto biases = src.blobs(1);
        for (int o = 0; o < out_channels; o++) {
            //TODO
            //dst->bias()[o] = biases.data(o);
            b[o] = biases.data(o);
        }
    }
}

inline void load_weights_pool(const caffe::LayerParameter& src, layer *dst) {
    auto pool_param = src.pooling_param();

    //TODO
    //if (dst->weight().size()) {
    if (dst->weights().size()) {
        layer_size_t pool_size = 0;

        if (!get_kernel_size_2d(pool_param, &pool_size)) {
            pool_size = pool_param.kernel_size();
        }

        // tiny-dnn has trainable parameter in average-pooling layer
        float_t weight = float_t(1) / sqr(pool_size);

        //TODO
        /*if (!dst->weight().empty()) {
            std::fill(dst->weight().begin(), dst->weight().end(), weight);
        }
        if (!dst->bias().empty()) {
            std::fill(dst->bias().begin(), dst->bias().end(), float_t(0));
            dst->init_bias();
        }*/

        vec_t& w = *dst->weights()[0];
        vec_t& b = *dst->weights()[1];

        if (!w.empty()) {
            std::fill(w.begin(), w.end(), weight);
        }
        if (!b.empty()) {
            std::fill(b.begin(), b.end(), float_t(0));
            //dst->init_bias();
        }
    }
}

inline
std::shared_ptr<layer> create_lrn(const caffe::LayerParameter& layer,
                                  caffe_layer_manager& layer_manager) {
    using lrn_layer = lrn_layer<activation::identity>;

    if (!layer.has_lrn_param()) {
        throw nn_error("lrn param missing");
    }

    auto lrn_param = layer.lrn_param();
    layer_size_t local_size = 5;
    float_t alpha = 1;
    float_t beta = 5;
    norm_region region = norm_region::across_channels;
    auto bottom_shape = layer_manager.bottom_shape(layer);

    if (lrn_param.has_local_size()) local_size = lrn_param.local_size();
    if (lrn_param.has_alpha()) alpha = lrn_param.alpha();
    if (lrn_param.has_beta()) beta = lrn_param.beta();
    if (lrn_param.has_norm_region()) {
        if (lrn_param.norm_region() == caffe::LRNParameter_NormRegion_WITHIN_CHANNEL) // NOLINT
            region = norm_region::within_channels;
    }

    auto lrn = std::make_shared<lrn_layer>(bottom_shape.width_,
                                           bottom_shape.height_,
                                           local_size,
                                           bottom_shape.depth_,
                                           alpha, beta, region);

    layer_manager.register_top_shape(layer, bottom_shape);

    return lrn;
}

inline
std::shared_ptr<layer> create_dropout(const caffe::LayerParameter& layer,
                                      caffe_layer_manager& layer_manager) {
    if (!layer.has_dropout_param()) {
        throw nn_error("dropout param missing");
    }

    float_t dropout_rate = float_t(0.5);

    if (layer.dropout_param().has_dropout_ratio()) {
        dropout_rate = layer.dropout_param().dropout_ratio();
    }

    auto bottom_shape = layer_manager.bottom_shape(layer);
 
    auto dropout = std::make_shared<dropout_layer>(bottom_shape.size(),
                                                   dropout_rate,
                                                   net_phase::test);

    layer_manager.register_top_shape(layer, dropout->out_shape()[0]);

    return dropout;
}

inline
std::shared_ptr<layer> create_convlayer(const caffe::LayerParameter& layer,
                                        caffe_layer_manager& layer_manager) {
    using conv_layer = convolutional_layer<activation::identity>;

    if (!layer.has_convolution_param()) {
        throw nn_error("convolution param missing");
    }

    shape3d bottom_shape = layer_manager.bottom_shape(layer);

    // layer parameters
    layer_size_t in_width = 0, in_height = 0, window_size = 0;
    layer_size_t in_channels = 0, out_channels = 0;
    layer_size_t w_stride = 1, h_stride = 1;
    bool has_bias = true;
    padding pad_type = padding::valid;
    connection_table table;

    auto conv_param = layer.convolution_param();

    // shape
    out_channels = conv_param.num_output();
    in_channels = bottom_shape.depth_;
    in_width = bottom_shape.width_;
    in_height = bottom_shape.height_;
    has_bias = conv_param.bias_term();
    window_size = get_kernel_size_2d(conv_param);

    // padding
    if (conv_param.pad_size() == 1 ||
       (conv_param.has_pad_w() && conv_param.has_pad_h())) {
        uint32_t pad_w = conv_param.pad_size() == 1 ?
                         conv_param.pad(0) : conv_param.pad_w();

        uint32_t pad_h = conv_param.pad_size() == 1 ?
                         conv_param.pad(0) : conv_param.pad_h();

        if (pad_w != pad_h) {
            throw nn_error("conv:not supported padding size");
        }

        // 0 ... valid, (window_size-1)/2 ... same
        if (pad_w == (window_size - 1) / 2) {
            pad_type = padding::same;
        } else if (pad_w == 0) {
            pad_type = padding::valid;
        } else {
            throw nn_error("conv:not supported padding size");
        }
    }

    // stride
    if (conv_param.stride_size() == 1 || conv_param.has_stride_h()) {
        h_stride = conv_param.stride_size() == 1 ?
                   conv_param.stride(0) : conv_param.stride_h();
    }

    if (conv_param.stride_size() == 1 || conv_param.has_stride_w()) {
        w_stride = conv_param.stride_size() == 1 ?
                   conv_param.stride(0) : conv_param.stride_w();
    }

    // group
    if (conv_param.has_group()) {
        table = connection_table(conv_param.group(), in_channels, out_channels);
    }

    auto conv = std::make_shared<conv_layer>(in_width, in_height,
                                             window_size,
                                             in_channels, out_channels,
                                             table,
                                             pad_type,
                                             has_bias,
                                             w_stride, h_stride);
    // filler
    if (conv_param.has_weight_filler()) {
        conv->weight_init(create_filler(conv_param.weight_filler().type()));
    }

    if (conv_param.has_bias_filler()) {
        conv->bias_init(create_filler(conv_param.bias_filler().type()));
    }

    // set weight (optional)
    if (layer.blobs_size() > 0) {  // blobs(0)...weight, blobs(1)...bias
        load_weights_conv(layer, conv.get());
    }

    layer_manager.register_top_shape(layer, conv->out_shape()[0]);

    return conv;
}

inline
std::shared_ptr<layer> create_deconvlayer(const caffe::LayerParameter& layer,
                                          caffe_layer_manager& layer_manager) {
    using deconv_layer = deconvolutional_layer<activation::identity>;

    if (!layer.has_convolution_param()) {
        throw nn_error("deconvolution param missing");
    }

    shape3d bottom_shape = layer_manager.bottom_shape(layer);

    // layer parameters
    layer_size_t in_width = 0, in_height = 0, window_size = 0;
    layer_size_t in_channels = 0, out_channels = 0;
    layer_size_t w_stride = 1, h_stride = 1;
    bool has_bias = true;
    padding pad_type = padding::valid;
    connection_table table;

    auto deconv_param = layer.convolution_param();

    // shape
    out_channels = deconv_param.num_output();
    in_channels = bottom_shape.depth_;
    in_width = bottom_shape.width_;
    in_height = bottom_shape.height_;
    has_bias = deconv_param.bias_term();
    window_size = get_kernel_size_2d(deconv_param);

    // unpadding
    if (deconv_param.pad_size() == 1 ||
       (deconv_param.has_pad_w() && deconv_param.has_pad_h())) {
        uint32_t unpad_w = deconv_param.pad_size() == 1 ?
                         deconv_param.pad(0) : deconv_param.pad_w();

        uint32_t unpad_h = deconv_param.pad_size() == 1 ?
                         deconv_param.pad(0) : deconv_param.pad_h();

        if (unpad_w != unpad_h) {
            throw nn_error("deconv:not supported unpadding size");
        }

        // 0 ... valid, (window_size-1)/2 ... same
        if (unpad_w == (window_size - 1) / 2) {
            pad_type = padding::same;
        } else if (unpad_w == 0) {
            pad_type = padding::valid;
        } else {
            throw nn_error("deconv:not supported unpadding size");
        }
    }

    // stride
    if (deconv_param.stride_size() == 1 || deconv_param.has_stride_h()) {
        h_stride = deconv_param.stride_size() == 1 ?
                   deconv_param.stride(0) : deconv_param.stride_h();
    }

    if (deconv_param.stride_size() == 1 || deconv_param.has_stride_w()) {
        w_stride = deconv_param.stride_size() == 1 ?
                   deconv_param.stride(0) : deconv_param.stride_w();
    }

    // group
    if (deconv_param.has_group()) {
        table = connection_table(deconv_param.group(), in_channels, out_channels);
    }

    auto deconv = std::make_shared<deconv_layer>(in_width, in_height,
                                             window_size,
                                             in_channels, out_channels,
                                             table,
                                             pad_type,
                                             has_bias,
                                             w_stride, h_stride);
    // filler
    if (deconv_param.has_weight_filler()) {
        deconv->weight_init(create_filler(deconv_param.weight_filler().type()));
    }

    if (deconv_param.has_bias_filler()) {
        deconv->bias_init(create_filler(deconv_param.bias_filler().type()));
    }

    // set weight (optional)
    if (layer.blobs_size() > 0) {  // blobs(0)...weight, blobs(1)...bias
        load_weights_deconv(layer, deconv.get());
    }

    layer_manager.register_top_shape(layer, deconv->out_shape()[0]);

    return deconv;
}

inline bool layer_skipped(const std::string& type) {
    if (type == "Data" || type == "EuclideanLoss" || type == "Input" || type == "Accuracy") return true;
    return false;
}

inline bool layer_has_weights(const std::string& type) {
    static const char* activations[] = {
        "SoftmaxWithLoss", "SigmoidCrossEntropyLoss", "LRN", "Dropout",
        "ReLU", "Sigmoid", "TanH", "Softmax"
    };
    for (unsigned int i = 0; i < sizeof(activations) / sizeof(activations[0]); i++) {
        if (activations[i] == type) return false;
    }
    return true;
}

inline bool layer_supported(const std::string& type) {
    static const char* supported[] = {
        "InnerProduct", "Convolution", "Deconvolution", "Pooling",
        "LRN", "Dropout",
        "SoftmaxWithLoss", "SigmoidCrossEntropyLoss",
        "ReLU", "Sigmoid", "TanH", "Softmax", "BatchNorm"
    };

    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (supported[i] == type) return true;
    }
    return false;
}

inline bool layer_match(const std::string& caffetype,
                        const std::string& tiny_dnn_type) {
    const char* conversions[][2] = {
        { "InnerProduct", "fully-connected" },
        { "Convolution", "conv" },
        { "Deconvolution", "deconv" },
        { "Pooling", "ave-pool" },
        { "Pooling", "max-pool" }
    };

    for (size_t i = 0; i < sizeof(conversions) / sizeof(conversions[0]); i++) {
        if (conversions[i][0] == caffetype &&
            conversions[i][1] == tiny_dnn_type) return true;
    }
    return false;
}

inline std::shared_ptr<layer> create(const caffe::LayerParameter& layer,
                                     caffe_layer_manager& layers) {
    const std::string layer_type = layer.type();

    if (layer_type == "Convolution") {
        return detail::create_convlayer(layer, layers);
    }

    if (layer_type == "Deconvolution") {
        return detail::create_deconvlayer(layer, layers);
    }

    if (layer_type == "InnerProduct") {
        return detail::create_fullyconnected(layer, layers);
    }

    if (layer_type == "Pooling") {
        return detail::create_pooling(layer, layers);
    }

    if (layer_type == "BatchNorm") {
        return detail::create_batchnorm(layer, layers);
    }

    if (layer_type == "LRN") {
        return detail::create_lrn(layer, layers);
    }

    if (layer_type == "Dropout") {
        return detail::create_dropout(layer, layers);
    }

    if (layer_type == "SoftmaxWithLoss" ||
        layer_type == "Softmax") {
        return detail::create_activation<activation::softmax>(layer, layers);
    }

    if (layer_type == "SigmoidCrossEntropyLoss" ||
        layer_type == "Sigmoid") {
        return detail::create_activation<activation::sigmoid>(layer, layers);
    }

    if (layer_type == "ReLU") {
        return detail::create_activation<activation::relu>(layer, layers);
    }

    if (layer_type == "TanH") {
        return detail::create_activation<activation::tan_h>(layer, layers);
    }

    throw nn_error("layer parser not found");

    /*typedef std::function<std::shared_ptr<layer>(
        const caffe::LayerParameter&, const shape_t&, shape_t*)> factoryimpl;

    std::unordered_map<std::string, factoryimpl> factory_registry;

    factory_registry["Convolution"] = detail::create_convlayer;
    factory_registry["Deconvolution"] = detail::create_deconvlayer;
    factory_registry["InnerProduct"] = detail::create_fullyconnected;
    factory_registry["Pooling"] = detail::create_pooling;
    factory_registry["LRN"] = detail::create_lrn;
    factory_registry["Dropout"] = detail::create_dropout;
    factory_registry["SoftmaxWithLoss"] = detail::create_softmax;
    factory_registry["SigmoidCrossEntropyLoss"] = detail::create_sigmoid;
    factory_registry["ReLU"] = detail::create_relu;
    factory_registry["Sigmoid"] = detail::create_sigmoid;
    factory_registry["TanH"] = detail::create_tanh;
    factory_registry["Softmax"] = detail::create_softmax;

    if (factory_registry.find(layer.type()) == factory_registry.end()) {
        throw nn_error("layer parser not found");
    }

    return factory_registry[layer.type()](layer, in_shape, out_shape);*/
}

inline void load(const caffe::LayerParameter& src, layer *dst) {
    typedef std::function<void(const caffe::LayerParameter&, layer*)> factoryimpl; // NOLINT
    std::unordered_map<std::string, factoryimpl> factory_registry;

    factory_registry["Convolution"] = detail::load_weights_conv;
    factory_registry["Deconvolution"] = detail::load_weights_deconv;
    factory_registry["InnerProduct"] = detail::load_weights_fullyconnected;
    factory_registry["Pooling"] = detail::load_weights_pool;

    if (factory_registry.find(src.type()) == factory_registry.end()) {
        throw nn_error("layer parser not found");
    }

    return factory_registry[src.type()](src, dst);
}


}  // namespace detail
}  // namespace tiny_dnn