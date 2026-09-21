/**
 * Encodes route and msgType into a single type_id value.
 *
 * @param route Route discriminator, bits [31:16]. Values >= 1 are reserved for RPC.
 * @param msgType Application-defined message type, bits [15:0].
 * @return Encoded uint32 type_id.
 */
export function makeTypeId(route: number, msgType: number): number {
	return ((route & 0xffff) << 16) | (msgType & 0xffff);
}

/**
 * Extracts the route_id from type_id.
 *
 * @param typeId Encoded type id value.
 * @return Route discriminator.
 */
export function routeId(typeId: number): number {
	return (typeId >>> 16) & 0xffff;
}

/**
 * Extracts the msg_type from type_id.
 *
 * @param typeId Encoded type id value.
 * @return Application message type.
 */
export function msgType(typeId: number): number {
	return typeId & 0xffff;
}
