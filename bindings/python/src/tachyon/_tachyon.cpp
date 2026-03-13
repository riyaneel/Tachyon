#define PY_SSIZE_T_CLEAN

#include <Python.h>

#include <tachyon.h>

/**
 * @brief Base exception class for Tachyon errors
 */
static PyObject *TachyonError;

/**
 * @brief Base exception class for Tachyon errors
 */
static PyObject *PeerDeadError;

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
 * @brief TxGuard context manager object
 */
typedef struct {
	PyObject_HEAD tachyon_bus_t *bus;
	void						*ptr;
	size_t						 actual_size;
	uint32_t					 type_id;
	int							 committed;
	size_t						 max_size;
	Py_ssize_t					 exports;
} TxGuard;

/**
 * @brief RxGuard context manager object
 */
typedef struct {
	PyObject_HEAD tachyon_bus_t *bus;
	const void					*ptr;
	size_t						 actual_size;
	uint32_t					 type_id;
	int							 committed;
	Py_ssize_t					 exports;
} RxGuard;

/**
 * Gets the actual payload size for the current TX transaction
 * @param self The TxGuard instance
 * @return The actual payload size as a Python integer
 */
static PyObject *TxGuard_get_actual_size(const TxGuard *self, void *Py_UNUSED(closure)) {
	return PyLong_FromSize_t(self->actual_size);
}

/**
 * Sets the actual payload size for the current TX transaction
 * @param self The TxGuard instance
 * @param value The new actual payload size as a Python integer
 * @return 0 on success, or -1 if an exception is raised
 */
static int TxGuard_set_actual_size(TxGuard *self, PyObject *value, void *Py_UNUSED(closure)) {
	if (!value) {
		PyErr_SetString(PyExc_TypeError, "Cannot delete actual size.");
		return -1;
	}

	const size_t val = PyLong_AsSize_t(value);
	if (PyErr_Occurred())
		return -1;
	self->actual_size = val;

	return 0;
}

/**
 * Gets the message type ID for the current TX transaction
 * @param self The TxGuard instance
 * @return The message type ID as a Python integer
 */
static PyObject *TxGuard_get_type_id(const TxGuard *self, void *Py_UNUSED(closure)) {
	return PyLong_FromUnsignedLong(self->type_id);
}

/**
 * Sets the message type ID for the current TX transaction
 * @param self The TxGuard instance
 * @param value The new message type ID as a Python integer
 * @return 0 on success, or -1 if an exception is raised
 */
static int TxGuard_set_type_id(TxGuard *self, PyObject *value, void *Py_UNUSED(closure)) {
	if (!value) {
		PyErr_SetString(PyExc_TypeError, "Cannot delete type_id");
		return -1;
	}

	const unsigned long val = PyLong_AsUnsignedLong(value);
	if (PyErr_Occurred())
		return -1;
	self->type_id = static_cast<uint32_t>(val);

	return 0;
}

/**
 * @brief Getters and setters for the TxGuard object
 */
static PyGetSetDef TxGuardGettersSetters[3] = {
	{const_cast<char *>("actual_size"),
	 reinterpret_cast<getter>(TxGuard_get_actual_size),
	 reinterpret_cast<setter>(TxGuard_set_actual_size),
	 const_cast<char *>("Actual payload size"),
	 nullptr},
	{const_cast<char *>("type_id"),
	 reinterpret_cast<getter>(TxGuard_get_type_id),
	 reinterpret_cast<setter>(TxGuard_set_type_id),
	 const_cast<char *>("Message type ID"),
	 nullptr},
	{nullptr, nullptr, nullptr, nullptr, nullptr}
};

/**
 * Enters the TxGuard context manager
 * @param self The TxGuard instance
 * @return The TxGuard instance itself
 */
static PyObject *TxGuard_enter(TxGuard *self, PyObject *Py_UNUSED(ignored)) {
	Py_INCREF(self);
	return reinterpret_cast<PyObject *>(self);
}

/**
 * Exits the TxGuard context manager, committing the TX transaction or rolling back on exception
 * @param self The TxGuard instance
 * @param args Tuple containing the exception type, value, and traceback
 * @return Py_FALSE to propagate exceptions or nullptr on parsing error
 */
