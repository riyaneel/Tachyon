# Migration Guide

## v0.3.x -> v0.4.0

### Wire protocol

V0.4.0 introduces `TACHYON_VERSION = 0x03`. The version check at `connect()` is strictly equal from previous
versions. **Both producer and consumer must be rebuilt from the same release tag.**

There is no in-place upgrade path. Restart both sides simultaneously.

### `type_id` encoding

v0.4.0 defines a semantic split of the existing `uint32_t type_id` field:

```text
bits [31:16] route_id - reserved for RPC
bits [15:0] msg_type - application-defined discriminator
```

The wire layout of `MessageHeader` is unchanged. No struct field was added or moved. The split is a convention, not a
structural change.

* **`route_id = 0` exactly preserves v0.3.x semantics.** A `type_id` value of `42` in v0.3.x is equivalent to
  `TACHYON_TYPE_ID(0, 42)` in v0.4.0. If your code does not use values above `0xFFFF`, no change is required. If it
  does,
  the high 16 bits were previously application-defined and are now reserved. Audit any `type_id` values that use bits
  `[31:16]` before upgrading.

Helper macros and functions are available in every binding. Use them instead of raw bit manipulation.

* **`route_id = 1` is reserved.** Values in bits `[31:16]` other than zero are undefined behavior on v0.4.0 consumers.
  They are reserved for the RPC introduced in v0.5.0.

### Changes per language

#### C++

```c++
#include <tachyon.h>

// v0.3.x
tachyon_commit_tx(bus, size, 42);

// v0.4.0
tachyon_commit_tx(bus, size, TACHYON_TYPE_ID(0, 42));

// reading
uint32_t type_id = 0;
tachyon_acquire_rx_blocking(bus, &type_id, &sz, 10000);
uint16_t route = TACHYON_ROUTE_ID(type_id); // 0
uint16_t msg = TACHYON_MSG_TYPE(type_id); // 42
```

#### Python

```python
# v0.3.x
bus.send(data, type_id=42)

# v0.4.0
from tachyon import make_type_id

bus.send(data, type_id=make_type_id(0, 42))

# reading
from tachyon import route_id, msg_type

route = route_id(msg.type_id) # 0
mt = msg_type(msg.type_id) # 42
```

#### Rust

```rust
use tachyon_ipc::{make_type_id, route_id, msg_type};

// v0.3.x
bus.send(data, 42)?;

// v0.4.0
bus.send(data, make_type_id(0, 42))?;

// reading
let guard = bus.acquire_rx(10_000)?;
let route = route_id(guard.type_id); // 0
let mt = msg_type(guard.type_id); // 42
```

#### Go

```go
import "github.com/riyaneel/tachyon/bindings/go/tachyon"

// v0.3.x
bus.Send(data, 42)

// v0.4.0
bus.Send(data, tachyon.MakeTypeID(0, 42))

// reading
batch, _ := bus.DrainBatch(64, 10_000)
for msg := range batch.Iter() {
    route := tachyon.RouteID(msg.TypeID()) // 0
    mt := tachyon.MsgType(msg.TypeID()) // 42
}
batch.Commit()
```

#### Java

```java
import dev.tachyon_ipc.TypeId;

// v0.3.x
tx.commit(size, 42);

// v0.4.0
tx.commit(size, TypeId.of(0, 42));

// reading
int typeId = rx.getTypeId();
int route = TypeId.routeId(typeId); // 0
int mt = TypeId.msgType(typeId); // 42
```

#### Kotlin

```kotlin
import dev.tachyon_ipc.TypeId

// v0.3.x
tx.commit(size.toLong(), 42)

// v0.4.0
tx.commit(size.toLong(), TypeId.of(0, 42))

// reading
val route = TypeId.routeId(rx.typeId) // 0
val mt = TypeId.msgType(rx.typeId) // 42
```

#### Node.js

```typescript
import { makeTypeId, routeId, msgType } from '@tachyon-ipc/core';

// v0.3.x
tx.commit(size, 42);

// v0.4.0
tx.commit(size, makeTypeId(0, 42));

// reading
const route = routeId(rx.typeId); // 0
const mt = msgType(rx.typeId); // 42
```
