#include <cstring>
#include <string>
#include <vector>

#include <napi.h>

#include <tachyon.h>

static const char *tachyon_error_code_str(const tachyon_error_t err) noexcept {
	switch (err) {
	case TACHYON_ERR_NULL_PTR:
		return "ERR_TACHYON_NULL_PTR";
	case TACHYON_ERR_MEM:
		return "ERR_TACHYON_MEM";
	case TACHYON_ERR_OPEN:
		return "ERR_TACHYON_OPEN";
	case TACHYON_ERR_TRUNCATE:
		return "ERR_TACHYON_TRUNCATE";
	case TACHYON_ERR_CHMOD:
		return "ERR_TACHYON_CHMOD";
	case TACHYON_ERR_SEAL:
		return "ERR_TACHYON_SEAL";
	case TACHYON_ERR_MAP:
		return "ERR_TACHYON_MAP";
	case TACHYON_ERR_INVALID_SZ:
		return "ERR_TACHYON_INVALID_SZ";
	case TACHYON_ERR_FULL:
		return "ERR_TACHYON_FULL";
	case TACHYON_ERR_EMPTY:
		return "ERR_TACHYON_EMPTY";
	case TACHYON_ERR_NETWORK:
		return "ERR_TACHYON_NETWORK";
	case TACHYON_ERR_SYSTEM:
		return "ERR_TACHYON_SYSTEM";
	case TACHYON_ERR_INTERRUPTED:
		return "ERR_TACHYON_INTERRUPTED";
	case TACHYON_ERR_ABI_MISMATCH:
		return "ERR_TACHYON_ABI_MISMATCH";
	default:
		return "ERR_TACHYON_UNKNOWN";
	}
}

static const char *tachyon_error_message(const tachyon_error_t err) noexcept {
	switch (err) {
	case TACHYON_ERR_NULL_PTR:
		return "Internal error: null pointer.";
	case TACHYON_ERR_MEM:
		return "Out of memory.";
	case TACHYON_ERR_OPEN:
		return "Failed to open shared memory file descriptor.";
	case TACHYON_ERR_TRUNCATE:
		return "Failed to allocate or truncate shared memory.";
	case TACHYON_ERR_CHMOD:
		return "Insufficient privileges to set shared memory permissions.";
	case TACHYON_ERR_SEAL:
		return "Failed to apply fcntl seals to memory descriptor.";
	case TACHYON_ERR_MAP:
		return "Failed to mmap() shared memory pages.";
	case TACHYON_ERR_INVALID_SZ:
		return "Capacity must be a strictly positive power of two.";
	case TACHYON_ERR_FULL:
		return "SPSC ring buffer is full.";
	case TACHYON_ERR_EMPTY:
		return "No messages available in the ring buffer.";
	case TACHYON_ERR_NETWORK:
		return "Unable to bind, listen, or connect via UNIX socket.";
	case TACHYON_ERR_SYSTEM:
		return "OS-level system error (errno).";
	case TACHYON_ERR_INTERRUPTED:
		return "Blocking call interrupted by a signal (EINTR).";
	case TACHYON_ERR_ABI_MISMATCH:
		return "ABI mismatch: incompatible Tachyon versions or alignment.";
	default:
		return "Unknown Tachyon error.";
	}
}

// Sets a pending JS exception with a .code property.
static void set_tachyon_error(Napi::Env env, const tachyon_error_t err) noexcept {
	const Napi::Error error = Napi::Error::New(env, tachyon_error_message(err));
	error.Value().Set("code", Napi::String::New(env, tachyon_error_code_str(err)));
	error.ThrowAsJavaScriptException();
}

// Prevents V8/Node from calling free() on SHM-backed memory.
static void noop_finalizer(Napi::Env, uint8_t *) noexcept {}

struct AddonData {
	Napi::FunctionReference ctor;
};