static PyObject *TxGuard_exit(TxGuard *self, PyObject *args) {
	PyObject *exc_type, *exc_value, *traceback;
	if (!PyArg_ParseTuple(args, "OOO", &exc_type, &exc_value, &traceback)) {
		return nullptr;
	}

	if (self->exports > 0) {
		if (!self->committed && self->bus != nullptr) {
			tachyon_commit_tx(self->bus, 0, self->type_id);
			self->committed = 1;
			self->ptr		= nullptr;
		}
		PyErr_SetString(
			PyExc_BufferError, "Dangling memoryview detected: release the buffer before exiting the context."
		);

		return nullptr;
	}

	if (!self->committed && self->bus != nullptr) {
		if (exc_type != Py_None) {
			self->actual_size = 0;
		}
		tachyon_commit_tx(self->bus, self->actual_size, self->type_id);
		self->committed = 1;
		self->ptr		= nullptr;
	}

	Py_RETURN_FALSE;
}

/**
 * @brief Array of methods exposed by the TxGuard object
 */
static PyMethodDef TxGuardMethods[3] = {
	{"__enter__", reinterpret_cast<PyCFunction>(TxGuard_enter), METH_NOARGS, "Enter TxContext"},
	{"__exit__", reinterpret_cast<PyCFunction>(TxGuard_exit), METH_VARARGS, "Exit TxContext and commit"},
	{nullptr, nullptr, 0, nullptr}
};

/**
 * Exposes the internal C++ pointer as a writable Python buffer
 * @param self The TxGuard instance
 * @param view The buffer view to fill
 * @param flags Buffer request flags
 * @return 0 on success, -1 on error
 */
static int TxGuard_getbuffer(TxGuard *self, Py_buffer *view, const int flags) {
	if (self->committed != 0 || !self->ptr) {
		PyErr_SetString(PyExc_BufferError, "TxBuffer already commited.");
		return -1;
	}

	const int ret = PyBuffer_FillInfo(
		view, reinterpret_cast<PyObject *>(self), self->ptr, static_cast<Py_ssize_t>(self->max_size), 0, flags
	);
	if (ret == 0)
		self->exports++;

	return ret;
}

/**
 * Releases the buffer view
 * @param self The TxGuard instance
 */
static void TxGuard_releasebuffer(TxGuard *self, Py_buffer *Py_UNUSED(view)) {
	self->exports--;
}

/**
 * @brief Buffer protocol definition for TxGuard
 */
static PyBufferProcs TxGuardBufferProcs = {
	reinterpret_cast<getbufferproc>(TxGuard_getbuffer), reinterpret_cast<releasebufferproc>(TxGuard_releasebuffer)
};

/**
 * @brief Python type definition for TxGuard
 */
static PyTypeObject TxGuardType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/**
 * Gets the actual payload size of the received RX transaction
 * @param self The RxGuard instance
 * @return The actual payload size as a Python integer
 */
static PyObject *RxGuard_get_actual_size(const RxGuard *self, void *Py_UNUSED(closure)) {
	return PyLong_FromSize_t(self->actual_size);
}

/**
 * Gets the message type ID of the received RX transaction
 * @param self The RxGuard instance
 * @return The message type ID as a Python integer
 */
static PyObject *RxGuard_get_type_id(const RxGuard *self, void *Py_UNUSED(closure)) {
	return PyLong_FromUnsignedLong(self->type_id);
}

/**
 * @brief Read-only properties for RxGuard
 */
static PyGetSetDef RxGuardGettersSetters[3] = {
	{const_cast<char *>("actual_size"),
	 reinterpret_cast<getter>(RxGuard_get_actual_size),
	 nullptr,
	 const_cast<char *>("Received payload size"),
	 nullptr},
	{const_cast<char *>("type_id"),
	 reinterpret_cast<getter>(RxGuard_get_type_id),
	 nullptr,
	 const_cast<char *>("Received message type ID"),
	 nullptr},
	{nullptr, nullptr, nullptr, nullptr, nullptr}
};

/**
 * Enters the RxGuard context manager
 * @param self The RxGuard instance
 * @return The RxGuard instance itself
 */
static PyObject *RxGuard_enter(RxGuard *self, PyObject *Py_UNUSED(ignored)) {
	Py_INCREF(self);
	return reinterpret_cast<PyObject *>(self);
}

