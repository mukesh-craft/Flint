#include <Python.h>
#include <stdint.h>

void flint_py_init(void) {
    if (!Py_IsInitialized()) {
        Py_Initialize();
    }
}

void flint_py_run(const char* code) {
    if (!code || !Py_IsInitialized()) return;
    PyRun_SimpleString(code);
}

void flint_py_fini(void) {
    if (Py_IsInitialized()) {
        Py_Finalize();
    }
}

int64_t flint_py_eval_int(const char* expr) {
    if (!expr || !Py_IsInitialized()) return 0;

    PyObject* main = PyImport_AddModule("__main__");   // borrowed ref
    if (!main) { PyErr_Print(); return 0; }
    PyObject* dict = PyModule_GetDict(main);           // borrowed ref
    if (!dict) { PyErr_Print(); return 0; }

    PyObject* result = PyRun_String(expr, Py_eval_input, dict, NULL); // new ref
    if (!result) {
        PyErr_Print();
        return 0;
    }

    int64_t val = PyLong_AsLongLong(result);
    // PyLong_AsLongLong returns -1 and sets an exception on error/overflow;
    // clear it so it doesn't spuriously trip later CPython calls.
    if (val == -1 && PyErr_Occurred()) {
        PyErr_Print();
        Py_DECREF(result);
        return 0;
    }
    Py_DECREF(result);
    return val;
}

// Python's `/` returns a float even for integers (3.5 for 7/2), so an
// integer-only py_eval made almost every arithmetic expression an error.
// This returns a double, accepting either a float or an int result so that
// `py_eval_float("2 + 3")` and `py_eval_float("7 / 2")` both work.
double flint_py_eval_float(const char* expr) {
    if (!expr || !Py_IsInitialized()) return 0.0;

    PyObject* main = PyImport_AddModule("__main__");   // borrowed ref
    if (!main) { PyErr_Print(); return 0.0; }
    PyObject* dict = PyModule_GetDict(main);           // borrowed ref
    if (!dict) { PyErr_Print(); return 0.0; }

    PyObject* result = PyRun_String(expr, Py_eval_input, dict, NULL); // new ref
    if (!result) { PyErr_Print(); return 0.0; }

    double value;
    if (PyFloat_Check(result)) {
        value = PyFloat_AsDouble(result);
    } else if (PyLong_Check(result)) {
        value = (double)PyLong_AsLongLong(result);
    } else {
        PyErr_SetString(PyExc_TypeError, "py_eval_float needs a numeric result");
        PyErr_Print();
        Py_DECREF(result);
        return 0.0;
    }
    if (PyErr_Occurred()) {   // overflow from PyFloat_AsDouble or the int cast
        PyErr_Print();
        Py_DECREF(result);
        return 0.0;
    }
    Py_DECREF(result);
    return value;
}
