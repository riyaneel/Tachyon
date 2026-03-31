#define PY_SSIZE_T_CLEAN

#include <new>

#include <Python.h>
#include <dlpack/dlpack.h>

#include <tachyon.h>

/**
 * @brief Base exception class for Tachyon errors
 */
static PyObject *TachyonError;

/**
 * @brief Raised when the peer process is dead or unresponsive
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
	case TACHYON_ERR_INTERRUPTED:
		PyErr_SetNone(PyExc_KeyboardInterrupt);
		return nullptr;
	case TACHYON_ERR_ABI_MISMATCH:
		PyErr_SetString(
			PyExc_ConnectionError,
			"ABI mismatch: producer and consumer were compiled with incompatible "
			"Tachyon versions or TACHYON_MSG_ALIGNMENT values. Rebuild both sides from the same version."
		);
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

typedef struct {
	PyObject_HEAD tachyon_bus_t *bus;
	tachyon_msg_view_t			*views;
	size_t						 count;
	int							 committed;
	Py_ssize_t					 exports;
} RxBatchGuard;

static PyTypeObject RxBatchGuardType = {PyVarObject_HEAD_INIT(nullptr, 0)};

typedef struct {
	PyObject_HEAD RxBatchGuard *parent_batch;
	const tachyon_msg_view_t   *view;
} RxMsgView;

static PyTypeObject RxMsgViewType = {PyVarObject_HEAD_INIT(nullptr, 0)};

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
			tachyon_rollback_tx(self->bus);
			self->committed = 1;
			self->ptr		= nullptr;
		}

		PyErr_SetString(PyExc_BufferError, "Dangling memoryview detected: release buffer before exiting context.");
		return nullptr;
	}

	if (!self->committed && self->bus != nullptr) {
		if (exc_type != Py_None) {
			tachyon_rollback_tx(self->bus);
		} else {
			tachyon_commit_tx(self->bus, self->actual_size, self->type_id);
		}
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
		PyErr_SetString(PyExc_BufferError, "TxBuffer already committed.");
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

static void TxGuard_dealloc(TxGuard *self) {
	if (!self->committed && self->bus != nullptr) {
		tachyon_rollback_tx(self->bus);
		self->committed = 1;
		self->ptr		= nullptr;
	}
	Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
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
	PyObject *exc_type, *exc_value, *traceback;
	if (!PyArg_ParseTuple(args, "OOO", &exc_type, &exc_value, &traceback)) {
		return nullptr;
	}

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

static void RxGuard_dealloc(RxGuard *self) {
	// Safety net: if __exit__ was never called, commit to release consumer_lock.
	if (!self->committed && self->bus != nullptr) {
		tachyon_commit_rx(self->bus);
		self->committed = 1;
		self->ptr		= nullptr;
	}

	Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
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

static PyObject *RxBatchGuard_enter(RxBatchGuard *self, PyObject *Py_UNUSED(ignore)) {
	Py_INCREF(self);
	return reinterpret_cast<PyObject *>(self);
}

static PyObject *RxBatchGuard_exit(RxBatchGuard *self, PyObject *args) {
	PyObject *exc_type, *exc_value, *traceback;
	if (!PyArg_ParseTuple(args, "OOO", &exc_type, &exc_value, &traceback)) {
		return nullptr;
	}

	if (self->exports > 0) {
		if (!self->committed && self->bus != nullptr) {
			tachyon_commit_rx_batch(self->bus, self->views, self->count);
			self->committed = 1;
		}

		PyErr_SetString(PyExc_BufferError, "Dangling memoryview detected in batch: release before exiting context.");
		return nullptr;
	}

	if (!self->committed && self->bus != nullptr) {
		tachyon_commit_rx_batch(self->bus, self->views, self->count);
		self->committed = 1;
	}

	Py_RETURN_FALSE;
}

static Py_ssize_t RxBatchGuard_len(RxBatchGuard *self) {
	return static_cast<Py_ssize_t>(self->count);
}

static PyObject *RxBatchGuard_getitem(RxBatchGuard *self, Py_ssize_t i) {
	if (i < 0 || i >= static_cast<Py_ssize_t>(self->count)) {
		PyErr_SetString(PyExc_IndexError, "Batch index out of range.");
		return nullptr;
	}

	if (self->committed) {
		PyErr_SetString(PyExc_RuntimeError, "Cannot access messages: batch already committed.");
		return nullptr;
	}

	RxMsgView *msg_view = PyObject_New(RxMsgView, &RxMsgViewType);
	if (!msg_view)
		return PyErr_NoMemory();

	Py_INCREF(self);
	msg_view->parent_batch = self;
	msg_view->view		   = &self->views[i];
	return reinterpret_cast<PyObject *>(msg_view);
}

static void RxBatchGuard_dealloc(RxBatchGuard *self) {
	if (!self->committed && self->bus != nullptr && self->views != nullptr) {
		tachyon_commit_rx_batch(self->bus, self->views, self->count);
		self->committed = 1;
	}

	if (self->views) {
		delete[] self->views;
		self->views = nullptr;
	}

	Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

static PySequenceMethods RxBatchGuardSequenceMethods = {
	reinterpret_cast<lenfunc>(RxBatchGuard_len),
	nullptr,
	nullptr,
	reinterpret_cast<ssizeargfunc>(RxBatchGuard_getitem),
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr
};

static PyMethodDef RxBatchGuardMethods[3] = {
	{"__enter__", reinterpret_cast<PyCFunction>(RxBatchGuard_enter), METH_NOARGS, "Enter RxBatchContext"},
	{"__exit__", reinterpret_cast<PyCFunction>(RxBatchGuard_exit), METH_VARARGS, "Exit RxBatchContext and commit"},
	{nullptr, nullptr, 0, nullptr}
};

static PyObject *RxMsgView_get_actual_size(const RxMsgView *self, void *Py_UNUSED(closure)) {
	return PyLong_FromSize_t(self->view->actual_size);
}

static PyObject *RxMsgView_get_type_id(const RxMsgView *self, void *Py_UNUSED(closure)) {
	return PyLong_FromUnsignedLong(self->view->type_id);
}

static PyGetSetDef RxMsgViewGettersSetters[3]{
	{const_cast<char *>("actual_size"),
	 reinterpret_cast<getter>(RxMsgView_get_actual_size),
	 nullptr,
	 const_cast<char *>("Payload size"),
	 nullptr},
	{const_cast<char *>("type_id"),
	 reinterpret_cast<getter>(RxMsgView_get_type_id),
	 nullptr,
	 const_cast<char *>("Message type ID"),
	 nullptr},
	{nullptr, nullptr, nullptr, nullptr, nullptr}
};

static int RxMsgView_getbuffer(RxMsgView *self, Py_buffer *view, const int flags) {
	if (self->parent_batch->committed != 0) {
		PyErr_SetString(PyExc_BufferError, "Cannot access memory: batch already committed.");
		return -1;
	}

	const int ret = PyBuffer_FillInfo(
		view,
		reinterpret_cast<PyObject *>(self),
		const_cast<void *>(self->view->ptr),
		static_cast<Py_ssize_t>(self->view->actual_size),
		1,
		flags
	);

	if (ret == 0) {
		self->parent_batch->exports++;
	}
	return ret;
}

static void RxMsgView_releasebuffer(RxMsgView *self, Py_buffer *Py_UNUSED(view)) {
	self->parent_batch->exports--;
}

static PyBufferProcs RxMsgViewBufferProcs = {
	reinterpret_cast<getbufferproc>(RxMsgView_getbuffer), reinterpret_cast<releasebufferproc>(RxMsgView_releasebuffer)
};

namespace {
	struct DLManagedTensorOwned {
		DLManagedTensor tensor;
		int64_t			shape[1];
		int64_t			strides[1];
	};
} // namespace

static PyObject *RxMsgView_dlpack_device(RxMsgView *self, PyObject *Py_UNUSED(ignored)) {
	return Py_BuildValue("(ii)", 1, 0); // Return (kDLCPU, 0)
}

static PyObject *RxMsgView_dlpack(RxMsgView *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[] = {const_cast<char *>("stream"), nullptr};
	PyObject	*stream	  = nullptr;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &stream)) {
		return nullptr;
	}

	if (self->parent_batch->committed) {
		PyErr_SetString(PyExc_RuntimeError, "Cannot access memory: batch already committed.");
		return nullptr;
	}

	auto *owned = new (std::nothrow) DLManagedTensorOwned{};
	if (!owned)
		return PyErr_NoMemory();

	DLManagedTensor *dlm = &owned->tensor;

	// Raw Data Mapping
	dlm->dl_tensor.data				  = const_cast<void *>(self->view->ptr);
	dlm->dl_tensor.device.device_type = kDLCPU;
	dlm->dl_tensor.device.device_id	  = 0;
	dlm->dl_tensor.ndim				  = 1;
	dlm->dl_tensor.dtype.code		  = kDLUInt;
	dlm->dl_tensor.dtype.bits		  = 8;
	dlm->dl_tensor.dtype.lanes		  = 1;

	owned->shape[0]		   = static_cast<int64_t>(self->view->actual_size);
	dlm->dl_tensor.shape   = owned->shape;
	owned->strides[0]	   = 1;
	dlm->dl_tensor.strides = owned->strides;

	dlm->dl_tensor.byte_offset = 0;

	dlm->manager_ctx = self->parent_batch;

	dlm->deleter = [](DLManagedTensor *managed_tensor) {
		if (managed_tensor->manager_ctx) {
			auto *parent = static_cast<RxBatchGuard *>(managed_tensor->manager_ctx);
			parent->exports--;
			Py_DECREF(parent);
		}

		delete reinterpret_cast<DLManagedTensorOwned *>(managed_tensor);
	};

	self->parent_batch->exports++;
	Py_INCREF(self->parent_batch);

	return PyCapsule_New(dlm, "dltensor", [](PyObject *capsule) {
		if (PyCapsule_IsValid(capsule, "dltensor")) {
			auto *m = static_cast<DLManagedTensor *>(PyCapsule_GetPointer(capsule, "dltensor"));
			if (m && m->deleter)
				m->deleter(m);
		}
	});
}

static PyMethodDef RxMsgViewMethods[3] = {
	{"__dlpack__", reinterpret_cast<PyCFunction>(RxMsgView_dlpack), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"__dlpack_device__", reinterpret_cast<PyCFunction>(RxMsgView_dlpack_device), METH_NOARGS, "Get DLPack device"},
	{nullptr, nullptr, 0, nullptr}
};

static void RxMsgView_dealloc(RxMsgView *self) {
	Py_XDECREF(reinterpret_cast<PyObject *>(self->parent_batch));
	Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

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
	while (true) {
		Py_BEGIN_ALLOW_THREADS;
		err = tachyon_bus_listen(socket_path, static_cast<size_t>(capacity), &self->bus);
		Py_END_ALLOW_THREADS;

		if (err == TACHYON_ERR_INTERRUPTED) {
			if (PyErr_CheckSignals() != 0) {
				return nullptr;
			}

			continue;
		}

		break;
	}

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
		tachyon_rollback_tx(self->bus);
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
	const void *ptr			= nullptr;

	for (;;) {
		Py_BEGIN_ALLOW_THREADS;
		ptr = tachyon_acquire_rx_blocking(self->bus, &type_id, &actual_size, spin_threshold);
		Py_END_ALLOW_THREADS;

		if (ptr != nullptr) {
			break;
		}

		if (tachyon_get_state(self->bus) == TACHYON_STATE_FATAL_ERROR) {
			PyErr_SetString(PeerDeadError, "Peer process is dead or unresponsive (Heartbeat timeout).");
			return nullptr;
		}

		if (PyErr_CheckSignals() != 0) {
			return nullptr;
		}
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

static PyObject *TachyonBus_drain_batch(TachyonBus *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[]		= {const_cast<char *>("max_msgs"), const_cast<char *>("spin_threshold"), nullptr};
	Py_ssize_t	 max_msgs		= 1024;
	unsigned int spin_threshold = 10000;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nI", kwlist, &max_msgs, &spin_threshold))
		return nullptr;

	if (self->bus == nullptr) {
		PyErr_SetString(PyExc_RuntimeError, "Bus is not initialized.");
		return nullptr;
	}

	if (max_msgs <= 0) {
		PyErr_SetString(PyExc_ValueError, "max_msgs must be > 0");
		return nullptr;
	}

	auto *views = new (std::nothrow) tachyon_msg_view_t[max_msgs];
	if (!views)
		return PyErr_NoMemory();

	size_t s = 0;
	while (true) {
		Py_BEGIN_ALLOW_THREADS s = tachyon_drain_batch(self->bus, views, static_cast<size_t>(max_msgs), spin_threshold);
		Py_END_ALLOW_THREADS

			if (s > 0) break;

		const tachyon_state_t state = tachyon_get_state(self->bus);
		if (state == TACHYON_STATE_FATAL_ERROR) {
			delete[] views;
			PyErr_SetString(PeerDeadError, "Peer process is dead.");
			return nullptr;
		}

		if (PyErr_CheckSignals() != 0) {
			delete[] views;
			return nullptr;
		}
	}

	RxBatchGuard *guard = PyObject_New(RxBatchGuard, &RxBatchGuardType);
	if (!guard) {
		tachyon_commit_rx_batch(self->bus, views, s);
		delete[] views;
		return PyErr_NoMemory();
	}

	guard->bus		 = self->bus;
	guard->views	 = views;
	guard->count	 = s;
	guard->committed = 0;
	guard->exports	 = 0;

	return reinterpret_cast<PyObject *>(guard);
}

/**
 * Binds the shared memory backing this bus to a specific NUMA node.
 * Uses MPOL_PREFERRED policy with MPOL_MF_MOVE to migrate existing pages.
 * No-op on non-Linux platforms (returns None silently).
 * @param self The TachyonBus instance
 * @param args Positional args containing node_id
 * @param kwds Keyword args containing node_id
 * @return Py_None on success, or nullptr if an exception is raised
 */
