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
#include <vector>
#include <functional>
#include <random>
#include <type_traits>
#include <limits>
#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <sstream>

#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/deque.hpp>

#include "tiny_dnn/config.h"
#include "tiny_dnn/util/macro.h"
#include "tiny_dnn/util/aligned_allocator.h"
#include "tiny_dnn/util/nn_error.h"
#include "tiny_dnn/util/parallel_for.h"
#include "tiny_dnn/util/random.h"

#if defined(USE_OPENCL) || defined(USE_CUDA)
#ifdef USE_OPENCL
#include "third_party/CLCudaAPI/clpp11.h"
#else
#include "third_party/CLCudaAPI/cupp11.h"
#endif
#endif

namespace tiny_dnn {

///< output label(class-index) for classification
///< must be equal to cnn_size_t, because size of last layer is equal to num. of classes
typedef cnn_size_t label_t;

typedef cnn_size_t layer_size_t; // for backward compatibility

typedef std::vector<float_t, aligned_allocator<float_t, 64>> vec_t;

typedef std::vector<vec_t> tensor_t;

enum class net_phase {
    train,
    test
};

enum class padding {
    valid,  ///< use valid pixels of input
    same    ///< add zero-padding around input so as to keep image size
};

template<typename T>
T* reverse_endian(T* p) {
    std::reverse(reinterpret_cast<char*>(p), reinterpret_cast<char*>(p) + sizeof(T));
    return p;
}

inline bool is_little_endian() {
    int x = 1;
    return *(char*) &x != 0;
}


template<typename T>
size_t max_index(const T& vec) {
    auto begin_iterator = std::begin(vec);
    return std::max_element(begin_iterator, std::end(vec)) - begin_iterator;
}

template<typename T, typename U>
U rescale(T x, T src_min, T src_max, U dst_min, U dst_max) {
    U value =  static_cast<U>(((x - src_min) * (dst_max - dst_min)) / (src_max - src_min) + dst_min);
    return std::min(dst_max, std::max(value, dst_min));
}

inline void nop()
{
    // do nothing
}

template <typename T> inline T sqr(T value) { return value*value; }

inline bool isfinite(float_t x) {
    return x == x;
}

template <typename Container> inline bool has_infinite(const Container& c) {
    for (auto v : c)
        if (!isfinite(v)) return true;
    return false;
}

template <typename Container>
size_t max_size(const Container& c) {
    typedef typename Container::value_type value_t;
    return std::max_element(c.begin(), c.end(),
        [](const value_t& left, const value_t& right) { return left.size() < right.size(); })->size();
}

inline std::string format_str(const char *fmt, ...) {
    static char buf[2048];

#ifdef _MSC_VER
#pragma warning(disable:4996)
#endif
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
#ifdef _MSC_VER
#pragma warning(default:4996)
#endif
    return std::string(buf);
}

template <typename T>
struct index3d {
    index3d(T width, T height, T depth) {
        reshape(width, height, depth);
    }

    index3d() : width_(0), height_(0), depth_(0) {}

    void reshape(T width, T height, T depth) {
        width_ = width;
        height_ = height;
        depth_ = depth;

        if ((long long) width * height * depth > std::numeric_limits<T>::max())
            throw nn_error(
            format_str("error while constructing layer: layer size too large for tiny-dnn\nWidthxHeightxChannels=%dx%dx%d >= max size of [%s](=%d)",
            width, height, depth, typeid(T).name(), std::numeric_limits<T>::max()));
    }

    T get_index(T x, T y, T channel) const {
        assert(x >= 0 && x < width_);
        assert(y >= 0 && y < height_);
        assert(channel >= 0 && channel < depth_);
        return (height_ * channel + y) * width_ + x;
    }

    T area() const {
        return width_ * height_;
    }

    T size() const {
        return width_ * height_ * depth_;
    }

    template <class Archive>
    void serialize(Archive & ar) {
        ar(cereal::make_nvp("width", width_));
        ar(cereal::make_nvp("height", height_));
        ar(cereal::make_nvp("depth", depth_));
    }

