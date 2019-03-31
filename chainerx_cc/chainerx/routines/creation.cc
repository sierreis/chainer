#include "chainerx/routines/creation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <istream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "chainerx/array.h"
#include "chainerx/backprop_mode.h"
#include "chainerx/backward_builder.h"
#include "chainerx/backward_context.h"
#include "chainerx/constant.h"
#include "chainerx/device.h"
#include "chainerx/dtype.h"
#include "chainerx/graph.h"
#include "chainerx/macro.h"
#include "chainerx/scalar.h"
#include "chainerx/shape.h"
#include "chainerx/strides.h"

#include "chainerx/routines/type_util.h"

namespace chainerx {
namespace internal {

size_t GetRequiredBytes(const Shape& shape, const Strides& strides, size_t item_size) {
    CHAINERX_ASSERT(shape.ndim() == strides.ndim());

    if (shape.GetTotalSize() == 0) {
        return 0;
    }

    // Calculate the distance between the first and the last element, plus single element size.
    size_t n_bytes = item_size;
    for (int8_t i = 0; i < shape.ndim(); ++i) {
        n_bytes += (shape[i] - 1) * std::abs(strides[i]);
    }
    return n_bytes;
}

Array FromHostData(
        const Shape& shape, Dtype dtype, const std::shared_ptr<void>& data, const Strides& strides, int64_t offset, Device& device) {
    auto range = GetDataRange(shape, strides, GetItemSize(dtype));
    // TODO(niboshi): Copy only required region. Currently the whole preceding (offset) region is copied.
    std::shared_ptr<void> device_data = device.FromHostMemory(data, offset + std::get<1>(range));
    return internal::MakeArray(shape, strides, dtype, device, std::move(device_data), offset);
}

Array Empty(const Shape& shape, Dtype dtype, const Strides& strides, Device& device) {
    auto bytesize = GetRequiredBytes(shape, strides, GetItemSize(dtype));
    std::shared_ptr<void> data = device.Allocate(bytesize);
    return MakeArray(shape, strides, dtype, device, std::move(data));
}

Array EmptyReduced(const Shape& shape, Dtype dtype, const Axes& axes, bool keepdims, Device& device) {
    Shape out_shape = ReduceShape(shape, axes, keepdims);
    if (!keepdims) {
        return Empty(out_shape, dtype, device);
    }
    // Set reduced strides of the output array to 0
    Strides out_strides{out_shape, dtype};
    for (int8_t axis : axes) {
        out_strides[axis] = 0;
    }
    return Empty(out_shape, dtype, out_strides, device);
}

void Strip(std::string& s) {
    const size_t original_size = s.size();
    size_t num_spaces_begin = 0, num_spaces_end = 0;
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; i--) {
        if (s[i] == '\0' || std::isspace(s[i]))
            num_spaces_end++;
        else
            break;
    }

    for (int i = 0; i < static_cast<int>(s.size() - num_spaces_end); i++) {
        if (s[i] == '\0' || std::isspace(s[i]))
            num_spaces_begin++;
        else
            break;
    }

    s.erase(0, num_spaces_begin);
    s.erase(original_size - num_spaces_begin - num_spaces_end, num_spaces_end);
}

bool ParseBoolRepr(const std::string& bool_repr) {
    std::string bool_repr_lower;
    bool_repr_lower.resize(bool_repr.size());
    std::transform(bool_repr.begin(), bool_repr.end(), bool_repr_lower.begin(), tolower);

    Strip(bool_repr_lower);

    if (bool_repr_lower == "true" || bool_repr_lower == "t") return true;
    if (bool_repr_lower == "false" || bool_repr_lower == "f") return false;

    throw ChainerxError{
            ("Invalid bool representation, expecting case insensite choice of "
             "(true, t, false, f).")};
}

template <typename T>
T ParseSpecialFloat(const std::string& value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), tolower);
    Strip(v);

    if (v == "inf" || v == "infinity") return std::numeric_limits<T>::infinity();
    if (v == "-inf" || v == "-infinity") return -std::numeric_limits<T>::infinity();
    if (v == "nan") return std::numeric_limits<T>::quiet_NaN();

    throw ChainerxError{"Invalid special floating-point value."};
}

template <typename T>
T ParseFloating(const std::string& value) {
    std::istringstream iss(value);
    T parsed;
    if (!(iss >> parsed)) parsed = ParseSpecialFloat<T>(value);
    return parsed;
}