static PyObject *TachyonBus_set_numa_node(TachyonBus *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[] = {const_cast<char *>("node_id"), nullptr};
	int			 node_id;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "i", kwlist, &node_id)) {
		return nullptr;
	}

	if (self->bus == nullptr) {
		PyErr_SetString(PyExc_RuntimeError, "TachyonBus is not initialized.");
		return nullptr;
	}

	const tachyon_error_t err = tachyon_bus_set_numa_node(self->bus, node_id);
	if (err != TACHYON_SUCCESS) {
		return raise_tachyon_error(err);
	}

	Py_RETURN_NONE;
}

/**
 * @brief Array of methods exposed by TachyonBus object
 */
static PyMethodDef TachyonBusMethods[9] = {
	{"listen", reinterpret_cast<PyCFunction>(TachyonBus_listen), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"connect", reinterpret_cast<PyCFunction>(TachyonBus_connect), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"destroy", reinterpret_cast<PyCFunction>(TachyonBus_destroy), METH_NOARGS, nullptr},
	{"flush", reinterpret_cast<PyCFunction>(TachyonBus_flush), METH_NOARGS, nullptr},
	{"acquire_tx", reinterpret_cast<PyCFunction>(TachyonBus_acquire_tx), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"acquire_rx", reinterpret_cast<PyCFunction>(TachyonBus_acquire_rx), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"drain_batch", reinterpret_cast<PyCFunction>(TachyonBus_drain_batch), METH_VARARGS | METH_KEYWORDS, nullptr},
	{"set_numa_node", reinterpret_cast<PyCFunction>(TachyonBus_set_numa_node), METH_VARARGS | METH_KEYWORDS, nullptr},
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
	TxGuardType.tp_dealloc	 = reinterpret_cast<destructor>(TxGuard_dealloc);
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
	RxGuardType.tp_dealloc	 = reinterpret_cast<destructor>(RxGuard_dealloc);
	RxGuardType.tp_flags	 = Py_TPFLAGS_DEFAULT;
	RxGuardType.tp_doc		 = "Tachyon RX Guard Context Manager";
	RxGuardType.tp_methods	 = RxGuardMethods;
	RxGuardType.tp_getset	 = RxGuardGettersSetters;
	RxGuardType.tp_as_buffer = &RxGuardBufferProcs;

	if (PyType_Ready(&RxGuardType) < 0) {
		return -1;
	}

	/* Initialize RxBatchGuard Type */
	RxBatchGuardType.tp_name		= "tachyon.RxBatchGuard";
	RxBatchGuardType.tp_basicsize	= sizeof(RxBatchGuard);
	RxBatchGuardType.tp_itemsize	= 0;
	RxBatchGuardType.tp_dealloc		= reinterpret_cast<destructor>(RxBatchGuard_dealloc);
	RxBatchGuardType.tp_flags		= Py_TPFLAGS_DEFAULT;
	RxBatchGuardType.tp_doc			= "Tachyon Rx Batch Guard";
	RxBatchGuardType.tp_methods		= RxBatchGuardMethods;
	RxBatchGuardType.tp_as_sequence = &RxBatchGuardSequenceMethods;

	if (PyType_Ready(&RxBatchGuardType) < 0) {
		return -1;
	}

	/* Initialize RxMsgView Type */
	RxMsgViewType.tp_name	   = "tachyon.RxMsgView";
	RxMsgViewType.tp_basicsize = sizeof(RxMsgView);
	RxMsgViewType.tp_itemsize  = 0;
	RxMsgViewType.tp_dealloc   = reinterpret_cast<destructor>(RxMsgView_dealloc);
	RxMsgViewType.tp_flags	   = Py_TPFLAGS_DEFAULT;
	RxMsgViewType.tp_doc	   = "Tachyon Rx Msg View";
	RxMsgViewType.tp_methods   = RxMsgViewMethods;
	RxMsgViewType.tp_getset	   = RxMsgViewGettersSetters;
	RxMsgViewType.tp_as_buffer = &RxMsgViewBufferProcs;

	if (PyType_Ready(&RxMsgViewType) < 0) {
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

	Py_INCREF(&RxBatchGuardType);
	if (PyModule_AddObject(m, "RxBatchGuard", reinterpret_cast<PyObject *>(&RxBatchGuardType)) < 0) {
		Py_DECREF(&RxBatchGuardType);
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	Py_INCREF(&RxMsgViewType);
	if (PyModule_AddObject(m, "RxMsgView", reinterpret_cast<PyObject *>(&RxMsgViewType)) < 0) {
		Py_DECREF(&RxMsgViewType);
		Py_DECREF(&RxBatchGuardType);
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	TachyonError = PyErr_NewException("tachyon.TachyonError", nullptr, nullptr);
	if (!TachyonError) {
		Py_DECREF(&RxMsgViewType);
		Py_DECREF(&RxBatchGuardType);
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	Py_INCREF(TachyonError);
	if (PyModule_AddObject(m, "TachyonError", TachyonError) < 0) {
		Py_DECREF(TachyonError);
		Py_DECREF(&RxMsgViewType);
		Py_DECREF(&RxBatchGuardType);
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	/* Initialize and add PeerDeadError inheriting from TachyonError */
	PeerDeadError = PyErr_NewException("tachyon.PeerDeadError", TachyonError, nullptr);
	if (!PeerDeadError) {
		Py_DECREF(TachyonError);
		Py_DECREF(&RxMsgViewType);
		Py_DECREF(&RxBatchGuardType);
		Py_DECREF(&RxGuardType);
		Py_DECREF(&TxGuardType);
		Py_DECREF(&TachyonBusType);
		return -1;
	}

	Py_INCREF(PeerDeadError);
	if (PyModule_AddObject(m, "PeerDeadError", PeerDeadError) < 0) {
		Py_DECREF(PeerDeadError);
		Py_DECREF(TachyonError);
		Py_DECREF(&RxMsgViewType);
		Py_DECREF(&RxBatchGuardType);
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