class TachyonBusNode : public Napi::ObjectWrap<TachyonBusNode> {
public:
	static Napi::Object Init(const Napi::Env env, Napi::Object exports) {
		Napi::Function func = DefineClass(
			env,
			"TachyonBusNode",
			{
				StaticMethod<&TachyonBusNode::Listen>("listen"),
				StaticMethod<&TachyonBusNode::Connect>("connect"),
				InstanceMethod<&TachyonBusNode::Close>("close"),
				InstanceMethod<&TachyonBusNode::Send>("send"),
				InstanceMethod<&TachyonBusNode::AcquireTx>("acquireTx"),
				InstanceMethod<&TachyonBusNode::CommitTx>("commitTx"),
				InstanceMethod<&TachyonBusNode::CommitTxUnflushed>("commitTxUnflushed"),
				InstanceMethod<&TachyonBusNode::RollbackTx>("rollbackTx"),
				InstanceMethod<&TachyonBusNode::Flush>("flush"),
				InstanceMethod<&TachyonBusNode::AcquireRxBlocking>("acquireRxBlocking"),
				InstanceMethod<&TachyonBusNode::CommitRx>("commitRx"),
				InstanceMethod<&TachyonBusNode::DrainBatch>("drainBatch"),
				InstanceMethod<&TachyonBusNode::CommitBatch>("commitBatch"),
				InstanceMethod<&TachyonBusNode::SetPollingMode>("setPollingMode"),
				InstanceMethod<&TachyonBusNode::SetNumaNode>("setNumaNode"),
				InstanceMethod<&TachyonBusNode::GetState>("getState"),
			}
		);

		auto *data = new AddonData();
		data->ctor = Napi::Persistent(func);
		env.SetInstanceData<AddonData>(data);

		exports.Set("TachyonBusNode", func);
		return exports;
	}

	explicit TachyonBusNode(const Napi::CallbackInfo &info) : Napi::ObjectWrap<TachyonBusNode>(info) {}

	~TachyonBusNode() override {
		if (bus_ != nullptr) {
			tachyon_bus_destroy(bus_);
			bus_ = nullptr;
		}
	}

private:
	tachyon_bus_t								   *bus_{nullptr};
	std::vector<tachyon_msg_view_t>					batch_views_;
	std::vector<Napi::Reference<Napi::ArrayBuffer>> batch_arraybuffers_;

	static Napi::Object make_instance(const Napi::CallbackInfo &info) {
		const auto *data = info.Env().GetInstanceData<AddonData>();
		return data->ctor.New({});
	}

	// Returns false and sets a pending JS exception if bus_ is null.
	// Caller must return env.Undefined() immediately when this returns false.
	bool assert_open(Napi::Env env) const noexcept {
		if (bus_ != nullptr)
			return true;
		Napi::Error::New(env, "TachyonBus is closed or not initialized.").ThrowAsJavaScriptException();
		return false;
	}

	static Napi::Value Listen(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();

		if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber()) {
			Napi::TypeError::New(env, "listen(path: string, capacity: number)").ThrowAsJavaScriptException();
			return env.Undefined();
		}

		const std::string path	   = info[0].As<Napi::String>().Utf8Value();
		const size_t	  capacity = static_cast<size_t>(info[1].As<Napi::Number>().Int64Value());

		Napi::Object	obj	 = make_instance(info);
		TachyonBusNode *node = Unwrap(obj);

		tachyon_bus_t		 *bus = nullptr;
		const tachyon_error_t err = tachyon_bus_listen(path.c_str(), capacity, &bus);
		if (err != TACHYON_SUCCESS) {
			set_tachyon_error(env, err);
			return env.Undefined();
		}