template <typename T>
T ParseIntegral(const std::string& value) {
    std::istringstream iss(value);
    T parsed;
    if (!(iss >> parsed)) throw ChainerxError{"Can't parse the text element."};
    return parsed;
}

template <typename T>
std::shared_ptr<void> ReadFromTextStream(std::istream& is, int64_t& count, const char delimiter) {
    std::string element;
    std::shared_ptr<T> data;
    if (count >= 0) {
        data = std::shared_ptr<T>{new T[count], std::default_delete<T[]>{}};
        T* data_ptr = static_cast<T*>(data.get());
        for (int64_t i = 0; i < count; i++) {
            if (!std::getline(is, element, delimiter)) throw ChainerxError("Can't read the provided number of elements.");
            data_ptr[i] = (std::is_floating_point<T>::value) ? ParseFloating<T>(element) : ParseIntegral<T>(element);
        }
    } else {
        std::vector<T> data_vector;
        while (std::getline(is, element, delimiter)) {
            data_vector.push_back((std::is_floating_point<T>::value) ? ParseFloating<T>(element) : ParseIntegral<T>(element));
        }

        count = static_cast<int64_t>(data_vector.size());
        data = std::shared_ptr<T>{new T[count], std::default_delete<T[]>{}};
        std::memcpy(data.get(), data_vector.data(), sizeof(T) * count);
    }

    return std::static_pointer_cast<void>(data);
}

template <>
std::shared_ptr<void> ReadFromTextStream<bool>(std::istream& is, int64_t& count, const char delimiter) {
    std::string element;
    std::shared_ptr<bool> data;
    if (count >= 0) {
        data = std::shared_ptr<bool>{new bool[count], std::default_delete<bool[]>{}};
        bool* data_ptr = static_cast<bool*>(data.get());
        for (int64_t i = 0; i < count; i++) {
            if (!std::getline(is, element, delimiter)) throw ChainerxError{"Can't read the provided number of elements."};
            data_ptr[i] = ParseBoolRepr(element);
        }
    } else {
        std::vector<bool> data_vector;
        while (std::getline(is, element, delimiter)) {
            data_vector.push_back(ParseBoolRepr(element));
        }

        count = static_cast<int64_t>(data_vector.size());
        data = std::shared_ptr<bool>{new bool[count], std::default_delete<bool[]>{}};
        bool* data_ptr = static_cast<bool*>(data.get());
        for (int64_t j = 0; j < count; j++) {
            data_ptr[j] = data_vector[j];
        }
    }

    return std::static_pointer_cast<void>(data);
}

template <>
std::shared_ptr<void> ReadFromTextStream<Float16>(std::istream& is, int64_t& count, const char delimiter) {
    std::string element;
    float element_float;
    std::shared_ptr<uint16_t> data;
    if (count >= 0) {
        data = std::shared_ptr<uint16_t>{new uint16_t[count], std::default_delete<uint16_t[]>{}};
        uint16_t* data_ptr = static_cast<uint16_t*>(data.get());
        for (int64_t i = 0; i < count; i++) {
            if (!std::getline(is, element, delimiter)) throw ChainerxError{"Can't read the provided number of elements."};
            element_float = ParseFloating<float>(element);
            Float16 element_float16{element_float};
            data_ptr[i] = element_float16.data();
        }
    } else {
        std::vector<uint16_t> data_vector;
        while (std::getline(is, element, delimiter)) {
            element_float = ParseFloating<float>(element);
            Float16 element_float16{element_float};
            data_vector.push_back(element_float16.data());
        }

        count = static_cast<int64_t>(data_vector.size());
        data = std::shared_ptr<uint16_t>{new uint16_t[count], std::default_delete<uint16_t[]>{}};
        std::memcpy(data.get(), data_vector.data(), sizeof(uint16_t) * count);
    }

    return std::static_pointer_cast<void>(data);
}