/**
 * Exits the RxGuard context manager, committing the RX transaction
 * @param self The RxGuard instance
 * @param args Tuple containing the exception type, value, and traceback
 * @return Py_FALSE to propagate exceptions or nullptr on parsing error
 */
static PyObject *RxGuard_exit(RxGuard *self, PyObject *args) {
	if (self->exports > 0) {
		if (!self->committed && self->bus != nullptr) {
			tachyon_commit_rx(self->bus);
			self->committed = 1;
			self->ptr		= nullptr;
		}

		PyErr_SetString(
			PyExc_BufferError, "Dangling memoryview detected: release the buffer before exiting the context."
		);
		return nullptr;
	}

	if (!self->committed && self->bus != nullptr) {
		tachyon_commit_rx(self->bus);
		self->committed = 1;
		self->ptr		= nullptr;
	}

	Py_RETURN_FALSE;
}

/**
 * @brief Array of methods exposed by the RxGuard object
 */
static PyMethodDef RxGuardMethods[3] = {
	{"__enter__", reinterpret_cast<PyCFunction>(RxGuard_enter), METH_NOARGS, "Enter RxContext"},
	{"__exit__", reinterpret_cast<PyCFunction>(RxGuard_exit), METH_VARARGS, "Exit RxContext and commit"},
	{nullptr, nullptr, 0, nullptr}
};

/**
 * Exposes the internal C++ pointer as a read-only Python Buffer
 * @param self The RxGuard instance
 * @param view The buffer view to fill
 * @param flags Buffer request flags
 * @return 0 on success, or -1 on error
 */
static int RxGuard_getbuffer(RxGuard *self, Py_buffer *view, const int flags) {
	if (self->committed != 0 || self->ptr == nullptr) {
		PyErr_SetString(PyExc_BufferError, "Cannot access memory: transaction already committed.");
		return -1;
	}

	const int ret = PyBuffer_FillInfo(
		view,
		reinterpret_cast<PyObject *>(self),
		const_cast<void *>(self->ptr),
		static_cast<Py_ssize_t>(self->actual_size),
		1,
		flags
	);
	if (ret == 0) {
		self->exports++;
	}

	return ret;
}

/**
 * Releases the buffer view
 * @param self The RxGuard instance
 */
static void RxGuard_releasebuffer(RxGuard *self, Py_buffer *Py_UNUSED(view)) {
	self->exports--;
}

/**
 * @brief Buffer protocol definition for RxGuard
 */
static PyBufferProcs RxGuardBufferProcs = {
	reinterpret_cast<getbufferproc>(RxGuard_getbuffer), reinterpret_cast<releasebufferproc>(RxGuard_releasebuffer)
};

/**
 * @brief Python type definition for RxGuard
 */
static PyTypeObject RxGuardType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/**
 * Allocates memory for a new TachyonBus Python object
 * @param type The Python type object
 * @return A pointer to the newly allocated PyObject
 */
static PyObject *TachyonBus_new(PyTypeObject *type, PyObject *Py_UNUSED(args), PyObject *Py_UNUSED(kwds)) {
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
 * Acquires a TX lock on the arena for writing
 * @param self The TachyonBus instance
 * @param args Positional args containing max_payload_size
 * @param kwds Keyword args containing max_payload_size
 * @return TxGuard context manager
 */
static PyObject *TachyonBus_acquire_tx(const TachyonBus *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[] = {const_cast<char *>("max_payload_size"), nullptr};
	Py_ssize_t	 max_payload_size;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "n", kwlist, &max_payload_size)) {
		return nullptr;
	}

	if (self->bus == nullptr) {
		PyErr_SetString(PyExc_RuntimeError, "TachyonBus is not initialized.");
		return nullptr;
	}

	void *ptr;

	Py_BEGIN_ALLOW_THREADS;
	ptr = tachyon_acquire_tx(self->bus, static_cast<size_t>(max_payload_size));
	Py_END_ALLOW_THREADS;

	if (ptr == nullptr) {
		if (tachyon_get_state(self->bus) == TACHYON_STATE_FATAL_ERROR) {
			PyErr_SetString(PeerDeadError, "Peer process is dead or unresponsive (Heartbeat timeout).");
			return nullptr;
		}
		PyErr_SetString(TachyonError, "Failed to acquire TX buffer (bus closed or full).");
		return nullptr;
	}

	TxGuard *guard = PyObject_New(TxGuard, &TxGuardType);
	if (guard == nullptr) {
		tachyon_commit_tx(self->bus, 0, 0);
		return PyErr_NoMemory();
	}

	guard->bus		   = self->bus;
	guard->ptr		   = ptr;
	guard->actual_size = 0;
	guard->type_id	   = 0;
	guard->committed   = 0;
	guard->max_size	   = static_cast<size_t>(max_payload_size);
	guard->exports	   = 0;

	return reinterpret_cast<PyObject *>(guard);
}