		node->bus_ = bus;
		return obj;
	}

	static Napi::Value Connect(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();

		if (info.Length() < 1 || !info[0].IsString()) {
			Napi::TypeError::New(env, "connect(path: string)").ThrowAsJavaScriptException();
			return env.Undefined();
		}

		const std::string path = info[0].As<Napi::String>().Utf8Value();

		Napi::Object	obj	 = make_instance(info);
		TachyonBusNode *node = Unwrap(obj);

		tachyon_bus_t		 *bus = nullptr;
		const tachyon_error_t err = tachyon_bus_connect(path.c_str(), &bus);
		if (err != TACHYON_SUCCESS) {
			set_tachyon_error(env, err);
			return env.Undefined();
		}

		node->bus_ = bus;
		return obj;
	}

	Napi::Value Close(const Napi::CallbackInfo &info) {
		if (bus_ != nullptr) {
			tachyon_bus_destroy(bus_);
			bus_ = nullptr;
		}

		return info.Env().Undefined();
	}

	Napi::Value Send(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		if (info.Length() < 1 || (!info[0].IsBuffer() && !info[0].IsTypedArray())) {
			Napi::TypeError::New(env, "send(data: Buffer | Uint8Array, typeId?: number)").ThrowAsJavaScriptException();
			return env.Undefined();
		}

		const uint8_t *src	= nullptr;
		size_t		   size = 0;

		if (info[0].IsBuffer()) {
			const auto buf = info[0].As<Napi::Buffer<uint8_t>>();
			src			   = buf.Data();
			size		   = buf.ByteLength();
		} else {
			const auto ta = info[0].As<Napi::TypedArray>();
			src			  = static_cast<uint8_t *>(ta.ArrayBuffer().Data()) + ta.ByteOffset();
			size		  = ta.ByteLength();
		}

		const uint32_t type_id =
			(info.Length() >= 2 && info[1].IsNumber()) ? info[1].As<Napi::Number>().Uint32Value() : 0u;

		void *slot = tachyon_acquire_tx(bus_, size);
		if (slot == nullptr) {
			set_tachyon_error(env, TACHYON_ERR_FULL);
			return env.Undefined();
		}

		std::memcpy(slot, src, size);

		const tachyon_error_t err = tachyon_commit_tx(bus_, size, type_id);
		if (err != TACHYON_SUCCESS) {
			set_tachyon_error(env, err);
			return env.Undefined();
		}

		tachyon_flush(bus_);
		return env.Undefined();
	}

	Napi::Value AcquireTx(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		if (info.Length() < 1 || !info[0].IsNumber()) {
			Napi::TypeError::New(env, "acquireTx(maxSize: number)").ThrowAsJavaScriptException();
			return env.Undefined();
		}

		const size_t max_size = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());

		void *slot = tachyon_acquire_tx(bus_, max_size);
		if (slot == nullptr) {
			set_tachyon_error(env, TACHYON_ERR_FULL);
			return env.Undefined();
		}

		return Napi::Buffer<uint8_t>::New(env, static_cast<uint8_t *>(slot), max_size, noop_finalizer);
	}

	Napi::Value CommitTx(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
			Napi::TypeError::New(env, "commitTx(actualSize: number, typeId: number)").ThrowAsJavaScriptException();
			return env.Undefined();
		}

		const size_t   actual_size = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
		const uint32_t type_id	   = info[1].As<Napi::Number>().Uint32Value();

		const tachyon_error_t err = tachyon_commit_tx(bus_, actual_size, type_id);
		if (err != TACHYON_SUCCESS) {
			set_tachyon_error(env, err);
			return env.Undefined();
		}

		tachyon_flush(bus_);
		return env.Undefined();
	}

	Napi::Value CommitTxUnflushed(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
			Napi::TypeError::New(env, "commitTxUnflushed(actualSize: number, typeId: number)")
				.ThrowAsJavaScriptException();
			return env.Undefined();
		}

		const size_t   actual_size = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
		const uint32_t type_id	   = info[1].As<Napi::Number>().Uint32Value();

		const tachyon_error_t err = tachyon_commit_tx(bus_, actual_size, type_id);
		if (err != TACHYON_SUCCESS) {
			set_tachyon_error(env, err);
			return env.Undefined();
		}

		return env.Undefined();
	}

	Napi::Value RollbackTx(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		const tachyon_error_t err = tachyon_rollback_tx(bus_);
		if (err != TACHYON_SUCCESS) {
			set_tachyon_error(env, err);
			return env.Undefined();
		}

		return env.Undefined();
	}

	Napi::Value Flush(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();
		tachyon_flush(bus_);
		return env.Undefined();
	}

	// Returns { data: Buffer, typeId: number, actualSize: number } or null on EINTR.
	// Buffer is zero-copy, backed directly by SHM with a noop finalizer.
	Napi::Value AcquireRxBlocking(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		const uint32_t spin_threshold =
			(info.Length() >= 1 && info[0].IsNumber()) ? info[0].As<Napi::Number>().Uint32Value() : 10000u;

		uint32_t	type_id		= 0;
		size_t		actual_size = 0;
		const void *ptr			= tachyon_acquire_rx_blocking(bus_, &type_id, &actual_size, spin_threshold);

		if (ptr == nullptr)
			return env.Null();

		Napi::Object result = Napi::Object::New(env);
		result.Set(
			"data",
			Napi::Buffer<uint8_t>::New(
				env, const_cast<uint8_t *>(static_cast<const uint8_t *>(ptr)), actual_size, noop_finalizer
			)
		);
		result.Set("typeId", Napi::Number::New(env, type_id));
		result.Set("actualSize", Napi::Number::New(env, static_cast<double>(actual_size)));

		return result;
	}

	Napi::Value CommitRx(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		const tachyon_error_t err = tachyon_commit_rx(bus_);
		if (err != TACHYON_SUCCESS) {
			set_tachyon_error(env, err);
			return env.Undefined();
		}

		return env.Undefined();
	}

	// Returns Array<{ data: Buffer, typeId: number, size: number }>.
	// Each Buffer is SHM-backed (noop finalizer). The underlying ArrayBuffer
	// reference is stored in batch_arraybuffers_ and detached by CommitBatch.
	Napi::Value DrainBatch(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		if (info.Length() < 1 || !info[0].IsNumber()) {
			Napi::TypeError::New(env, "drainBatch(maxMsgs: number, spinThreshold?: number)")
				.ThrowAsJavaScriptException();
			return env.Undefined();
		}

		const size_t   max_msgs = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
		const uint32_t spin_threshold =
			(info.Length() >= 2 && info[1].IsNumber()) ? info[1].As<Napi::Number>().Uint32Value() : 10000u;

		batch_views_.resize(max_msgs);

		const size_t count = tachyon_drain_batch(bus_, batch_views_.data(), max_msgs, spin_threshold);
		batch_views_.resize(count);
		batch_arraybuffers_.clear();
		batch_arraybuffers_.reserve(count);

		Napi::Array result = Napi::Array::New(env, count);

		for (size_t i = 0; i < count; ++i) {
			const tachyon_msg_view_t &v = batch_views_[i];

			Napi::Buffer<uint8_t> buf = Napi::Buffer<uint8_t>::New(
				env, const_cast<uint8_t *>(static_cast<const uint8_t *>(v.ptr)), v.actual_size, noop_finalizer
			);

			batch_arraybuffers_.push_back(Napi::Reference<Napi::ArrayBuffer>::New(buf.ArrayBuffer(), 1));

			Napi::Object msg = Napi::Object::New(env);
			msg.Set("data", buf);
			msg.Set("typeId", Napi::Number::New(env, v.type_id));
			msg.Set("size", Napi::Number::New(env, static_cast<double>(v.actual_size)));

			result.Set(static_cast<uint32_t>(i), msg);
		}

		return result;
	}

	// Advances the consumer head, then detaches every ArrayBuffer from the
	// previous DrainBatch. Any cached JS reference will immediately throw
	// TypeError explicit fail-fast, no SHM writes, no MESI bouncing.
	Napi::Value CommitBatch(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		if (batch_views_.empty())
			return env.Undefined();

		const tachyon_error_t err = tachyon_commit_rx_batch(bus_, batch_views_.data(), batch_views_.size());
		batch_views_.clear();

		for (auto &ref : batch_arraybuffers_) {
			ref.Value().Detach();
			ref.Reset();
		}
		batch_arraybuffers_.clear();

		if (err != TACHYON_SUCCESS) {
			set_tachyon_error(env, err);
			return env.Undefined();
		}

		return env.Undefined();
	}

	Napi::Value SetPollingMode(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		if (info.Length() < 1 || !info[0].IsNumber()) {
			Napi::TypeError::New(env, "setPollingMode(spinMode: 0 | 1)").ThrowAsJavaScriptException();
			return env.Undefined();
		}

		tachyon_bus_set_polling_mode(bus_, info[0].As<Napi::Number>().Int32Value());
		return env.Undefined();
	}

	Napi::Value SetNumaNode(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		if (info.Length() < 1 || !info[0].IsNumber()) {
			Napi::TypeError::New(env, "setNumaNode(nodeId: number)").ThrowAsJavaScriptException();
			return env.Undefined();
		}

		const tachyon_error_t err = tachyon_bus_set_numa_node(bus_, info[0].As<Napi::Number>().Int32Value());
		if (err != TACHYON_SUCCESS) {
			set_tachyon_error(env, err);
			return env.Undefined();
		}

		return env.Undefined();
	}

	Napi::Value GetState(const Napi::CallbackInfo &info) {
		Napi::Env env = info.Env();
		if (!assert_open(env))
			return env.Undefined();

		return Napi::Number::New(env, static_cast<int>(tachyon_get_state(bus_)));
	}
};

Napi::Object InitModule(const Napi::Env env, const Napi::Object exports) {
	return TachyonBusNode::Init(env, exports);
}

NODE_API_MODULE(tachyon_node, InitModule)