template <typename T>
std::shared_ptr<void> ReadFromBinaryStream(std::istream& is, int64_t& count) {
    T element;
    std::shared_ptr<T> data;
    if (count >= 0) {
        data = std::shared_ptr<T>{new T[count], std::default_delete<T[]>{}};
        T* data_ptr = static_cast<T*>(data.get());
        for (int64_t i = 0; i < count; i++) {
            if (!is.read(reinterpret_cast<char*>(&element), sizeof(T))) throw ChainerxError{"Can't read the provided number of elements."};
            data_ptr[i] = element;
        }
    } else {
        std::vector<T> data_vector;
        while (is.read(reinterpret_cast<char*>(&element), sizeof(T))) {
            data_vector.push_back(element);
        }

        count = static_cast<int64_t>(data_vector.size());
        data = std::shared_ptr<T>{new T[count], std::default_delete<T[]>{}};
        std::memcpy(data.get(), data_vector.data(), sizeof(T) * count);
    }

    return std::static_pointer_cast<void>(data);
}

template <>
std::shared_ptr<void> ReadFromBinaryStream<bool>(std::istream& is, int64_t& count) {
    uint8_t element;
    std::shared_ptr<bool> data;
    if (count >= 0) {
        data = std::shared_ptr<bool>{new bool[count], std::default_delete<bool[]>{}};
        bool* data_ptr = static_cast<bool*>(data.get());
        for (int64_t i = 0; i < count; i++) {
            if (!is.read(reinterpret_cast<char*>(&element), sizeof(uint8_t)))
                throw ChainerxError{"Can't read the provided number of elements."};
            data_ptr[i] = element;
        }
    } else {
        std::vector<bool> data_vector;
        while (is.read(reinterpret_cast<char*>(&element), sizeof(uint8_t))) {
            data_vector.push_back(static_cast<bool>(element));
        }

        count = static_cast<int64_t>(data_vector.size());
        data = std::shared_ptr<bool>{new bool[count], std::default_delete<bool[]>{}};
        bool* data_ptr = static_cast<bool*>(data.get());
        for (int64_t j = 0; j < count; j++) {
            data_ptr[j] = data_vector[j];
        }
    }

    return std::static_pointer_cast<void>(data);
}

template <>
std::shared_ptr<void> ReadFromBinaryStream<Float16>(std::istream& is, int64_t& count) {
    return ReadFromBinaryStream<uint16_t>(is, count);
}

}  // namespace internal

Array FromContiguousHostData(const Shape& shape, Dtype dtype, const std::shared_ptr<void>& data, Device& device) {
    return internal::FromHostData(shape, dtype, data, {shape, dtype}, 0, device);
}

Array FromData(
        const Shape& shape,
        Dtype dtype,
        const std::shared_ptr<void>& data,
        const nonstd::optional<Strides>& strides,
        int64_t offset,
        Device& device) {
    return internal::MakeArray(
            shape, strides.value_or(Strides{shape, dtype}), dtype, device, device.MakeDataFromForeignPointer(data), offset);
}

Array FromFile(const std::string& filename, Dtype dtype, int64_t count, nonstd::optional<char> delimiter, Device& device) {
    std::ifstream::openmode mode = std::ios::in;
    if (!delimiter.has_value()) mode |= std::ios::binary;

    std::ifstream file{filename, mode};
    Array x = FromStream(file, dtype, count, delimiter, device);
    file.close();

    return x;
}

Array FromString(const std::string& data, Dtype dtype, int64_t count, nonstd::optional<char> delimiter, Device& device) {
    std::stringstream::openmode mode = std::ios::in;
    if (!delimiter.has_value()) mode |= std::ios::binary;

    std::stringstream ss{data, mode};
    return FromStream(ss, dtype, count, delimiter, device);
}

Array FromStream(std::istream& is, Dtype dtype, int64_t count, nonstd::optional<char> delimiter, Device& device) {
    std::shared_ptr<void> data = VisitDtype(dtype, [&](auto pt) {
        using T = typename decltype(pt)::type;
        if (delimiter.has_value()) return internal::ReadFromTextStream<T>(is, count, *delimiter);
        return internal::ReadFromBinaryStream<T>(is, count);
    });

    return FromData({count}, dtype, data, nonstd::nullopt, 0, device);
}

Array Empty(const Shape& shape, Dtype dtype, Device& device) {
    auto bytesize = static_cast<size_t>(shape.GetTotalSize() * GetItemSize(dtype));
    std::shared_ptr<void> data = device.Allocate(bytesize);
    return internal::MakeArray(shape, Strides{shape, dtype}, dtype, device, std::move(data));
}

Array Full(const Shape& shape, Scalar fill_value, Dtype dtype, Device& device) {
    Array array = Empty(shape, dtype, device);
    array.Fill(fill_value);
    return array;
}

