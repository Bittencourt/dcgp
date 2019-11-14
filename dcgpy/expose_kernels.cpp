// See: https://docs.scipy.org/doc/numpy/reference/c-api.array.html#importing-the-api
// In every cpp file We need to make sure this is included before everything else,
// with the correct #defines.
#define NO_IMPORT_ARRAY
#define PY_ARRAY_UNIQUE_SYMBOL dcgpy_ARRAY_API
#include "numpy.hpp"

#include <boost/numeric/conversion/cast.hpp>
#include <boost/python.hpp>

#include <sstream>
#include <string>
#include <vector>

#include <dcgp/function.hpp>
#include <dcgp/kernel.hpp>
#include <dcgp/kernel_set.hpp>
#include <dcgp/s11n.hpp>
#include <dcgp/wrapped_functions_s11n_implement.hpp>

#include "common_utils.hpp"
#include "docstrings.hpp"

using namespace dcgp;
using namespace dcgpy;
using namespace audi;
namespace bp = boost::python;

namespace dcgpy
{

// Wrapper around the CPython function to create a bytes object from raw data.
inline bp::object make_bytes(const char *ptr, Py_ssize_t len)
{
    PyObject *retval;
    if (len) {
        retval = PyBytes_FromStringAndSize(ptr, len);
    } else {
        retval = PyBytes_FromStringAndSize(nullptr, 0);
    }
    if (!retval) {
        PyErr_SetString(PyExc_RuntimeError, "unable to create a bytes object: the 'PyBytes_FromStringAndSize()' "
                                            "function returned NULL");
        boost::python::throw_error_already_set();
    }
    return bp::object(bp::handle<>(retval));
}

template <typename T>
struct kernel_pickle_suite : bp::pickle_suite {
    static bp::tuple getstate(const T &k)
    {
        // The idea here is that first we extract a char array
        // into which the kernel has been serialized, then we turn
        // this object into a Python bytes object and return that.
        std::ostringstream oss;
        {
            boost::archive::binary_oarchive oarchive(oss);
            oarchive << k;
        }
        auto s = oss.str();
        // Store the serialized kernel.
        return bp::make_tuple(make_bytes(s.data(), boost::numeric_cast<Py_ssize_t>(s.size())));
    }
    static void setstate(T &k, const bp::tuple &state)
    {
        // Similarly, first we extract a bytes object from the Python state,
        // and then we build a C++ string from it. The string is then used
        // to deserialize the object.
        if (len(state) != 1) {
            PyErr_SetString(PyExc_ValueError, ("the state tuple passed for kernel deserialization "
                                               "must have 1 element, but instead it has "
                                               + std::to_string(len(state)) + " elements")
                                                  .c_str());
            boost::python::throw_error_already_set();
        }

        auto ptr = PyBytes_AsString(bp::object(state[0]).ptr());
        if (!ptr) {
            PyErr_SetString(PyExc_TypeError, "a bytes object is needed to deserialize a kernel");
            boost::python::throw_error_already_set();
        }
        const auto size = len(state[0]);
        std::string s(ptr, ptr + size);
        std::istringstream iss;
        iss.str(s);
        {
            boost::archive::binary_iarchive iarchive(iss);
            iarchive >> k;
        }
    }
};

template <typename T>
void expose_kernel(const std::string &type)
{
    std::string class_name = "kernel_" + type;
    bp::class_<kernel<T>>(class_name.c_str(), "The function defining the generic CGP node", bp::init<>())
        .def("__init__",
             bp::make_constructor(
                 +[](const bp::object &obj1, const bp::object &obj2, const std::string &name) {
                     dcgp::function<T(const std::vector<T> &)> my_function([obj1](const std::vector<T> &x) {
                         T in = bp::extract<T>(obj1(v_to_l(x)));
                         return in;
                     });
                     dcgp::function<std::string(const std::vector<std::string> &)> my_print_function(
                         [obj2](const std::vector<std::string> &x) {
                             std::string in = bp::extract<std::string>(obj2(v_to_l(x)));
                             return in;
                         });
                     return ::new kernel<T>(my_function, my_print_function, name);
                 },
                 bp::default_call_policies(), (bp::arg("callable_f"), bp::arg("callable_s"), bp::arg("name"))),
             kernel_init_doc(type).c_str())
        .def(
            "__call__",
            +[](kernel<T> &instance, const bp::object &in) {
                try {
                    auto v = l_to_v<T>(in);
                    return bp::object(instance(v));
                } catch (...) {
                    PyErr_Clear();
                    auto v = l_to_v<std::string>(in);
                    return bp::object(instance(v));
                }
            })
        .def(
            "__repr__",
            +[](const kernel<T> &instance) -> std::string {
                std::ostringstream oss;
                oss << instance;
                return oss.str();
            })
        .def_pickle(kernel_pickle_suite<kernel<T>>());
    ;
}

template <typename T>
kernel<T> wrap_operator(const kernel_set<T> &ks, typename std::vector<dcgp::kernel<T>>::size_type idx)
{
    return ks[idx];
}

template <typename T>
void expose_kernel_set(std::string type)
{
    std::string class_name = "kernel_set_" + type;
    bp::class_<kernel_set<T>>(class_name.c_str(),
                              "Helper to construct a set of kernel functions from their common name", bp::init<>())
        .def("__init__",
             bp::make_constructor(
                 +[](const bp::object &obj1) {
                     auto a = l_to_v<std::string>(obj1);
                     return ::new kernel_set<T>(a);
                 },
                 bp::default_call_policies(), (bp::arg("kernels"))),
             kernel_set_init_doc(type).c_str())
        .def(
            "__call__", +[](kernel_set<T> &instance) { return v_to_l(instance()); })
        .def(
            "__repr__",
            +[](const kernel_set<T> &instance) -> std::string {
                std::ostringstream oss;
                oss << instance;
                return oss.str();
            })
        .def("push_back", (void (kernel_set<T>::*)(std::string)) & kernel_set<T>::push_back,
             kernel_set_push_back_str_doc().c_str(), bp::arg("kernel_name"))
        .def("push_back", (void (kernel_set<T>::*)(const kernel<T> &)) & kernel_set<T>::push_back,
             kernel_set_push_back_ker_doc(type).c_str(), bp::arg("kernel"))
        .def("__getitem__", &wrap_operator<T>)
        .def_pickle(kernel_pickle_suite<kernel_set<T>>());
}

void expose_kernels()
{
    // double
    expose_kernel<double>("double");
    expose_kernel_set<double>("double");

    // gdual_d
    expose_kernel<gdual_d>("gdual_double");
    expose_kernel_set<gdual_d>("gdual_double");

    // gdual_v
    expose_kernel<gdual_v>("gdual_vdouble");
    expose_kernel_set<gdual_v>("gdual_vdouble");
}
} // namespace dcgpy