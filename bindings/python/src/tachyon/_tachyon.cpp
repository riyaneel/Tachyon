#define PY_SSIZE_T_CLEAN

#include <Python.h>

static PyMethodDef TachyonMethods[] = {{nullptr, nullptr, 0, nullptr}};

static PyModuleDef tachyon_module = {
	PyModuleDef_HEAD_INIT,
	"tachyon._tachyon",
	"Tachyon IPC Bindings",
	-1,
	TachyonMethods,
	nullptr,
	nullptr,
	nullptr,
	nullptr
};

PyMODINIT_FUNC PyInit__tachyon(void) {
	PyObject *m = PyModule_Create(&tachyon_module);
	if (!m) {
		return nullptr;
	}

#ifdef Py_GIL_DISABLED
	PyUnstable_Module_SetGIL(m, Py_MOD_GIL_NOT_USED);
#endif // #ifdef Py_GIL_DISABLED

	return m;
}