Array Full(const Shape& shape, Scalar fill_value, Device& device) {
    return Full(shape, fill_value, internal::GetDefaultDtype(fill_value.kind()), device);
}

Array Zeros(const Shape& shape, Dtype dtype, Device& device) { return Full(shape, 0, dtype, device); }

Array Ones(const Shape& shape, Dtype dtype, Device& device) { return Full(shape, 1, dtype, device); }

Array Arange(Scalar start, Scalar stop, Scalar step, Dtype dtype, Device& device) {
    // TODO(hvy): Simplify comparison if Scalar::operator== supports dtype conversion.
    if (static_cast<double>(step) == 0.0) {
        throw ChainerxError("Cannot create an arange array with 0 step size.");
    }

    // Compute the size of the output.
    auto start_value = static_cast<double>(start);
    auto stop_value = static_cast<double>(stop);
    auto step_value = static_cast<double>(step);
    if (step_value < 0) {
        std::swap(start_value, stop_value);
        step_value *= -1;
    }
    auto size = std::max(int64_t{0}, static_cast<int64_t>(std::ceil((stop_value - start_value) / step_value)));
    if (size > 2 && dtype == Dtype::kBool) {
        throw DtypeError{"Cannot create an arange array of booleans with size larger than 2."};
    }

    Array out = Empty({size}, dtype, device);
    device.Arange(start, step, out);
    return out;
}

Array Arange(Scalar start, Scalar stop, Scalar step, Device& device) {
    // TODO(hvy): Type promote instead of using the dtype of step.
    return Arange(start, stop, step, internal::GetDefaultDtype(step.kind()), device);
}

Array Arange(Scalar start, Scalar stop, Dtype dtype, Device& device) { return Arange(start, stop, 1, dtype, device); }

Array Arange(Scalar start, Scalar stop, Device& device) {
    // TODO(hvy): Type promote dtype instead of using the dtype of stop.
    return Arange(start, stop, 1, internal::GetDefaultDtype(stop.kind()), device);
}

Array Arange(Scalar stop, Dtype dtype, Device& device) { return Arange(0, stop, 1, dtype, device); }

Array Arange(Scalar stop, Device& device) { return Arange(0, stop, 1, internal::GetDefaultDtype(stop.kind()), device); }

Array EmptyLike(const Array& a, Device& device) { return Empty(a.shape(), a.dtype(), device); }

Array FullLike(const Array& a, Scalar fill_value, Device& device) { return Full(a.shape(), fill_value, a.dtype(), device); }

Array ZerosLike(const Array& a, Device& device) { return Zeros(a.shape(), a.dtype(), device); }

Array OnesLike(const Array& a, Device& device) { return Ones(a.shape(), a.dtype(), device); }

Array Copy(const Array& a) {
    Array out = EmptyLike(a, a.device());
    {
        NoBackpropModeScope scope{};
        a.device().Copy(a, out);
    }

    BackwardBuilder bb{"copy", a, out};
    if (BackwardBuilder::Target bt = bb.CreateTarget(0)) {
        bt.Define([](BackwardContext& bctx) { bctx.input_grad() = *bctx.output_grad(); });
    }
    bb.Finalize();

    CHAINERX_ASSERT(out.IsContiguous());
    return out;
}

// Creates the identity array.
Array Identity(int64_t n, Dtype dtype, Device& device) {
    if (n < 0) {
        throw DimensionError{"Negative dimensions are not allowed"};
    }

    Array out = Empty(Shape{n, n}, dtype, device);
    {
        NoBackpropModeScope scope{};
        device.Identity(out);
    }
    return out;
}

Array Eye(int64_t n, nonstd::optional<int64_t> m, nonstd::optional<int64_t> k, nonstd::optional<Dtype> dtype, Device& device) {
    if (!m.has_value()) {
        m = n;
    }
    if (!k.has_value()) {
        k = 0;
    }
    if (!dtype.has_value()) {
        dtype = Dtype::kFloat64;
    }
    if (n < 0 || m < 0) {
        throw DimensionError{"Negative dimensions are not allowed"};
    }

    Array out = Empty({n, m.value()}, dtype.value(), device);
    {
        NoBackpropModeScope scope{};
        device.Eye(k.value(), out);
    }
    return out;
}

