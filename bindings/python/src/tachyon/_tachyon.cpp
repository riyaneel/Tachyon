#define PY_SSIZE_T_CLEAN

#include <Python.h>

#include <tachyon.h>

/**
 * @brief Base exception class for Tachyon errors
 */
static PyObject *TachyonError;

/**
 * Maps C-API error codes to native Python exceptions
 * @param error The Tachyon error code returned by the C-API
 * @return Always return nullptr to signal an exception to Python
 */
static PyObject *raise_tachyon_error(const tachyon_error_t error) {
	switch (error) {
	case TACHYON_SUCCESS:
		Py_RETURN_NONE; /* Should never be reached in an error path */
	case TACHYON_ERR_NULL_PTR:
		PyErr_SetString(PyExc_ValueError, "Internal error: Invalid null pointer.");
		break;
	case TACHYON_ERR_MEM:
		return PyErr_NoMemory();
	case TACHYON_ERR_OPEN:
		PyErr_SetString(TachyonError, "Failed to open shared memory file descriptor.");
		break;
	case TACHYON_ERR_TRUNCATE:
		PyErr_SetString(TachyonError, "Failed to allocate or truncate shared memory capacity.");
		break;
	case TACHYON_ERR_CHMOD:
		PyErr_SetString(PyExc_PermissionError, "Insufficient privileges to set shared memory permissions.");
		break;
	case TACHYON_ERR_SEAL:
		PyErr_SetString(PyExc_PermissionError, "Failed to apply fcntl seals (F_SEAL_SEAL) to memory descriptor.");
		break;
	case TACHYON_ERR_MAP:
		PyErr_SetString(PyExc_OSError, "Failed to mmap() shared memory pages.");
		break;
	case TACHYON_ERR_INVALID_SZ:
		PyErr_SetString(PyExc_ValueError, "Capacity must be a strictly positive power of two.");
		break;
	case TACHYON_ERR_FULL:
		PyErr_SetString(TachyonError, "SPSC ring buffer capacity exceeded (Anti-Overwrite shield).");
		break;
	case TACHYON_ERR_EMPTY:
		PyErr_SetString(TachyonError, "No messages available in the queue.");
		break;
	case TACHYON_ERR_NETWORK:
		PyErr_SetString(PyExc_ConnectionError, "Unable to bind, listen, or connect via UNIX socket.");
		break;
	case TACHYON_ERR_SYSTEM:
		PyErr_SetFromErrno(PyExc_OSError);
		break;
	default:
		PyErr_Format(TachyonError, "Unknown Tachyon internal error (code: %d).", error);
		break;
	}
	return nullptr;
}

/**
 * @brief Tachyon bus object struct def
 */
typedef struct {
	PyObject_HEAD tachyon_bus_t *bus;
} TachyonBus;

/**
 * Allocates memory for a new TachyonBus Python object
 * @param type The Python type object
 * @return A pointer to the newly allocated PyObject
 */
static PyObject *TachyonBus_new(PyTypeObject *type, PyObject Py_UNUSED(args), PyObject *Py_UNUSED(kwds)) {
	TachyonBus *self = reinterpret_cast<TachyonBus *>(type->tp_alloc(type, 0));
	if (self != nullptr) {
		self->bus = nullptr;
	}
	return reinterpret_cast<PyObject *>(self);
}

/**
 * Frees the TachyonBus object and automatically destroys the underlying C-API bus
 * @param self The TachyonBus object to deallocate
 */
static void TachyonBus_dealloc(TachyonBus *self) {
	if (self->bus != nullptr) {
		tachyon_bus_destroy(self->bus);
		self->bus = nullptr;
	}

	const PyTypeObject *tp = Py_TYPE(self);
	tp->tp_free(reinterpret_cast<PyObject *>(self));
}

/**
 * Formats and initializes a new IPC bus on the specified UNIX socket
 * @param self The TachyonBus instance
 * @param args Positional args containing socket path and capacity
 * @param kwds Keyword args containing socket path and capacity
 * @return Py_None on success, or nullptr if an exception is raised
 */
static PyObject *TachyonBus_listen(TachyonBus *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[] = {const_cast<char *>("socket_path"), const_cast<char *>("capacity"), nullptr};
	const char	*socket_path;
	Py_ssize_t	 capacity;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "sn", kwlist, &socket_path, &capacity)) {
		return nullptr;
	}

	if (self->bus != nullptr) {
		PyErr_SetString(PyExc_RuntimeError, "TachyonBus is already initialized.");
		return nullptr;
	}

	tachyon_error_t err;

	Py_BEGIN_ALLOW_THREADS;
	err = tachyon_bus_listen(socket_path, static_cast<size_t>(capacity), &self->bus);
	Py_END_ALLOW_THREADS;

	if (err != TACHYON_SUCCESS) {
		return raise_tachyon_error(err);
	}

	Py_RETURN_NONE;
}