/**
 * Acquires an RX lock on the arena for reading (Blocking mode with spin fallback)
 * @param self The TachyonBus instance
 * @param args Positional args containing spin_threshold
 * @param kwds Keyword args containing spin_threshold
 * @return RxGuard context manager
 */
static PyObject *TachyonBus_acquire_rx(const TachyonBus *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[]		= {const_cast<char *>("spin_threshold"), nullptr};
	unsigned int spin_threshold = 10000;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|I", kwlist, &spin_threshold)) {
		return nullptr;
	}

	if (self->bus == nullptr) {
		PyErr_SetString(PyExc_RuntimeError, "TachyonBus is not initialized.");
		return nullptr;
	}

	uint32_t	type_id		= 0;
	size_t		actual_size = 0;
	const void *ptr;

	Py_BEGIN_ALLOW_THREADS;
	ptr = tachyon_acquire_rx_blocking(self->bus, &type_id, &actual_size, spin_threshold);
	Py_END_ALLOW_THREADS;

	if (ptr == nullptr) {
		if (tachyon_get_state(self->bus) == TACHYON_STATE_FATAL_ERROR) {
			PyErr_SetString(PeerDeadError, "Peer process is dead or unresponsive (Heartbeat timeout).");
			return nullptr;
		}
		PyErr_SetString(TachyonError, "Failed to acquire RX buffer (bus closed or fatal error).");
		return nullptr;
	}

	RxGuard *guard = PyObject_New(RxGuard, &RxGuardType);
	if (guard == nullptr) {
		tachyon_commit_rx(self->bus);
		return PyErr_NoMemory();
	}

	guard->bus		   = self->bus;
	guard->ptr		   = ptr;
	guard->actual_size = actual_size;
	guard->type_id	   = type_id;
	guard->committed   = 0;
	guard->exports	   = 0;

	return reinterpret_cast<PyObject *>(guard);
}

/**
 * @brief Array of methods exposed by TachyonBus object
 */
static PyMethodDef TachyonBusMethods[7] = {
	{"listen", reinterpret_cast<PyCFunction>(TachyonBus_listen), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"connect", reinterpret_cast<PyCFunction>(TachyonBus_connect), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"destroy", reinterpret_cast<PyCFunction>(TachyonBus_destroy), METH_NOARGS, nullptr},
	{"flush", reinterpret_cast<PyCFunction>(TachyonBus_flush), METH_NOARGS, nullptr},
	{"acquire_tx", reinterpret_cast<PyCFunction>(TachyonBus_acquire_tx), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"acquire_rx", reinterpret_cast<PyCFunction>(TachyonBus_acquire_rx), METH_VARARGS | METH_KEYWORDS, nullptr},
	{nullptr, nullptr, 0, nullptr}
};

/**
 * @brief Python type definition for TachyonBus
 */
static PyTypeObject TachyonBusType = {PyVarObject_HEAD_INIT(nullptr, 0)};

/**
 * @brief Module execution function called during multiphase initialization
 * @param m The module object
 * @return 0 on success, -1 on error
 */