    T width_;
    T height_;
    T depth_;
};

typedef index3d<cnn_size_t> shape3d;

template <typename T>
bool operator == (const index3d<T>& lhs, const index3d<T>& rhs) {
    return (lhs.width_ == rhs.width_) && (lhs.height_ == rhs.height_) && (lhs.depth_ == rhs.depth_);
}

template <typename T>
bool operator != (const index3d<T>& lhs, const index3d<T>& rhs) {
    return !(lhs == rhs);
}

template <typename Stream, typename T>
Stream& operator << (Stream& s, const index3d<T>& d) {
    s << d.width_ << "x" << d.height_ << "x" << d.depth_;
    return s;
}

template <typename T>
std::ostream& operator << (std::ostream& s, const index3d<T>& d) {
    s << d.width_ << "x" << d.height_ << "x" << d.depth_;
    return s;
}

template <typename Stream, typename T>
Stream& operator << (Stream& s, const std::vector<index3d<T>>& d) {
    s << "[";
    for (cnn_size_t i = 0; i < d.size(); i++) {
        if (i) s << ",";
        s << "[" << d[i] << "]";
    }
    s << "]";
    return s;
}

// equivalent to std::to_string, which android NDK doesn't support
template <typename T>
std::string to_string(T value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

// boilerplate to resolve dependent name
#define CNN_USE_LAYER_MEMBERS using layer::parallelize_; \
    using feedforward_layer<Activation>::h_


#define CNN_LOG_VECTOR(vec, name)
/*
void CNN_LOG_VECTOR(const vec_t& vec, const std::string& name) {
    std::cout << name << ",";

    if (vec.empty()) {
        std::cout << "(empty)" << std::endl;
    }
    else {
        for (size_t i = 0; i < vec.size(); i++) {
            std::cout << vec[i] << ",";
        }
    }

    std::cout << std::endl;
}
*/


template <typename T, typename Pred, typename Sum>
cnn_size_t sumif(const std::vector<T>& vec, Pred p, Sum s) {
    size_t sum = 0;
    for (size_t i = 0; i < vec.size(); i++) {
        if (p(i)) sum += s(vec[i]);
    }
    return sum;
}

template <typename T, typename Pred>
std::vector<T> filter(const std::vector<T>& vec, Pred p) {
    std::vector<T> res;
    for (size_t i = 0; i < vec.size(); i++) {
        if (p(i)) res.push_back(vec[i]);
    }
    return res;
}

template <typename Result, typename T, typename Pred>
std::vector<Result> map_(const std::vector<T>& vec, Pred p) {
    std::vector<Result> res;
    for (auto& v : vec) {
        res.push_back(p(v));
    }
    return res;
}

enum class vector_type : int32_t {
    // 0x0001XXX : in/out data
    data = 0x0001000, // input/output data, fed by other layer or input channel

    // 0x0002XXX : trainable parameters, updated for each back propagation
    weight = 0x0002000,
    bias = 0x0002001,

    label = 0x0004000,
    aux = 0x0010000 // layer-specific storage
};

inline std::string to_string(vector_type vtype) {
    switch (vtype)
    {
    case tiny_dnn::vector_type::data:
        return "data";
    case tiny_dnn::vector_type::weight:
        return "weight";
    case tiny_dnn::vector_type::bias:
        return "bias";
    case tiny_dnn::vector_type::label:
        return "label";
    case tiny_dnn::vector_type::aux:
        return "aux";
    default:
        return "unknown";
    }
}

inline std::ostream& operator << (std::ostream& os, vector_type vtype) {
    os << to_string(vtype);
    return os;
}

inline vector_type operator & (vector_type lhs, vector_type rhs) {
    return (vector_type)(static_cast<int32_t>(lhs) & static_cast<int32_t>(rhs));
}

inline bool is_trainable_weight(vector_type vtype) {
    return (vtype & vector_type::weight) == vector_type::weight;
}

inline std::vector<vector_type> std_input_order(bool has_bias) {
    if (has_bias) {
        return{ vector_type::data, vector_type::weight, vector_type::bias };
    }
    else {
        return{ vector_type::data, vector_type::weight };
    }
}

inline std::vector<vector_type> std_output_order(bool has_activation) {
    if (has_activation) {
        return{ vector_type::data, vector_type::aux };
    }
    else {
        return{ vector_type::data };
    }
}

inline void fill_tensor(tensor_t& tensor, float_t value) {
    for (auto& t : tensor) {
        std::fill(t.begin(), t.end(), value);
    }
}

inline void fill_tensor(tensor_t& tensor, float_t value, cnn_size_t size) {
    for (auto& t : tensor) {
        t.resize(size, value);
    }
}

inline cnn_size_t conv_out_length(cnn_size_t in_length,
                                  cnn_size_t window_size,
                                  cnn_size_t stride,
                                  padding pad_type) {
    size_t output_length;

    if (pad_type == padding::same) {
        output_length = in_length;
    }
    else if (pad_type == padding::valid) {
        output_length = in_length - window_size + 1;
    }
    else {
        throw nn_error("Not recognized pad_type.");
    }
    return (output_length + stride - 1) / stride;
}

// get all platforms (drivers), e.g. NVIDIA
// https://github.com/CNugteren/CLCudaAPI/blob/master/samples/device_info.cc

inline void printAvailableDevice(const cnn_size_t platform_id,
                                 const cnn_size_t device_id) {
#if defined(USE_OPENCL) || defined(USE_CUDA)
    // Initializes the CLCudaAPI platform and device. This initializes the OpenCL/CUDA back-end and
    // selects a specific device on the platform.
    auto platform = CLCudaAPI::Platform(platform_id);
    auto device = CLCudaAPI::Device(platform, device_id);

    // Prints information about the chosen device. Most of these results should stay the same when
    // switching between the CUDA and OpenCL back-ends.
    printf("\n## Printing device information...\n");
    printf(" > Platform ID                  %zu\n", platform_id);
    printf(" > Device ID                    %zu\n", device_id);
    printf(" > Framework version            %s\n", device.Version().c_str());
    printf(" > Vendor                       %s\n", device.Vendor().c_str());
    printf(" > Device name                  %s\n", device.Name().c_str());
    printf(" > Device type                  %s\n", device.Type().c_str());
    printf(" > Max work-group size          %zu\n", device.MaxWorkGroupSize());
    printf(" > Max thread dimensions        %zu\n", device.MaxWorkItemDimensions());
    printf(" > Max work-group sizes:\n");
    for (auto i=size_t{0}; i<device.MaxWorkItemDimensions(); ++i) {
        printf("   - in the %zu-dimension         %zu\n", i, device.MaxWorkItemSizes()[i]);
    }
    printf(" > Local memory per work-group  %zu bytes\n", device.LocalMemSize());
    printf(" > Device capabilities          %s\n", device.Capabilities().c_str());
    printf(" > Core clock rate              %zu MHz\n", device.CoreClock());
    printf(" > Number of compute units      %zu\n", device.ComputeUnits());
    printf(" > Total memory size            %zu bytes\n", device.MemorySize());
    printf(" > Maximum allocatable memory   %zu bytes\n", device.MaxAllocSize());
    printf(" > Memory clock rate            %zu MHz\n", device.MemoryClock());
    printf(" > Memory bus width             %zu bits\n", device.MemoryBusWidth());
#else
    nn_warn("TinyDNN was not build with OpenCL or CUDA support.");
#endif
}

} // namespace tiny_dnn
