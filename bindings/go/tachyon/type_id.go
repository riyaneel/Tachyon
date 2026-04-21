//go:build linux || darwin

package tachyon

// MakeTypeID encodes a route and msgType into a single type_id value.
//
// Use route=0 to preserve v0.3.x behavior: MakeTypeID(0, 42) == 42.
// Values of route >= 1 are reserved for RPC and must not be used on v0.4.0 consumers.
func MakeTypeID(route, msgType uint16) uint32 {
	return uint32(route)<<16 | uint32(msgType)
}

// RouteID extracts the route discriminator from bits [31:16] of typeID.
func RouteID(typeID uint32) uint16 {
	return uint16(typeID >> 16)
}

// MsgType extracts the application message type from bits [15:0] of typeID.
func MsgType(typeID uint32) uint16 {
	return uint16(typeID & 0xFFFF)
}