/**
 * Connects to an existing IPC bus via UNIX socket descriptor
 * @param self The TachyonBus instance
 * @param args Positional args containing the socket path
 * @param kwds Keyword args containing the socket path
 * @return Py_None on success, or nullptr if an exception is raised
 */
static PyObject *TachyonBus_connect(TachyonBus *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[] = {const_cast<char *>("socket_path"), nullptr};
	const char	*socket_path;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &socket_path)) {
		return nullptr;
	}

	if (self->bus != nullptr) {
		PyErr_SetString(PyExc_RuntimeError, "TachyonBus is already initialized.");
		return nullptr;
	}

	tachyon_error_t err;

	Py_BEGIN_ALLOW_THREADS;
	err = tachyon_bus_connect(socket_path, &self->bus);
	Py_END_ALLOW_THREADS;

	if (err != TACHYON_SUCCESS) {
		return raise_tachyon_error(err);
	}

	Py_RETURN_NONE;
}

/**
 * Explicitly unmap shared memory and closes fds
 * @param self The TachyonBus instance
 * @return Py_None
 */
static PyObject *TachyonBus_destroy(TachyonBus *self, PyObject *Py_UNUSED(ignored)) {
	if (self->bus != nullptr) {
		tachyon_bus_destroy(self->bus);
		self->bus = nullptr;
	}

	Py_RETURN_NONE;
}

/**
 * Forcefully flushes pending TX transactions to the consumer
 * @param self The TachyonBus instance
 * @return Py_None on success, or nullptr if the bus is uninitialized
 */
static PyObject *TachyonBus_flush(const TachyonBus *self, PyObject *Py_UNUSED(ignored)) {
	if (self->bus == nullptr) {
		PyErr_SetString(PyExc_RuntimeError, "TachyonBus is not initialized.");
		return nullptr;
	}

	tachyon_flush(self->bus);
	Py_RETURN_NONE;
}

/**
 * @brief Array of methods exposed by TachyonBus object
 */
static PyMethodDef TachyonBusMethods[5] = {
	{"listen", reinterpret_cast<PyCFunction>(TachyonBus_listen), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"connect", reinterpret_cast<PyCFunction>(TachyonBus_connect), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"destroy", reinterpret_cast<PyCFunction>(TachyonBus_destroy), METH_NOARGS, nullptr},
	{"flush", reinterpret_cast<PyCFunction>(TachyonBus_flush), METH_NOARGS, nullptr},
	{nullptr, nullptr, 0, nullptr}
};

/**
 * @brief Python type definition for TachyonBus
 */
static PyTypeObject TachyonBusType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/**
 * @brief Module definition for bindings
 */
static PyModuleDef TachyonModule = {
	PyModuleDef_HEAD_INIT, "tachyon._tachyon", "Tachyon IPC Bindings", -1, nullptr, nullptr, nullptr, nullptr, nullptr
};

PyMODINIT_FUNC PyInit__tachyon(void) {
	TachyonBusType.tp_name		= "tachyon.TachyonBus";
	TachyonBusType.tp_basicsize = sizeof(TachyonBus);
	TachyonBusType.tp_itemsize	= 0;
	TachyonBusType.tp_new		= reinterpret_cast<newfunc>(TachyonBus_new);
	TachyonBusType.tp_dealloc	= reinterpret_cast<destructor>(TachyonBus_dealloc);
	TachyonBusType.tp_flags		= Py_TPFLAGS_DEFAULT;
	TachyonBusType.tp_doc		= "Tachyon IPC Bus";
	TachyonBusType.tp_methods	= TachyonBusMethods;

	if (PyType_Ready(&TachyonBusType) < 0) {
		return nullptr;
	}

	PyObject *m = PyModule_Create(&TachyonModule);
	if (!m) {
		return nullptr;
	}

	/* Support for Free-Threading */
#ifdef Py_GIL_DISABLED
	PyUnstable_Module_SetGIL(m, Py_MOD_GIL_NOT_USED);
#endif // #ifdef Py_GIL_DISABLED

	Py_INCREF(&TachyonBusType);
	if (PyModule_AddObject(m, "TachyonBus", reinterpret_cast<PyObject *>(&TachyonBusType)) < 0) {
		Py_DECREF(&TachyonBusType);
		Py_DECREF(m);
		return nullptr;
	}

	TachyonError = PyErr_NewException("tachyon.TachyonError", nullptr, nullptr);
	if (!TachyonError) {
		Py_DECREF(&TachyonBusType);
		Py_DECREF(m);
		return nullptr;
	}

	Py_INCREF(TachyonError);
	if (PyModule_AddObject(m, "TachyonError", TachyonError) < 0) {
		Py_DECREF(TachyonError);
		Py_DECREF(&TachyonBusType);
		Py_DECREF(m);
		return nullptr;
	}

	return m;
}