namespace internal {

Array AsContiguous(const Array& a, Dtype dtype) {
    if (a.IsContiguous() && a.dtype() == dtype) {
        return a;
    }

    Array out = Empty(a.shape(), dtype, a.device());
    {
        NoBackpropModeScope scope{};
        a.device().AsType(a, out);
    }

    if (GetKind(dtype) == DtypeKind::kFloat) {
        BackwardBuilder bb{"ascontiguousarray", a, out};
        if (BackwardBuilder::Target bt = bb.CreateTarget(0)) {
            bt.Define([src_dtype = a.dtype()](BackwardContext& bctx) {
                const Array& gout = *bctx.output_grad();
                bctx.input_grad() = gout.AsType(src_dtype, false);
            });
        }
        bb.Finalize();
    }

    CHAINERX_ASSERT(out.IsContiguous());
    CHAINERX_ASSERT(out.shape() == a.shape());
    CHAINERX_ASSERT(out.dtype() == dtype);
    return out;
}

}  // namespace internal

Array AsContiguousArray(const Array& a, const nonstd::optional<Dtype>& dtype) {
    Dtype src_dt = a.dtype();
    Dtype dt = dtype.value_or(src_dt);

    if (a.IsContiguous() && src_dt == dt) {
        if (a.ndim() == 0) {
            return a.Reshape(Shape{1});
        }
        return a;
    }

    Array out = internal::AsContiguous(a, dt);
    if (a.ndim() == 0) {
        out = out.Reshape({1});
    }
    return out;
}

Array Diag(const Array& v, int64_t k, Device& device) {
    Array out{};

    int8_t ndim = v.ndim();
    if (ndim == 1) {
        // Return a square matrix with filled diagonal.
        int64_t n = v.shape()[0] + std::abs(k);
        out = Empty(Shape{n, n}, v.dtype(), device);
        {
            NoBackpropModeScope scope{};
            device.Diagflat(v, k, out);
        }
    } else if (ndim == 2) {
        // Return the diagonal as a 1D array.
        int64_t rows = v.shape()[0];
        int64_t cols = v.shape()[1];
        int64_t n = std::min(rows, cols);
        int64_t offset{};
        if (k >= 0) {
            offset = k * v.strides()[1];
            if (cols <= k + n - 1) {
                n = std::max(int64_t{0}, cols - k);
            }
        } else {
            offset = -k * v.strides()[0];
            if (rows <= -k + n - 1) {
                n = std::max(int64_t{0}, rows + k);
            }
        }
        out = internal::MakeArray(Shape{n}, Strides{v.strides()[0] + v.strides()[1]}, v.dtype(), device, v.data(), v.offset() + offset);
    } else {
        throw DimensionError{"Input must be 1D or 2D."};
    }

    BackwardBuilder bb{"diag", v, out};
    if (BackwardBuilder::Target bt = bb.CreateTarget(0)) {
        bt.Define([& device = v.device(), k](BackwardContext& bctx) {
            const Array& gout = *bctx.output_grad();
            bctx.input_grad() = Diag(gout, k, device);
        });
    }
    bb.Finalize();

    return out;
}

Array Diagflat(const Array& v, int64_t k, Device& device) {
    // TODO(hvy): Use Ravel or Flatten when implemented instead of Reshape.
    return Diag(v.Reshape({v.GetTotalSize()}), k, device);
}

// Creates a 1-d array with evenly spaced numbers.
Array Linspace(
        Scalar start,
        Scalar stop,
        const nonstd::optional<int64_t>& num,
        bool endpoint,
        const nonstd::optional<Dtype>& dtype,
        Device& device) {
    static const int64_t kDefaultNum = 50;

    // Always default to float type.
    // Similar behavior to numpy
    // Ref: https://github.com/numpy/numpy/issues/8597
    Dtype dtype_a = dtype.value_or(internal::GetDefaultDtype(chainerx::DtypeKind::kFloat));
    int64_t num_a = num.value_or(kDefaultNum);

    if (num_a < 0) {
        throw ChainerxError{"Number of samples, ", num_a, ", must be non-negative"};
    }

    Array out = Empty(Shape{num_a}, dtype_a, device);
    if (num_a > 0) {
        auto start_value = static_cast<double>(start);
        auto stop_value = static_cast<double>(stop);
        if (!endpoint) {
            stop_value = start_value + (stop_value - start_value) * (num_a - 1) / num_a;
        }
        {
            NoBackpropModeScope scope{};
            device.Linspace(start_value, stop_value, out);
        }
    }
    return out;
}

}  // namespace chainerx
