#include <torch/csrc/jit/runtime/register_ops_utils.h>
#include <torch/library.h>

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace torch {
namespace jit {

namespace {

std::string stringSlice(
    std::string string,
    int64_t start,
    int64_t end,
    int64_t step) {
  TORCH_CHECK(step == 1, "Slicing a string only supports step=1");

  const int64_t size = string.size();

  // Clamp start and end to the bounds of the list
  start = std::max(int64_t(0), normalizeIndex(start, size));
  end = std::min(size, normalizeIndex(end, size));

  if (end <= start) {
    // Slice is empty
    return std::string("");
  }

  std::string result(string.begin() + start, string.begin() + end);
  return result;
}

// consecutive whitespace are regarded as a single separator,
// the result will contain no empty strings at the start or end
// if the string has leading or trailing whitespace.
c10::List<std::string> splitNoneSeparator(const std::string& string) {
  c10::List<std::string> splits;
  // whitespaces includes tab, space and
  // the delimiters defined in the implementation of splitlines
  std::string whitespaces =
      " \t\n\r\r\n\v\x0b\f\x0c\x1c\x1d\x1e\x85\u2028\u2029";
  std::string::size_type prev_pos = 0;
  std::string::size_type pos = 0;

  while ((pos = string.find_first_of(whitespaces, pos)) != std::string::npos) {
    auto substr = string.substr(prev_pos, pos - prev_pos);
    // skip the whitespaces as the Python split() method
    if (!substr.empty()) {
      splits.emplace_back(substr);
    }
    pos++;
    prev_pos = pos;
  }
  if (prev_pos != string.size()) {
    splits.emplace_back(string.substr(prev_pos));
  }
  return splits;
}

TORCH_LIBRARY_IMPL(aten, CatchAll, m) {
  m.impl("slice.str", TORCH_FN(stringSlice));
  m.impl("strip", [](std::string string, const std::string& chars) {
    auto rindex = string.find_last_not_of(chars);
    if (rindex != std::string::npos) {
      string = string.substr(0, rindex + 1);
    } else {
      string = "";
    }
    auto lindex = string.find_first_not_of(chars);
    if (lindex != std::string::npos) {
      string = string.substr(lindex, string.size());
    } else {
      string = "";
    }
    return string;
  });
  m.impl(
      "split.str",
      [](const std::string& string,
         c10::optional<std::string> separator,
         int64_t max) {
        if (!separator.has_value()) {
          // if separator is not specified,
          // a different splitting algorithm is applied as Python
          return splitNoneSeparator(string);
          ;
        }
        if (separator.value().empty()) {
          throw std::runtime_error("ValueError: empty separator");
        }

        std::string::size_type prev_pos = 0;
        std::string::size_type pos = 0;
        c10::List<std::string> splits;
        auto count = 0;

        while ((pos = string.find(separator.value(), pos)) !=
               std::string::npos) {
          count++;
          if (max >= 0 && count > max) {
            break;
          } else {
            splits.emplace_back(string.substr(prev_pos, pos - prev_pos));
          }
          pos += separator.value().size();
          prev_pos = pos;
        }
        splits.emplace_back(string.substr(prev_pos, string.size() - prev_pos));
        return splits;
      });
}

RegisterOperators reg(
    {Operator(
         "aten::list(str t) -> str[]",
         [](Stack& stack) {
           auto str = pop(stack).toStringRef();
           c10::List<std::string> chars;
           chars.reserve(str.size());
           for (auto c : str) {
             chars.push_back(std::string(1, c));
           }
           push(stack, std::move(chars));
           return 0;
         },
         aliasAnalysisFromSchema()),
     // only used internally in range() translation
     Operator(
         "aten::__range_length(int lo, int hi, int step) -> int",
         [](Stack& stack) {
           int64_t lo, hi, step;
           pop(stack, lo, hi, step);
           // error handling when step_val = 0 during runtime
           if (step == 0) {
             throw std::runtime_error("range() arg 3 must not be zero");
           }
           if (step > 0 && lo < hi) {
             push(stack, 1 + (hi - 1 - lo) / step);
           } else if (step < 0 && lo > hi) {
             push(stack, 1 + (lo - 1 - hi) / (0 - step));
           } else {
             push(stack, 0);
           }
           return 0;
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::__derive_index(int index, int start, int step) -> int",
         [](Stack& stack) {
           int64_t index, start, step;
           pop(stack, index, start, step);
           push(stack, start + index * step);
           return 0;
         },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::TupleUnpack(Any tup) -> ...",
         [](Stack* stack) { tupleUnpack(*stack); },
         aliasAnalysisSpecialCase()),
     Operator(
         "prim::unchecked_cast(t x) -> t",
         noop,
         aliasAnalysisSpecialCase()),
     Operator(
         "aten::IntImplicit(Tensor a) -> int",
         [](Stack* stack) {
           at::Tensor a;
           pop(stack, a);
           checkImplicitTensorToNum(a, /*to int*/ true);
           push(stack, a.item<int64_t>());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::FloatImplicit(Tensor a) -> float",
         [](Stack* stack) {
           at::Tensor a;
           pop(stack, a);
           checkImplicitTensorToNum(a, /*to int*/ false);
           push(stack, a.item<double>());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::ScalarImplicit(Tensor a) -> Scalar",
         [](Stack* stack) {
           at::Tensor a;
           pop(stack, a);
           checkImplicitTensorToNum(a, /*to int*/ false);
           push(stack, a.item());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Bool.Tensor(Tensor a) -> bool",
         [](Stack* stack) {
           at::Tensor a;
           pop(stack, a);
           push(stack, a.is_nonzero());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Bool.int(int a) -> bool",
         [](Stack* stack) {
           int64_t i;
           pop(stack, i);
           push(stack, (bool)i);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Bool.float(float a) -> bool",
         [](Stack* stack) {
           double d;
           pop(stack, d);
           push(stack, (bool)d);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Float.Tensor(Tensor a) -> float",
         [](Stack* stack) {
           at::Tensor a;
           pop(stack, a);
           push(stack, a.item<double>());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Float.Scalar(Scalar a) -> float",
         [](Stack* stack) {
           IValue scalar;
           pop(stack, scalar);
           if (scalar.isDouble()) {
             push(stack, std::move(scalar));
           } else {
             push(stack, static_cast<double>(scalar.toInt()));
           }
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Float.int(int a) -> float",
         [](Stack* stack) {
           int64_t i;
           pop(stack, i);
           push(stack, (float)i);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Float.bool(bool a) -> float",
         [](Stack* stack) {
           bool b;
           pop(stack, b);
           push(stack, (float)b);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Float.str(str a) -> float",
         [](Stack* stack) {
           auto s = pop(stack).toString();
           std::string::size_type sz;
           double b = c10::stod(s->string(), &sz);
           if (sz == s->string().size()) {
             push(stack, b);
           } else {
             std::stringstream error_str;
             error_str << "could not convert string "
                       << "to float: '" << s->string() << "'";
             throw std::runtime_error(error_str.str());
           }
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::format(str self, ...) -> str",
         [](Stack* stack) {
           size_t num_inputs = pop(stack).toInt();
           format(*stack, num_inputs);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::NumToTensor.Scalar(Scalar a) -> Tensor",
         [](Stack* stack) {
           at::Scalar s;
           pop(stack, s);
           push(stack, at::scalar_to_tensor(s));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::RaiseException(str msg) -> ()",
         [](Stack* stack) { throw JITException(pop(stack).toStringRef()); },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Size(int[] sizes) -> int[]",
         [](Stack* stack) {},
         aliasAnalysisFromSchema()),
     Operator(
         "aten::size(Tensor self) -> int[]",
         [](Stack* stack) {
           auto t = std::move(pop(stack)).toTensor();
           pack(stack, t.sizes().vec());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::EnumName(AnyEnumType enum) -> str",
         [](Stack* stack) {
           IValue e = pop(stack);
           push(stack, e.toEnumHolder()->name());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::EnumValue.int(AnyEnumType enum) -> int",
         [](Stack* stack) {
           IValue e = pop(stack);
           push(stack, e.toEnumHolder()->value());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::EnumValue.float(AnyEnumType enum) -> float",
         [](Stack* stack) {
           IValue e = pop(stack);
           push(stack, e.toEnumHolder()->value());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::EnumValue.str(AnyEnumType enum) -> str",
         [](Stack* stack) {
           IValue e = pop(stack);
           push(stack, e.toEnumHolder()->value());
         },
         aliasAnalysisFromSchema()),
     Operator(
         // note the compiler knows to type TupleIndex more accurately than it
         // is listed here.
         "prim::TupleIndex(Any tup, int i) -> Any",
         [](Stack* stack) {
           int64_t index = pop(stack).toInt();
           auto tuple = pop(stack).toTuple();
           auto norm_index = normalizeIndex(index, tuple->elements().size());
           if (norm_index < 0 ||
               norm_index > static_cast<int64_t>(tuple->elements().size())) {
             throw std::out_of_range("Tuple list index out of range");
           }
           stack->emplace_back(tuple->elements()[norm_index]);
         },
         aliasAnalysisSpecialCase()),
     Operator(
         "aten::ne.int_list(int[] a, int[] b) -> bool",
         listNe<int64_t>,
         aliasAnalysisFromSchema()),
     Operator(
         "prim::unchecked_unwrap_optional(t(a)? optional) -> t(a)",
         noop,
         aliasAnalysisFromSchema()),
     Operator(
         "prim::device(Tensor a) -> Device",
         [](Stack* stack) { push(stack, pop(stack).toTensor().device()); },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::dtype(Tensor a) -> int",
         [](Stack* stack) {
           at::Tensor a;
           pop(stack, a);
           push(stack, static_cast<int64_t>(a.scalar_type()));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::__not__(bool self) -> bool",
         [](Stack* stack) { push(stack, !pop(stack).toBool()); },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::__is__(t1 self, t2 obj) -> bool",
         [](Stack* stack) {
           IValue self, obj;
           pop(stack, self, obj);
           push(stack, self.is(obj));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::__isnot__(t1 self, t2 obj) -> bool",
         [](Stack* stack) {
           IValue self, obj;
           pop(stack, self, obj);
           push(stack, !self.is(obj));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::element_size(Tensor self) -> int",
         [](Stack* stack) {
           at::Tensor arg = pop(stack).toTensor();
           push(stack, arg.element_size());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::numel(Tensor self) -> int",
         [](Stack* stack) {
           at::Tensor arg = pop(stack).toTensor();
           push(stack, arg.numel());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::dim(Tensor self) -> int",
         [](Stack* stack) {
           at::Tensor arg = pop(stack).toTensor();
           push(stack, arg.dim());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::get_device(Tensor self) -> int",
         [](Stack* stack) {
           RECORD_FUNCTION("get_device", std::vector<c10::IValue>());
           auto result =
               at::get_device((std::move(peek(stack, 0, 1))).toTensor());
           drop(stack, 1);
           pack(stack, result);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::storage_offset(Tensor self) -> int",
         [](Stack* stack) {
           RECORD_FUNCTION("storage_offset", std::vector<c10::IValue>());
           auto result =
               ((std::move(peek(stack, 0, 1))).toTensor()).storage_offset();
           drop(stack, 1);
           pack(stack, result);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::is_contiguous(Tensor self) -> bool",
         [](Stack* stack) {
           RECORD_FUNCTION("is_contiguous", std::vector<c10::IValue>());
           auto result =
               ((std::move(peek(stack, 0, 1))).toTensor()).is_contiguous();
           drop(stack, 1);
           pack(stack, result);
         },
         aliasAnalysisFromSchema()),
     // these ops are generic over the list element type.
     // CREATING GENERIC_LIST_OPS
     Operator(
         "aten::select.t(t[](a) list, int idx) -> t(*)",
         listSelect,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::__getitem__.t(t[](a) list, int idx) -> t(*)",
         listSelect,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::append.t(t[](a!) self, t(c -> *) el) -> t[](a!)",
         listAppend,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::reverse.t(t[](a!) self) -> ()",
         listReverse,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::extend.t(t[](a!) self, t[] other) -> ()",
         listExtend,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::copy.t(t[](a) self) -> t[]",
         listCopy,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::_set_item.t(t [](a!) l, int idx, t(b -> *) el) -> t[](a!)",
         listSetItem,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::clear.t(t[](a!) self) -> ()",
         listClear,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::Delete.t(t[](a!) self, int idx) -> ()",
         listDelete,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::insert.t(t[](a!) self, int idx, t(b -> *) el) -> ()",
         listInsert,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::pop.t(t[](a!) self, int idx=-1) -> t(*)",
         listPop,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::add.t(t[] a, t[] b) -> t[]",
         listAdd,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::add_.t(t[](a!) self, t[] b) -> t[]",
         listInplaceAdd,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::slice.t(t[] l, int start, int end=9223372036854775807, int step=1) -> t[]",
         listSlice,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::list.t(t[] l) -> t[]",
         listList,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::mul.left_t(t[] l, int n) -> t[]",
         listMulIntLeft,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::mul.right_(int n, t[] l) -> t[]",
         listMulIntRight,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::mul_.t(t[](a!) l, int n) -> t[](a!)",
         listMulIntLeftInPlace,
         aliasAnalysisFromSchema()),
     Operator("aten::len.t(t[] a) -> int", listLen, aliasAnalysisFromSchema()),
     Operator(
         "aten::eq.int_list(int[] a, int[] b) -> bool",
         listEq<int64_t>,
         aliasAnalysisFromSchema()),
     Operator(
         "prim::Uninitialized() -> Any",
         [](Stack* stack) { push(stack, IValue::uninitialized()); },
         aliasAnalysisSpecialCase()),
     Operator(
         "prim::Print(...) -> ()",
         [](Stack* stack) {
           auto num_inputs = pop(stack).toInt();
           std::stringstream ss;
           bool first = true;
           for (const IValue& i : last(stack, num_inputs)) {
             if (!first)
               ss << " ";
             first = false;
             ss << i;
           }
           drop(stack, num_inputs);
           ss << std::endl;
           auto* handler = getPrintHandler();
           TORCH_INTERNAL_ASSERT(handler);
           handler(ss.str());
         },
         aliasAnalysisSpecialCase()),
     Operator(
         "aten::eq.enum(AnyEnumType a, AnyEnumType b) -> bool",
         [](Stack* stack) {
           IValue x = pop(stack);
           IValue y = pop(stack);
           push(stack, x == y);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::ne.enum(AnyEnumType a, AnyEnumType b) -> bool",
         [](Stack* stack) {
           IValue x = pop(stack);
           IValue y = pop(stack);
           push(stack, x != y);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::dequantize.tensor(Tensor qtensor) -> Tensor",
         [](Stack* stack) {
           at::Tensor qtensor;
           pop(stack, qtensor);
           push(stack, at::dequantize(qtensor));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::dequantize.any(Any tensors) -> Any",
         [](Stack* stack) { dequantize(*stack); },
         aliasAnalysisFromSchema()),
     DEFINE_STRING_OP(aten::add, a + b, str),
     DEFINE_COMPARISON_OP(aten::eq, a == b),
     DEFINE_COMPARISON_OP(aten::ne, a != b),
     DEFINE_COMPARISON_OP(aten::lt, a < b),
     DEFINE_COMPARISON_OP(aten::gt, a > b),
     DEFINE_COMPARISON_OP(aten::le, a <= b),
     DEFINE_COMPARISON_OP(aten::ge, a >= b),
     DEFINE_BINARY_OP(aten::add, a + b),
     DEFINE_BINARY_OP(aten::sub, a - b),
     DEFINE_BINARY_OP(aten::mul, a* b),
     DEFINE_BOOL_OP(aten::__and__, a&& b),
     DEFINE_BOOL_OP(aten::__or__, a || b),
     DEFINE_BOOL_OP(aten::__xor__, a != b),
     DEFINE_UNARY_OP(aten::floor, floor(a), int, int),
     DEFINE_UNARY_OP(aten::ceil, ceil(a), int, int),
     DEFINE_UNARY_OP(aten::neg, -a, int, float),
     DEFINE_UNARY_OP(aten::exp, std::exp(a), float, float),
     // Pass in two ops for handling int and float separately as % in C++ only
     // works for int The modulus calculation is different between C++ and
     // Python (on negative), we preserve the python behavior as it's more
     // common and match python syntax, hence the conversion.
     DEFINE_GENERIC_OP(
         aten::remainder,
         (b + (a % b)) % b,
         fmod((b + fmod(a, b)), b),
         int,
         float),
     DEFINE_INT_FLOAT_OP(aten::remainder, fmod((b + fmod(a, b)), b), float),
     DEFINE_SCALAR_BINARY_OP(
         aten::remainder,
         (b + (a % b)) % b,
         fmod((b + fmod(a, b)), b),
         Scalar),
     // NB: This is the python truediv operation
     DEFINE_GENERIC_OP(
         aten::div,
         static_cast<double>(a) / static_cast<double>(b),
         a / b,
         float,
         float),
     DEFINE_SCALAR_BINARY_OP(
         aten::div,
         static_cast<double>(a) / static_cast<double>(b),
         a / b,
         float),
     DEFINE_GENERIC_OP(
         aten::floordiv,
         floordiv(a, b),
         std::floor(a / b),
         int,
         float),
     DEFINE_INT_FLOAT_OP(aten::floordiv, std::floor(a / b), float),
     DEFINE_SCALAR_BINARY_OP(
         aten::floordiv,
         floordiv(a, b),
         std::floor(a / b),
         Scalar),
     // int ** int produces a float, because negative exponents produce float
     // results
     DEFINE_GENERIC_OP(
         aten::pow,
         static_cast<double>(pow(a, b)),
         static_cast<double>(pow(a, b)),
         float,
         float),
     DEFINE_INT_FLOAT_OP(aten::pow, pow(a, b), float),
     DEFINE_SCALAR_SCALAR_BINARY_OP(
         aten::pow,
         static_cast<double>(pow(a, b)),
         static_cast<double>(pow(a, b)),
         float),
     Operator(
         "aten::pow.int_to_int(int a, int b) -> int",
         [](Stack* stack) {
           int64_t a, b;
           pop(stack, a, b);
           push(stack, pow(a, b));
         },
         aliasAnalysisFromSchema()),
     // min and max are in prim:: because there is a difference between
     // the python builtin 'min' and 'torch.min'
     DEFINE_BINARY_OP(prim::min, a < b ? a : b),
     DEFINE_BINARY_OP(prim::max, a > b ? a : b),
     Operator(
         "prim::type(Device self) -> str",
         [](Stack* stack) {
           auto d = pop(stack);
           push(
               stack,
               DeviceTypeName(d.toDevice().type(), /* lower_case=*/true));
         },
         aliasAnalysisFromSchema()),
     // tensor length op (size of 1st dimension)
     Operator(
         "aten::len.Tensor(Tensor t) -> int",
         [](Stack* stack) {
           at::Tensor t = pop(stack).toTensor();
           if (t.dim() == 0) {
             AT_ERROR("len() of a 0-d tensor");
           }
           push(stack, t.sizes()[0]);
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::ord(str string) -> int",
         [](Stack& stack) {
           auto string = pop(stack).toStringRef();
           TORCH_CHECK(
               string.size() == 1,
               "String for ord() must be 1 character, found ",
               string.size());
           uint8_t ord = string.at(0);
           push(stack, int64_t(ord));
           return 0;
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::lower(str self) -> str",
         [](Stack& stack) {
           auto string = pop(stack).toStringRef();
           std::stringstream ss;
           for (char c : string) {
             ss << static_cast<char>(::tolower(c));
           }
           push(stack, ss.str());
           return 0;
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::__contains__.str_list(str[] l, str item) -> bool",
         listContains<std::string>,
         aliasAnalysisFromSchema()),
     Operator(
         "aten::len.str(str s) -> int",
         [](Stack& stack) {
           auto string = pop(stack).toStringRef();
           push(stack, static_cast<int64_t>(string.size()));
           return 0;
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::__getitem__.str(str s, int index) -> str",
         [](Stack& stack) {
           auto index = pop(stack).toInt();
           auto string = pop(stack).toStringRef();
           auto norm_index = normalizeIndex(index, string.size());
           char c = string.at(norm_index);
           push(stack, std::string(&c, 1));
           return 0;
         },
         aliasAnalysisFromSchema()),
#define CREATE_COPY_OP(other_type, c_type)                               \
  Operator(                                                              \
      "aten::copy_." #other_type "(Tensor(a!) self, " #other_type        \
      " other) -> Tensor(a!)",                                           \
      [](Stack* stack) {                                                 \
        at::Tensor t;                                                    \
        c_type other;                                                    \
        pop(stack, t, other);                                            \
        std::move(t) = other; /* NOLINT(bugprone-use-after-move) */      \
        push(stack, std::move(t)); /* NOLINT(bugprone-use-after-move) */ \
      },                                                                 \
      aliasAnalysisFromSchema())

     CREATE_COPY_OP(Tensor, at::Tensor),
     CREATE_COPY_OP(int, int64_t),
     CREATE_COPY_OP(float, double),
#undef CREATE_COPY_OP
     Operator(
         "aten::backward(Tensor self, Tensor? gradient=None, bool? retain_graph=None, bool create_graph=False) -> ()",
         [](Stack* stack) {
           bool create_graph = pop(stack).toBool();
           auto retain_graph = pop(stack).toOptional<bool>();
           IValue gradient_ivalue = pop(stack);
           at::Tensor gradient = gradient_ivalue.isNone()
               ? at::Tensor()
               : gradient_ivalue.toTensor();
           at::Tensor self = pop(stack).toTensor();
           bool keep_graph = retain_graph ? retain_graph.value() : create_graph;
           self.backward(gradient, keep_graph, create_graph);
         },
         aliasAnalysisConservative()),
     //
     // create a clone of these declarations with a _hacked_twin overload name
     // and nullability scrubbed from TensorList arg types
     // TOOD find out why this exists and how to do it without the hack
     //
     Operator(
         "aten::index.Tensor_hacked_twin(Tensor self, Tensor[] indices) -> Tensor",
         [](Stack* stack) {
           auto indices = pop(stack).toTensorVector();
           auto self = pop(stack).toTensor();
           auto result = at::index(self, indices);
           push(stack, std::move(result));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::_index_put_impl_.hacked_twin(Tensor(a!) self, Tensor[] indices, Tensor values, bool accumulate=False, bool unsafe=False) -> Tensor(a!)",
         [](Stack* stack) {
           auto unsafe = pop(stack).toBool();
           auto accumulate = pop(stack).toBool();
           auto values = pop(stack).toTensor();
           auto indices = pop(stack).toTensorVector();
           auto self = pop(stack).toTensor();
           auto result =
               at::_index_put_impl_(self, indices, values, accumulate, unsafe);
           push(stack, std::move(result));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::index_put_.hacked_twin(Tensor(a!) self, Tensor[] indices, Tensor values, bool accumulate=False) -> Tensor(a!)",
         [](Stack* stack) {
           auto accumulate = pop(stack).toBool();
           auto values = pop(stack).toTensor();
           auto indices = pop(stack).toTensorVector();
           auto self = pop(stack).toTensor();
           auto result = at::index_put_(self, indices, values, accumulate);
           push(stack, std::move(result));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::index_put.hacked_twin(Tensor self, Tensor[] indices, Tensor values, bool accumulate=False) -> Tensor",
         [](Stack* stack) {
           auto accumulate = pop(stack).toBool();
           auto values = pop(stack).toTensor();
           auto indices = pop(stack).toTensorVector();
           auto self = pop(stack).toTensor();
           auto result = at::index_put_(self, indices, values, accumulate);
           push(stack, std::move(result));
         },
         aliasAnalysisFromSchema()),
     // reference function parse_to_conversion in python_arg_parsing.h
     Operator(
         "aten::to.prim_Device(Tensor(a) self, Device? device, int? dtype=None, bool non_blocking=False, bool copy=False) -> Tensor(a|b)",
         [](Stack* stack) {
           bool non_blocking;
           bool copy;
           pop(stack, non_blocking, copy);
           c10::optional<at::ScalarType> scalarType =
               pop(stack).toOptional<at::ScalarType>();
           c10::optional<c10::Device> device =
               pop(stack).toOptional<c10::Device>();
           at::Tensor self = pop(stack).toTensor();
           push(
               stack,
               to_dispatch(self, device, scalarType, non_blocking, copy));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "aten::to.prim_dtype(Tensor(a) self, int? dtype=None, bool non_blocking=False, bool copy=False) -> Tensor(a|b)",
         [](Stack* stack) {
           bool non_blocking;
           bool copy;
           pop(stack, non_blocking, copy);
           c10::optional<at::ScalarType> scalarType =
               pop(stack).toOptional<at::ScalarType>();
           c10::optional<c10::Device> device = c10::nullopt;
           at::Tensor self = pop(stack).toTensor();
           push(
               stack,
               to_dispatch(self, device, scalarType, non_blocking, copy));
         },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::is_cuda(Tensor a) -> bool",
         [](Stack* stack) {
           at::Tensor a;
           pop(stack, a);
           push(stack, a.is_cuda());
         },
         aliasAnalysisFromSchema()),
     Operator(
         "prim::data(Tensor(a) a) -> Tensor(a)",
         [](Stack* stack) {
           at::Tensor a;
           pop(stack, a);
           push(stack, autograd::Variable(a).variable_data());
         },
         aliasAnalysisFromSchema()),
// these ops are not defined for Tensor
#define CREATE_COMPARATOR_LIST_OPS_SPECIALIZED(decl_type, value_type)         \
  Operator(                                                                   \
      "prim::min." decl_type "_list(" decl_type "[] l, " decl_type            \
      "[] r) -> " decl_type "[]",                                             \
      minList<value_type>,                                                    \
      aliasAnalysisFromSchema()),                                             \
      Operator(                                                               \
          "prim::max." decl_type "_list(" decl_type "[] l, " decl_type        \
          "[] r) -> " decl_type "[]",                                         \
          maxList<value_type>,                                                \
          aliasAnalysisFromSchema()),                                         \
      Operator(                                                               \
          "prim::min.self_" decl_type "(" decl_type "[] self) -> " decl_type, \
          listMin<value_type>,                                                \
          aliasAnalysisFromSchema()),                                         \
      Operator(                                                               \
          "prim::max.self_" decl_type "(" decl_type "[] self) -> " decl_type, \
          listMax<value_type>,                                                \
          aliasAnalysisFromSchema()),
     CREATE_COMPARATOR_LIST_OPS_SPECIALIZED("int", int64_t)
         CREATE_COMPARATOR_LIST_OPS_SPECIALIZED("float", double)
             CREATE_COMPARATOR_LIST_OPS_SPECIALIZED("bool", bool)
#undef CREATE_COMPARATOR_LIST_OPS_SPECIALIZED
    });

void dictSetItem(Stack* stack) {
  auto value = pop(stack);
  auto idx = pop(stack);
  auto dict = pop(stack).toGenericDict();
  dict.insert_or_assign(std::move(idx), std::move(value));
}

void dictLen(Stack* stack) {
  auto dict = pop(stack).toGenericDict();
  push(stack, int64_t(dict.size()));
}

void dictValues(Stack* stack) {
  auto dict = pop(stack).toGenericDict();
  auto values = c10::impl::GenericList(dict.valueType());
  for (const auto& entry : dict) {
    values.emplace_back(entry.value());
  }
  push(stack, values);
}

void dictKeys(Stack* stack) {
  auto dict = pop(stack).toGenericDict();
  auto keys = c10::impl::GenericList(dict.keyType());
  for (const auto& entry : dict) {
    keys.emplace_back(entry.key());
  }
  push(stack, keys);
}

void dictIndex(Stack* stack) {
  auto key = pop(stack);
  auto dict = pop(stack).toGenericDict();
  auto value = dict.find(key);
  if (value == dict.end()) {
    AT_ERROR("KeyError: ", key);
  }
  push(stack, value->value());
}

template <bool has_default>
void dictGet(Stack* stack) {
  IValue default_value;
  if (has_default) {
    default_value = pop(stack);
  }
  auto key = pop(stack);
  auto dict = pop(stack).toGenericDict();
  auto value = dict.find(key);
  if (value == dict.end()) {
    push(stack, std::move(default_value));
  } else {
    push(stack, value->value());
  }
}

// If the key is in the dict, return it. Else set it to the default value and
// return that.
void dictSetDefault(Stack* stack) {
  auto default_value = pop(stack);
  auto key = pop(stack);
  auto dict = pop(stack).toGenericDict();
  auto value = dict.find(key);
  if (value == dict.end()) {
    dict.insert(key, default_value);
    push(stack, std::move(default_value));
  } else {
    push(stack, value->value());
  }
}

template <bool has_default>
void dictPop(Stack* stack) {
  IValue default_value;
  if (has_default) {
    default_value = pop(stack);
  }
  auto key = pop(stack);
  auto dict = pop(stack).toGenericDict();
  auto iter = dict.find(key);
  if (iter == dict.end()) {
    if (has_default) {
      push(stack, default_value);
    } else {
      AT_ERROR("KeyError: ", key);
    }
  } else {
    // note: before erase
    push(stack, iter->value());
    auto erase_count = dict.erase(key);
    TORCH_CHECK(
        erase_count == 1, "Expected to erase 1 item, found ", erase_count);
  }
}

void dictDelete(Stack* stack) {
  dictPop<false>(stack);
  // pop pushes an item on the stack but delete does not, so get rid of it
  pop(stack);
}

void dictPopItem(Stack* stack) {
  auto dict = pop(stack).toGenericDict();
  if (dict.size() == 0) {
    AT_ERROR("popitem(): dictionary is empty");
  }
  auto head_item = dict.begin();

  IValue tuple =
      c10::ivalue::Tuple::create({head_item->key(), head_item->value()});
  auto erase_count = dict.erase(head_item->key());
  TORCH_CHECK(
      erase_count == 1, "Expected to erase 1 item, found ", erase_count);
  push(stack, tuple);
}

void dictContains(Stack* stack) {
  auto key = pop(stack);
  auto dict = pop(stack).toGenericDict();
  push(stack, dict.contains(key));
}

void dictClear(Stack* stack) {
  auto dict = pop(stack).toGenericDict();
  dict.clear();
}

void dictUpdate(Stack* stack) {
  auto to_add = pop(stack).toGenericDict();
  auto dict = pop(stack).toGenericDict();

  for (const auto& item : to_add) {
    dict.insert(item.key(), item.value());
  }
}

void dictItems(Stack* stack) {
  auto dict = pop(stack).toGenericDict();
  auto key_type = dict.keyType();
  auto value_type = dict.valueType();
  auto items =
      c10::impl::GenericList(TupleType::create({key_type, value_type}));
  items.reserve(dict.size());
  for (const auto& item : dict) {
    items.emplace_back(c10::ivalue::Tuple::create({item.key(), item.value()}));
  }
  push(stack, std::move(items));
}

void dictCopy(Stack* stack) {
  push(stack, pop(stack).toGenericDict().copy());
}

void dictConstructFromList(Stack* stack) {
  auto input_list = pop(stack);
  auto list = input_list.toList();
  auto tup_type = list.elementType()->expect<TupleType>();
  auto dict = c10::impl::GenericDict(
      tup_type->elements().at(0), tup_type->elements().at(1));
  dict.reserve(list.size());
  for (IValue input : list) {
    const auto tup = input.toTuple()->elements();
    dict.insert_or_assign(tup[0], tup[1]);
  }
  push(stack, dict);
}

#define CREATE_DICT_OPS(key_type)                                            \
  Operator(                                                                  \
      "aten::len.Dict_" key_type "(Dict(" key_type ", t) self) -> int",      \
      dictLen,                                                               \
      aliasAnalysisFromSchema()),                                            \
      Operator(                                                              \
          "aten::keys." key_type "(Dict(" key_type ", t) self) -> " key_type \
          "[](*)",                                                           \
          dictKeys,                                                          \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::values." key_type "(Dict(" key_type ", t) self) -> t[](*)", \
          dictValues,                                                        \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::__getitem__.Dict_" key_type "(Dict(" key_type               \
          ", t) self, " key_type " key) -> t(*)",                            \
          dictIndex,                                                         \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::get." key_type "(Dict(" key_type ", t) self, " key_type     \
          " key) -> t(*)?",                                                  \
          dictGet<false>,                                                    \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::get.default_" key_type "(Dict(" key_type                    \
          ", t) self, " key_type " key, t default_value) -> t(*)",           \
          dictGet<true>,                                                     \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::setdefault." key_type "(Dict(" key_type                     \
          ", t)(a!) self, " key_type                                         \
          "(b -> *) key, t(c -> *) default_value) -> t(*)",                  \
          dictSetDefault,                                                    \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::Delete.Dict_" key_type "(Dict(" key_type                    \
          ", t)(a!) self, " key_type " key) -> ()",                          \
          dictDelete,                                                        \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::pop.Dict_" key_type "(Dict(" key_type                       \
          ", t)(a!) self, " key_type " key) -> t(*)",                        \
          dictPop<false>,                                                    \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::pop.Dict_default_" key_type "(Dict(" key_type               \
          ", t)(a!) self, " key_type " key, t default_value) -> t(*)",       \
          dictPop<true>,                                                     \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::popitem." key_type "(Dict(" key_type                        \
          ", t)(a!) self) -> ((" key_type ", t))",                           \
          dictPopItem,                                                       \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::clear." key_type "(Dict(" key_type ", t)(a!) self) -> ()",  \
          dictClear,                                                         \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::update." key_type "(Dict(" key_type                         \
          ", t)(a!) self, Dict(" key_type ", t)(a!) to_add) -> ()",          \
          dictUpdate,                                                        \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::items." key_type "(Dict(" key_type                          \
          ", t) self) -> ((" key_type ", t)[])",                             \
          dictItems,                                                         \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::copy.Dict_" key_type "(Dict(" key_type                      \
          ", t)(a) self) -> Dict(" key_type ", t)",                          \
          dictCopy,                                                          \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::__contains__." key_type "(Dict(" key_type                   \
          ", t) dict, " key_type " key) -> bool",                            \
          dictContains,                                                      \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::_set_item." key_type "(Dict(" key_type                      \
          ", t)(a!) l, " key_type "(b -> *) idx, t(c -> *) v) -> ()",        \
          dictSetItem,                                                       \
          aliasAnalysisFromSchema()),                                        \
      Operator(                                                              \
          "aten::dict." key_type "((" key_type                               \
          ", tVal)[] inputs) -> Dict(" key_type ", tVal)",                   \
          dictConstructFromList,                                             \
          aliasAnalysisFromSchema())

RegisterOperators reg_dict_ops({
    CREATE_DICT_OPS("str"),
    CREATE_DICT_OPS("int"),
    CREATE_DICT_OPS("bool"),
    CREATE_DICT_OPS("float"),
    CREATE_DICT_OPS("Tensor"),
});

} // namespace
} // namespace jit
} // namespace torch