static int tachyon_exec(PyObject *m) {
	/* Initialize TxGuard Type */
	TxGuardType.tp_name		 = "tachyon.TxGuard";
	TxGuardType.tp_basicsize = sizeof(TxGuard);
	TxGuardType.tp_itemsize	 = 0;
	TxGuardType.tp_flags	 = Py_TPFLAGS_DEFAULT;
	TxGuardType.tp_doc		 = "Tachyon TX Guard Context Manager";
	TxGuardType.tp_methods	 = TxGuardMethods;
	TxGuardType.tp_getset	 = TxGuardGettersSetters;
	TxGuardType.tp_as_buffer = &TxGuardBufferProcs;

	if (PyType_Ready(&TxGuardType) < 0) {
		return -1;
	}

	/* Initialize RxGuard Type */
	RxGuardType.tp_name		 = "tachyon.RxGuard";
	RxGuardType.tp_basicsize = sizeof(RxGuard);
	RxGuardType.tp_itemsize	 = 0;
	RxGuardType.tp_flags	 = Py_TPFLAGS_DEFAULT;
	RxGuardType.tp_doc		 = "Tachyon RX Guard Context Manager";
	RxGuardType.tp_methods	 = RxGuardMethods;
	RxGuardType.tp_getset	 = RxGuardGettersSetters;
	RxGuardType.tp_as_buffer = &RxGuardBufferProcs;

	if (PyType_Ready(&RxGuardType) < 0) {
		return -1;
	}

	/* Initialize TachyonBus Type */
	TachyonBusType.tp_name		= "tachyon.TachyonBus";
	TachyonBusType.tp_basicsize = sizeof(TachyonBus);
	TachyonBusType.tp_itemsize	= 0;
	TachyonBusType.tp_new		= reinterpret_cast<newfunc>(TachyonBus_new);
	TachyonBusType.tp_dealloc	= reinterpret_cast<destructor>(TachyonBus_dealloc);
	TachyonBusType.tp_flags		= Py_TPFLAGS_DEFAULT;
	TachyonBusType.tp_doc		= "Tachyon IPC Bus";
	TachyonBusType.tp_methods	= TachyonBusMethods;

	if (PyType_Ready(&TachyonBusType) < 0) {
		return -1;
	}

	Py_INCREF(&TachyonBusType);
	if (PyModule_AddObject(m, "TachyonBus", reinterpret_cast<PyObject *>(&TachyonBusType)) < 0) {
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	Py_INCREF(&TxGuardType);
	if (PyModule_AddObject(m, "TxGuard", reinterpret_cast<PyObject *>(&TxGuardType)) < 0) {
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	Py_INCREF(&RxGuardType);
	if (PyModule_AddObject(m, "RxGuard", reinterpret_cast<PyObject *>(&RxGuardType)) < 0) {
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	TachyonError = PyErr_NewException("tachyon.TachyonError", nullptr, nullptr);
	if (!TachyonError) {
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	Py_INCREF(TachyonError);
	if (PyModule_AddObject(m, "TachyonError", TachyonError) < 0) {
		Py_DECREF(TachyonError);
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	/* Initialize and add PeerDeadError inheriting from TachyonError */
	PeerDeadError = PyErr_NewException("tachyon.PeerDeadError", TachyonError, nullptr);
	if (!PeerDeadError) {
		Py_DECREF(TachyonError);
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	Py_INCREF(PeerDeadError);
	if (PyModule_AddObject(m, "PeerDeadError", PeerDeadError) < 0) {
		Py_DECREF(PeerDeadError);
		Py_DECREF(TachyonError);
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	return 0;
}

/**
 * @brief Module slots definitions for multiphase initialization
 */
static PyModuleDef_Slot TachyonSlots[] = {
	{Py_mod_exec, reinterpret_cast<void *>(tachyon_exec)},
#ifdef Py_mod_multiple_interpreters
	/* Support for sub-interpreters (PEP 684) */
	{Py_mod_multiple_interpreters, (Py_MOD_PER_INTERPRETER_GIL_SUPPORTED)},
#endif
#ifdef Py_GIL_DISABLED
	/* Explicitly declare this module does not use the GIL (PEP 703) */
	{Py_mod_gil, reinterpret_cast<void *>(Py_MOD_GIL_NOT_USED)},
#endif
	{0, nullptr}
};

/**
 * @brief Module definition using PEP 489
 */
static PyModuleDef TachyonModule = {
	PyModuleDef_HEAD_INIT,
	"tachyon._tachyon",
	"Tachyon IPC Bindings",
	0,
	nullptr,
	TachyonSlots,
	nullptr,
	nullptr,
	nullptr
};

PyMODINIT_FUNC PyInit__tachyon(void) {
	return PyModuleDef_Init(&TachyonModule);
}
